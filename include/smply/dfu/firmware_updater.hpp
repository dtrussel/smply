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
#include <span>
#include <string_view>
#include <variant>
#include <vector>

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
    AwaitingDeviceApply,  ///< Polling get-state until the device runs its images on trial.
    AwaitingConfirmation, ///< `TestThenConfirm` only: waiting for `confirm()`.
    Confirming,           ///< Set-state{confirm}.
    VerifyingConfirmed,   ///< Get-state: is it confirmed?
    AwaitingDeviceCommit, ///< Polling get-state until the device committed its images.
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

/// Who commits an image once it is in place (ADR-0021).
enum class CommitBy : std::uint8_t
{
    /// smply: it is marked for test, verified after the reset, and confirmed
    /// by hash after the confirmation window. The ordinary update.
    Client,

    /// The device: smply stages and marks it, then waits in
    /// `AwaitingDeviceApply` until the device reports it running on trial, and
    /// never confirms it. The device commits it when smply confirms the
    /// `Client` images, and smply waits for that in `AwaitingDeviceCommit`
    /// (ADR-0022). What each state looks like is the device contract in
    /// docs/multi-image.md.
    Device,
};

/// One image of a multi-image update.
struct ImageTarget
{
    /// The image number on the device: 0 is the running application.
    std::uint32_t image = 0;
    /// The firmware file. Not owned; must outlive the update.
    ImageSource* source = nullptr;
    CommitBy commit = CommitBy::Client;
};

/// What to do, and how.
struct UpdatePlan
{
    UpdateMode mode = UpdateMode::TestThenConfirm;

    /// Passed through to `ImageManagement::upload`. `sha` and `server_buf_size`
    /// are filled in by the updater when absent -- it computes the first from
    /// the source and learns the second from the device.
    ///
    /// For the single-image `start()`, `upload.image` is also the image the
    /// rest of the update inspects, marks and confirms: there is one image
    /// number for the whole update, not one for the transfer and another for
    /// everything after it. The image-list `start()` replaces it with each
    /// target's `image`. The confirm names
    /// the image by hash, because a hashless confirm reaches only the device's
    /// running image. Confirming image >= 1 is refused unless the device is
    /// built to allow it (docs/protocol-notes.md section 9, A27).
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

    /// How long to wait, after the reset, for the device to apply every
    /// `CommitBy::Device` image -- and, separately, after the confirm, for it
    /// to commit them. While the link is down the application's reconnect
    /// policy bounds the wait instead, so size that for the same outage. Size it for the slowest
    /// apply: on a coordinating MCU that is a whole image sent to the second MCU over its link,
    /// plus that MCU's reboot (docs/multi-image.md).
    Duration apply_timeout = std::chrono::minutes{5};

    /// How often to read the device's image state while waiting for it to
    /// apply. Must be positive when any image is `CommitBy::Device`.
    Duration apply_poll_interval = std::chrono::seconds{2};
};

/// What an update did to one of its images.
struct ImageReport
{
    std::uint32_t image = 0;
    CommitBy commit = CommitBy::Client;
    /// The image-state hash of this image's file.
    ImageHash target_hash;
    std::uint64_t bytes_transferred = 0;
    /// The device already held the image, so nothing was transferred.
    bool upload_skipped = false;
    /// `Client` only: MCUboot reverted this image to the old one.
    bool rolled_back = false;
    /// `Device` only: the device reported the image applied, running on trial.
    bool applied = false;
    /// `Device` only: the device reported the image committed.
    bool committed = false;
};

/// What an update did, however it ended.
///
/// Every field but `images` sums up all the images of the update, and for a
/// single-image update describes that image.
struct UpdateReport
{
    UpdateState final_state = UpdateState::Idle;
    /// Bytes transferred, over every image.
    std::uint64_t bytes_transferred = 0;
    /// True when every transfer was skipped because the device already held
    /// the image -- by the pre-flight check or by the server's own (rule 9a).
    bool upload_skipped = false;

    /// The image-state hash of the first image's file, once it has been read.
    std::optional<ImageHash> target_hash;
    /// The last slot table read from the device.
    std::optional<ImageState> final_device_state;

    /// Why it failed. Set exactly when `final_state` is `Failed`.
    std::optional<Error> cause;

    /// MCUboot reverted: the device booted the **old** image
    /// (docs/protocol-notes.md section 7), for at least one image.
    bool rolled_back = false;

