// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TESTS_HIL_SUPPORT_RIG_HPP
#define SMPLY_TESTS_HIL_SUPPORT_RIG_HPP

/// \file
/// An application, for tests: the pump loop of `examples/winrt_ble_dfu/main.cpp`
/// made callable one operation at a time.
///
/// A case says *what* to do to the device -- read its state, upload an image,
/// drop the link, run an update -- and the rig does the part every real
/// application has to do: own the dispatcher and the links in the right order,
/// drain and poll on a real clock, wait on the right deadline, reconnect with a
/// policy. Each blocking helper returns when the operation completes or its
/// deadline passes, and the rig keeps a timeline of what happened when, which
/// the cases print for the supervisor to keep.
///
/// Lifetime order is the whole point of the member layout: the dispatcher
/// outlives every transport, every transport outlives the client, and the
/// client outlives the groups and the updater (smply/transport.hpp,
/// handoff.md "Lifetime").

#include "support/bench.hpp"

#include "dfu_app/reconnect_policy.hpp"
#include "winrt_ble/winrt_ble_transport.hpp"

#include "smply/clock.hpp"
#include "smply/dfu/firmware_updater.hpp"
#include "smply/groups/image.hpp"
#include "smply/groups/os.hpp"
#include "smply/image_source.hpp"
#include "smply/result.hpp"
#include "smply/smp_client.hpp"
#include "smply/util/dispatcher.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace smply::hil {

/// What happened, and when, in milliseconds since the rig was built.
class Timeline
{
public:
    void note(std::string what);
    [[nodiscard]] std::int64_t elapsed_ms() const;
    /// One line per entry, plus `HIL-METRIC name=value` lines for the metrics.
    [[nodiscard]] std::string dump() const;
    void metric(std::string_view name, std::int64_t value);

private:
    struct Entry
    {
        std::int64_t ms;
        std::string what;
    };

    std::chrono::steady_clock::time_point started_{std::chrono::steady_clock::now()};
    std::vector<Entry> entries_;
    std::vector<std::pair<std::string, std::int64_t>> metrics_;
};

/// What a case may hook into a whole update.
struct UpdateHooks
{
    /// Every progress event, before the rig records it.
    std::function<void(const UploadProgress&)> on_progress;
    /// Just before the first reconnect attempt of an episode.
    std::function<void()> on_await_reconnect;
    /// Confirm when asked (the default mode's pause). False leaves the update
    /// parked in `AwaitingConfirmation`, and `update()` returns on its deadline.
    bool auto_confirm = true;
    dfu_app::ReconnectSettings reconnect{};
    Duration deadline{std::chrono::minutes{5}};
};

/// One upload's outcome, with the offsets the device acknowledged along the way.
struct UploadOutcome
{
    Result<UploadResult> result = fail(ErrorCode::InvalidState, "hil: not run");
    UploadHandle handle;
    std::vector<UploadProgress> progress;
};

/// How a case interrupts an upload part-way through.
///
/// The action runs from the pump loop, **between** poll()s, never from the
/// progress callback -- closing the link or cancelling from inside a callback
/// dispatched by poll() re-enters the transport, which is exactly what an
/// application must not do (the examples set a flag and act in the loop). The
/// rig does the same on the case's behalf.
struct UploadInterrupt
{
    enum class Action : std::uint8_t
    {
        None,
        DropLink, ///< The application closes the link (a decision to go away).
        Cancel,   ///< The application cancels the upload.
    };
    Action action = Action::None;
    /// Fire once the device has acknowledged this fraction of the image.
    double at_fraction = 0.5;
};

class Rig
{
public:
    explicit Rig(const Bench& bench);
    Rig(const Rig&) = delete;
    Rig(Rig&&) = delete;
    Rig& operator=(const Rig&) = delete;
    Rig& operator=(Rig&&) = delete;
    ~Rig();

