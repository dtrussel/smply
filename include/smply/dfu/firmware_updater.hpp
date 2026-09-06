// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_DFU_FIRMWARE_UPDATER_HPP
#define SMPLY_DFU_FIRMWARE_UPDATER_HPP

/// \file
/// The end-to-end firmware update, and the reset/reconnect protocol it needs
/// the application to take part in.
///
/// Everything below this class is a single command or a single transfer.
/// `FirmwareUpdater` is what decides the *order*: query the device's buffer
/// budget, read its slot table, upload, mark the new image for test, reset,
/// wait for the link to come back, check what booted, and confirm.
///
/// **It never touches a connection** (ADR-0004). A reset drops the link by
/// design, and re-establishing it is the application's job: the updater emits
/// `ReconnectRequired`, the application reconnects, calls
/// `SmpClient::rebind_transport()` and then `resume_after_reconnect()`.
///
/// **The default mode does not finish on its own** (ADR-0014). MCUboot reverts
/// an unconfirmed image on the next reset, and that window is the only chance
/// anybody gets to decide the update worked. `TestThenConfirm` stops there and
/// emits `ConfirmationRequired`; the application validates however it likes and
/// calls `confirm()`. An unattended caller wants `ConfirmImmediately`, which
/// runs the same sequence without asking.
///
/// **Threading and lifetime.** As everywhere: one client context, and a
/// callback never runs inside the call that started the operation
/// (ADR-0003). The event callback runs from `poll()` or from `on_bytes()`, so
/// whatever it captures must outlive the client, both groups **and** this
/// updater -- all of which complete outstanding work in their destructors.
/// Declare it first.

#include "smply/bytes.hpp"
#include "smply/clock.hpp"
#include "smply/error.hpp"
#include "smply/groups/image.hpp"
#include "smply/groups/os.hpp"
#include "smply/image_source.hpp"
#include "smply/result.hpp"
#include "smply/smp_client.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace smply {

/// How far to take the update.
enum class UpdateMode : std::uint8_t
{
    /// Upload, test-boot, then **stop and ask** (ADR-0014). The application
    /// decides whether the new image is good and calls `confirm()`. If it never
    /// does, the device reverts on its next reset.
    TestThenConfirm,

    /// The same sequence, confirmed automatically. For an unattended updater,
    /// where nothing is going to validate the new image anyway.
    ConfirmImmediately,

    /// Stop once the device holds the image. Activation is left entirely to the
    /// caller.
    UploadOnly,
};

/// Where an update has got to.
///
/// Exposed because a user interface wants to say what is happening, and because
/// a failure report is far more useful with the state it failed in.
enum class UpdateState : std::uint8_t
{
    Idle,
    QueryingParameters, ///< OS mcumgr-params; `NotSupported` is fine (A8).
    InspectingImages,   ///< Image get-state: what does the device hold?
    Planning,           ///< Decide whether anything needs uploading at all.
    Uploading,
    VerifyingUpload,      ///< Get-state: is the target hash in a slot?
    MarkingForTest,       ///< Set-state{hash}: swap it in on the next boot.
    Resetting,            ///< OS reset. Acceptance, not completion.
    AwaitingDisconnect,   ///< The link should drop; a grace timer bounds the wait.
    AwaitingReconnect,    ///< The application's turn.
    VerifyingBooted,      ///< Get-state: did it boot ours, or revert?
    AwaitingConfirmation, ///< `TestThenConfirm` only: waiting for `confirm()`.
    Confirming,           ///< Set-state{confirm}.
    VerifyingConfirmed,   ///< Get-state: is it confirmed?
    Completed,
    Failed,
    Cancelled,
};

/// A short, stable name for a state. Never allocates.
[[nodiscard]] std::string_view to_string(UpdateState state) noexcept;

/// True for `Completed`, `Failed` and `Cancelled`.
[[nodiscard]] constexpr bool is_terminal(UpdateState state) noexcept
{
    return state == UpdateState::Completed || state == UpdateState::Failed ||
           state == UpdateState::Cancelled;
}

/// What to do, and how.
struct UpdatePlan
{
    UpdateMode mode = UpdateMode::TestThenConfirm;

    /// Which image to update. Zephyr supports two; 0 is the usual one.
    std::uint32_t image = 0;

    /// Passed through to `ImageManagement::upload`. `sha` and `server_buf_size`
    /// are filled in by the updater when absent -- it computes the first from
    /// the source and learns the second from the device.
    UploadOptions upload{};

    /// Skip the transfer when the device already holds this image, recognised
    /// by its MCUboot hash TLV in the slot table already read.
    ///
    /// Turning it off costs one round trip rather than the whole transfer: the
    /// server runs the same check itself on the first packet and answers
    /// "complete" (docs/protocol-notes.md section 6, rule 9a).
    bool skip_if_already_present = true;

    /// How long to wait for the link to drop after a reset is accepted, before
    /// carrying on regardless. The verify step is the real check.
    Duration disconnect_grace = std::chrono::seconds{10};