    /// The device holds a swapped-in image that nobody confirmed, so it will
    /// revert on its next reset.
    ///
    /// Set when an update ends in the confirmation window -- cancelled there,
    /// or refused by the device. It is the difference between "nothing
    /// happened" and "something will happen when this device next restarts",
    /// which a caller must be able to tell apart.
    ///
    /// A multi-image update whose `Device` image was not applied ends here
    /// too: its `Client` images are left unconfirmed, so they revert.
    bool revert_pending = false;

    /// One entry per image, in the order the update was given them.
    std::vector<ImageReport> images;
};

/// The update moved from one state to another.
struct UpdateStateChanged
{
    UpdateState from{};
    UpdateState to{};
};

/// The reset was accepted and the link is about to drop. Stop treating a
/// disconnection as an error.
struct DisconnectExpected
{};

/// Re-establish the link, call `SmpClient::rebind_transport()`, then
/// `FirmwareUpdater::resume_after_reconnect()`.
struct ReconnectRequired
{
    /// How long to wait before the first attempt (`UpdatePlan::reconnect_hint`).
    Duration hint{};
};

/// The device is running the new image, unconfirmed. Validate it and call
/// `confirm()`, or `cancel()` to let it revert (ADR-0014).
struct ConfirmationRequired
{};

/// The update is over. Nothing is emitted after this.
struct UpdateFinished
{
    /// The report on success. On failure, the `Error` that ended the update;
    /// `FirmwareUpdater::report()` still has the full report.
    Result<UpdateReport> result;
};

/// What the updater tells the application: exactly one of these.
///
/// Upload progress arrives as `UploadProgress` itself. A `std::variant`, so a
/// handler cannot read a field that belongs to another kind of event, and a
/// `std::visit` over it fails to compile when a kind is not handled:
/// \code
///     updater.start(source, plan, [&](const UpdateEvent& event) {
///         std::visit(smply::overloaded{
///             [&](const UpdateStateChanged& e) { show(e.to); },
///             [&](const UploadProgress& p) { show(p.transferred, p.total); },
///             [&](const DisconnectExpected&) {},
///             [&](const ReconnectRequired& e) { schedule_reconnect(e.hint); },
///             [&](const ConfirmationRequired&) { validate_then_confirm(); },
///             [&](const UpdateFinished& e) { done(e.result); },
///         }, event);
///     });
/// \endcode
using UpdateEvent = std::variant<UpdateStateChanged, UploadProgress, DisconnectExpected,
                                 ReconnectRequired, ConfirmationRequired, UpdateFinished>;

/// Combines lambdas into one visitor for `std::visit`, as in the `UpdateEvent`
/// example above.
template<class... Handlers>
struct overloaded : Handlers...
{
    using Handlers::operator()...;
};

/// Deduction guide. C++20 deduces this by itself, but not every supported
/// compiler implements that yet (ADR-0001 names the minimum versions).
template<class... Handlers>
overloaded(Handlers...) -> overloaded<Handlers...>;

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

    /// Begins an update of several images of one device, with one reset
    /// (ADR-0021).
    ///
    /// Every image is uploaded and marked for test in the order given, then
    /// the device is reset once. `Client` images must then be running their
    /// target; `Device` images are waited for until the device reports them
    /// applied (docs/multi-image.md). Only then are the `Client` images
    /// confirmed -- after the confirmation window, as for a single image. If a
    /// `Device` image is not applied, nothing is confirmed and the update
    /// fails with `revert_pending`.
    ///
    /// Each target's `image` replaces `plan.upload.image`. The list is copied.
    ///
    /// \return `InvalidArgument` for an empty list, a null source, an image
    ///         number given twice, a `plan.upload.sha` with more than one
    ///         image (it describes one file), or a `Device` image with a
    ///         non-positive `apply_poll_interval`; otherwise as the
    ///         single-image `start()`.
    [[nodiscard]] Result<void> start(std::span<const ImageTarget> targets, const UpdatePlan& plan,
                                     UpdateEventCallback on_event);

    /// Approves the new image after `ConfirmationRequired`.
    ///
    /// The approval is remembered for the rest of the update. If the link or
    /// the device's answer is lost around the confirm, a `ReconnectRequired`
    /// may follow, and the device is read again and, if needed, confirmed
    /// again without a second `ConfirmationRequired` (ADR-0023).
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