    /// Opens a link (the first, or a replacement the client is rebound to).
    [[nodiscard]] Result<void> connect();
    /// Retries `connect()` on the policy's schedule until it works or the
    /// policy is exhausted.
    [[nodiscard]] Result<void> reconnect(const dfu_app::ReconnectSettings& settings);
    /// Closes the current link without telling the client -- what an
    /// application does when *it* decides to go away. The client learns of it
    /// the way it would of a failed medium: its next send fails.
    void drop_link();
    /// How long the last `drop_link()` took inside `close()`.
    [[nodiscard]] Duration last_close_duration() const noexcept;

    [[nodiscard]] Timeline& timeline() noexcept;
    [[nodiscard]] const SmpClientStats& stats() const noexcept;
    [[nodiscard]] ImageManagement& images() noexcept;
    [[nodiscard]] bool connected() const noexcept;

    // --- one request each, blocking on a real clock --------------------------

    [[nodiscard]] Result<ImageState> read_state(Duration limit = std::chrono::seconds{10});
    [[nodiscard]] Result<ImageState> set_state(const SetStateRequest& request,
                                               Duration limit = std::chrono::seconds{20});
    [[nodiscard]] Result<SlotInfo> slot_info(Duration limit = std::chrono::seconds{10});
    [[nodiscard]] Result<McumgrParameters> params(Duration limit = std::chrono::seconds{10});
    [[nodiscard]] Result<std::string> echo(std::string_view text,
                                           Duration limit = std::chrono::seconds{10});
    [[nodiscard]] Result<void> reset(Duration limit = std::chrono::seconds{10});
    [[nodiscard]] Result<void> erase(std::optional<std::uint32_t> slot,
                                     Duration limit = std::chrono::seconds{90});

    /// True once the client has seen the link drop, or false at the deadline.
    [[nodiscard]] bool wait_disconnected(Duration limit);

    // --- uploads and updates -------------------------------------------------

    [[nodiscard]] UploadOutcome upload(ImageSource& source, const UploadOptions& options,
                                       UploadInterrupt interrupt = {},
                                       Duration limit = std::chrono::minutes{5});
    /// Resumes `outcome.handle` after `reconnect()`; progress is collected into
    /// `outcome.progress` again from empty.
    [[nodiscard]] Result<UploadResult> resume(UploadOutcome& outcome,
                                              Duration limit = std::chrono::minutes{5});
    /// A whole `FirmwareUpdater` run, reconnects and confirmation included.
    [[nodiscard]] Result<UpdateReport> update(ImageSource& source, const UpdatePlan& plan,
                                              const UpdateHooks& hooks);
    /// The states the last `update()` passed through, in order.
    [[nodiscard]] const std::vector<UpdateState>& states() const noexcept;

private:
    template<class T>
    [[nodiscard]] Result<T> await(const std::function<void(Callback<T>)>& issue, Duration limit,
                                  const char* what);

    /// Drain the dispatcher and poll the client (and the updater, if any).
    void pump_step();
    /// Sleep until the next deadline, a wake from a WinRT thread, or `until`,
    /// whichever is first. Always look before sleeping: see pump_until().
    void pump_wait(std::optional<TimePoint> until);

    template<class Pred>
    [[nodiscard]] bool pump_until(Pred&& done, Duration limit);

    void wake_up();

    Bench bench_;
    Timeline timeline_;
    Duration last_close_{0};
    std::vector<UpdateState> states_;
    /// Progress lands here, captured by `this`, then copied into the outcome.
    /// A callback registered by upload() may fire during a later resume(), so
    /// it must not reference a local that has since been returned and destroyed.
    std::vector<UploadProgress> progress_;

    std::mutex wake_mutex_;
    std::condition_variable wake_;
    bool woken_ = false;

    // Declaration order is lifetime order, reversed on destruction.
    Dispatcher inbound_;
    std::vector<std::unique_ptr<transport::WinRtBleTransport>> links_;
    std::optional<SmpClient> client_;
    std::optional<ImageManagement> images_;
    std::optional<OsManagement> os_;
    std::optional<FirmwareUpdater> updater_;
};

/// The MCUboot image-state hash of a file (`IMAGE_TLV_SHA256`), for comparing
/// with what the device reports.
[[nodiscard]] Result<ImageHash> image_hash_of(ImageSource& source);

} // namespace smply::hil

#endif // SMPLY_TESTS_HIL_SUPPORT_RIG_HPP