    /// Passed to the application in `ReconnectRequired`, as a hint about how
    /// long to wait before its first attempt.
    Duration reconnect_hint = std::chrono::seconds{3};
};

/// What an update did, however it ended.
struct UpdateReport
{
    UpdateState final_state = UpdateState::Idle;
    std::uint64_t bytes_transferred = 0;
    /// True when the transfer was skipped because the device already held the
    /// image -- by the pre-flight check or by the server's own (rule 9a).
    bool upload_skipped = false;

    /// The image-state hash of the file, once it has been read.
    std::optional<ImageHash> target_hash;
    /// The last slot table read from the device.
    std::optional<ImageState> final_device_state;

    /// Why it failed. Set exactly when `final_state` is `Failed`.
    std::optional<Error> cause;

    /// MCUboot reverted: the device booted the **old** image
    /// (docs/protocol-notes.md section 7).
    bool rolled_back = false;

    /// The device holds a swapped-in image that nobody confirmed, so it will
    /// revert on its next reset.
    ///
    /// Set when an update ends in the confirmation window -- cancelled there,
    /// or refused by the device. It is the difference between "nothing
    /// happened" and "something will happen when this device next restarts",
    /// which a caller must be able to tell apart.
    bool revert_pending = false;
};

/// What the updater tells the application.
struct UpdateEvent
{
    enum class Kind : std::uint8_t
    {
        StateChanged,
        Progress,
        /// The reset was accepted; the link is about to drop. Stop treating a
        /// disconnection as an error.
        DisconnectExpected,
        /// Re-establish the link, `rebind_transport()`, then
        /// `resume_after_reconnect()`.
        ReconnectRequired,
        /// The device is running the new image, unconfirmed. Validate it and
        /// call `confirm()` -- or `cancel()` to let it revert (ADR-0014).
        ConfirmationRequired,
        Finished,
    };

    Kind kind{};
    /// `StateChanged` only.
    UpdateState from{};
    UpdateState to{};
    /// `Progress` only.
    UploadProgress progress{};
    /// `ReconnectRequired` only.
    Duration reconnect_hint{};
    /// `Finished` only; borrowed for the duration of the callback.
    const Result<UpdateReport>* result = nullptr;
};

/// Invoked for every event, on the client context.
using UpdateEventCallback = std::function<void(const UpdateEvent&)>;

/// Runs one update at a time over an existing client.
///
/// Non-copyable and non-movable: its callbacks capture a pointer to it.
class FirmwareUpdater
{
public:
    /// \param client The client; must outlive this updater, as must \p image
    ///               and \p os.
    FirmwareUpdater(SmpClient& client, ImageManagement& image, OsManagement& os) noexcept;

    FirmwareUpdater(const FirmwareUpdater&) = delete;
    FirmwareUpdater(FirmwareUpdater&&) = delete;
    FirmwareUpdater& operator=(const FirmwareUpdater&) = delete;
    FirmwareUpdater& operator=(FirmwareUpdater&&) = delete;

    /// Completes a running update with `Cancelled` before returning, so no
    /// event can fire afterwards. Like `~SmpClient`, that means a callback runs
    /// during destruction -- see the file comment.
    ~FirmwareUpdater();

    /// Begins an update.
    ///
    /// \param source The firmware file. Must outlive the update.
    /// \return `InvalidState` if an update is already running, or
    ///         `InvalidArgument` for a plan that cannot be honoured. No event
    ///         is emitted from inside this call; the first arrives on the next
    ///         `poll()`.
    [[nodiscard]] Result<void> start(ImageSource& source, const UpdatePlan& plan,
                                     UpdateEventCallback on_event);

    /// Approves the new image after `ConfirmationRequired`.
    ///
    /// \return `InvalidState` unless the update is in `AwaitingConfirmation`.
    [[nodiscard]] Result<void> confirm();

    /// Abandons the update. The callback receives `Cancelled` on the next
    /// `poll()`. Nothing already written to the device is undone -- if a swap
    /// was scheduled, the report says a revert is pending.
    void cancel() noexcept;

    /// Continues after the application has re-established the link and called
    /// `SmpClient::rebind_transport()`.
    ///
    /// \return `InvalidState` unless the update is in `AwaitingReconnect`.
    [[nodiscard]] Result<void> resume_after_reconnect();

    /// Tells the updater the application has given up reconnecting. Terminal.
    void reconnect_failed(Error error);

    /// Drives the updater's own deadlines and deferred work. Call it alongside
    /// `SmpClient::poll()`.
    void poll(TimePoint now);

    /// The earliest point at which `poll()` has something to do, or
    /// `std::nullopt`.
    [[nodiscard]] std::optional<TimePoint> next_deadline() const noexcept;

    [[nodiscard]] UpdateState state() const noexcept;

    /// The report as it stands. Complete once the update is terminal.
    [[nodiscard]] const UpdateReport& report() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace smply

#endif // SMPLY_DFU_FIRMWARE_UPDATER_HPP
