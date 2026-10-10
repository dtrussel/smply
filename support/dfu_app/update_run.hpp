// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_DFU_APP_UPDATE_RUN_HPP
#define SMPLY_DFU_APP_UPDATE_RUN_HPP

/// \file
/// Drives a started update to its end: the loop every application used to
/// write for itself.
///
/// `FirmwareUpdater` deliberately leaves two jobs to the application --
/// re-establishing the link after the device reboots, and deciding whether the
/// new image is good -- and leaves the pump to it as well, because the core
/// owns no clock and starts no thread (ADR-0004). So every caller wrote the
/// same loop: a set of pending flags, an event visitor that only set them, a
/// turn of deliver, client poll and updater poll, a reconnect episode around
/// `ReconnectPolicy`, a confirm step, and a wait on the earlier of two
/// deadlines. The ordering rules that make that loop correct live here now,
/// once:
///
/// * a hook is **never** called from inside `poll()` or any other library
///   callback: events are noted, and acted on after the poll returns, so a
///   hook may block, sleep and reopen ports;
/// * on `ReconnectRequired`, one `ReconnectPolicy` episode: wait, ask the
///   application for a link, and on success `SmpClient::rebind_transport()`
///   **before** `FirmwareUpdater::resume_after_reconnect()`;
/// * on exhaustion, or when the application gives up, the updater is told
///   (`reconnect_failed()`), or it would wait for ever;
/// * the wait is on the earliest of the client's deadline, the updater's, and
///   the overall one, and it wakes even when there is none, so the overall
///   deadline stays reachable while the library waits on the application.
///
/// The application keeps what differs between products: building the client,
/// the groups and the updater, choosing which `start()` to call (passing
/// `event_handler()`), and three hooks -- open a link, approve the image,
/// observe events.
///
/// Support code, not the library (ADR-0016): it is application policy, it
/// runs on the application's client context -- the thread that calls `run()`
/// -- and it is not installed.
///
/// \code
/// DispatcherWait wait;
/// Dispatcher inbound{wait.waker()};
/// wait.deliver_from(inbound);
/// // ... links, then client, groups and updater, as the lifetime rules say ...
/// UpdateRun run{client, updater, wait, settings, UpdateRunHooks{
///     .open_link = [&](const ReconnectAttempt&) { return reopen(); },
///     .approve = [&] { return self_test() ? Approval::Confirm : Approval::Stop; },
///     .observe = [&](const UpdateEvent& event) { print(event); },
/// }};
/// updater.start(source, plan, run.event_handler());
/// const UpdateRunOutcome outcome = run.run();
/// \endcode

#include "dfu_app/reconnect_policy.hpp"

#include "smply/clock.hpp"
#include "smply/dfu/firmware_updater.hpp"
#include "smply/error.hpp"
#include "smply/result.hpp"
#include "smply/smp_client.hpp"
#include "smply/transport.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace smply::dfu_app {

/// The seam the run waits through. Two adapters: `DispatcherWait`, the real
/// one, and the component suite's `SimulatedWait`, under `ManualClock`.
class UpdateWait
{
public:
    UpdateWait() = default;
    UpdateWait(const UpdateWait&) = delete;
    UpdateWait(UpdateWait&&) = delete;
    UpdateWait& operator=(const UpdateWait&) = delete;
    UpdateWait& operator=(UpdateWait&&) = delete;
    virtual ~UpdateWait() = default;

    /// The time to poll the client and the updater with.
    [[nodiscard]] virtual TimePoint now() = 0;

    /// Waits until \p deadline, or until there is something to deliver, then
    /// delivers it. `std::nullopt` means nobody has a deadline: the adapter
    /// still returns after a short while, so the run can check its own.
    virtual void wait_until(std::optional<TimePoint> deadline) = 0;

    /// Waits out a reconnect delay.
    virtual void sleep_for(Duration delay) = 0;
};

/// One attempt of a reconnect episode, as the open-a-link hook is told it.
struct ReconnectAttempt
{
    /// 1 for the first attempt of this episode.
    unsigned number = 0;
    /// The policy's bound: the last attempt has `number == max_attempts`.
    unsigned max_attempts = 0;
    /// The delay waited just before this attempt.
    Duration waited{};
};

/// The open-a-link hook's answer.
class LinkAttempt
{
public:
    /// A fresh link to rebind the client to. The application owns it, and it
    /// must outlive the client (the lifetime rules in docs/handoff.md).
    [[nodiscard]] static LinkAttempt opened(Transport& link)
    {
        return LinkAttempt{&link, false, std::nullopt};
    }

    /// Not yet: try again after the next delay, if the policy allows one.
    [[nodiscard]] static LinkAttempt retry()
    {
        return LinkAttempt{nullptr, false, std::nullopt};
    }

    /// Not yet, and why: if the policy runs out, this is what the updater is
    /// told (a port that is absent, say).
    [[nodiscard]] static LinkAttempt retry(Error why)
    {
        return LinkAttempt{nullptr, false, std::move(why)};
    }

    /// Stop the episode now, and tell the updater \p why (a port that exists
    /// and refuses will not change its mind).
    [[nodiscard]] static LinkAttempt give_up(Error why)
    {
        return LinkAttempt{nullptr, true, std::move(why)};
    }

    /// The link, when one was opened.
    [[nodiscard]] Transport* link() const noexcept
    {
        return link_;
    }

    [[nodiscard]] bool gives_up() const noexcept
    {
        return give_up_;
    }

    [[nodiscard]] const std::optional<Error>& error() const noexcept
    {
        return error_;
    }

private:
    // Not noexcept: an Error carries a reason string.
    LinkAttempt(Transport* link, bool give_up, std::optional<Error> error)
        : link_{link}, give_up_{give_up}, error_{std::move(error)}
    {}

    Transport* link_;
    bool give_up_;
    std::optional<Error> error_;
};

/// The approve hook's answer to `ConfirmationRequired`.
enum class Approval
{
    /// Confirm the new image: the update goes on to make it permanent.
    Confirm,
    /// Do not confirm, and end the run with the update still waiting: the
    /// device stays in its trial boot, which the next reset reverts
    /// (`--stop-before-confirm`). `run()` may be called again later.
    Stop,
};

/// What the application supplies. Every hook runs on the client context, in
/// `run()`, and never inside a library callback.
struct UpdateRunHooks
{
    /// Called once per reconnect attempt, after the policy's delay. It owns
    /// what it opens, may close the old link first, and may block. Empty means
    /// the application cannot reconnect: the episode gives up at once.
    std::function<LinkAttempt(const ReconnectAttempt&)> open_link;

    /// Called on `ConfirmationRequired`. Empty means `Approval::Confirm`.
    std::function<Approval()> approve;

    /// Every `UpdateEvent`, in order, for output only. Optional.
    std::function<void(const UpdateEvent&)> observe;
};

/// How patiently, and for how long.
struct UpdateRunSettings
{
    /// The reconnect patience: the application's decision (ADR-0005).
    ReconnectSettings reconnect{};

    /// An end to the whole run, measured from each `run()` call. Without one,
    /// a run waits as long as the updater does.
    std::optional<Duration> overall_timeout;

    /// What the updater is told when the policy runs out and no attempt said
    /// why (`LinkAttempt::retry()` without an error).
    Error unreachable{ErrorCode::Disconnected, "could not reconnect"};
};

/// How a run ended.
enum class RunEnd
{
    /// The update finished; `UpdateRunOutcome::result` is the updater's own.
    Finished,
    /// The approve hook answered `Approval::Stop`. The update is still
    /// running, in `AwaitingConfirmation`.
    StoppedBeforeConfirm,
    /// The overall deadline passed. The update is still running.
    TimedOut,
};

struct UpdateRunOutcome
{
    RunEnd end = RunEnd::Finished;
    /// For `Finished`, the result `UpdateFinished` carried. Otherwise an error
    /// naming the end: `InvalidState` for a stop, `Timeout` for the deadline.
    Result<UpdateReport> result;
    /// True if a reconnect episode of this run gave up and told the updater.
    bool gave_up_reconnecting = false;
};

/// Runs one started update to its end. Not copyable or movable: the hooks
/// usually capture what is around it.
///
/// Holds the client, the updater and the wait adapter by reference; declare
/// them, and every link, before the run.
class UpdateRun
{
public:
    UpdateRun(SmpClient& client, FirmwareUpdater& updater, UpdateWait& wait,
              UpdateRunSettings settings, UpdateRunHooks hooks);

    UpdateRun(const UpdateRun&) = delete;
    UpdateRun(UpdateRun&&) = delete;
    UpdateRun& operator=(const UpdateRun&) = delete;
    UpdateRun& operator=(UpdateRun&&) = delete;
    ~UpdateRun() = default;

    /// The handler to pass to `FirmwareUpdater::start()`. It only queues the
    /// event; the run acts on it, and forwards it to `observe`, after the poll
    /// that raised it. It holds the queue by shared ownership, so an updater
    /// that outlives the run -- and completes the update in its destructor --
    /// does not reach a dangling run.
    [[nodiscard]] UpdateEventCallback event_handler() const;

    /// Drives the update until it finishes, the approve hook stops it, or the
    /// overall deadline passes. Blocks. After a stop or a timeout, the update
    /// is still running and `run()` may be called again.
    [[nodiscard]] UpdateRunOutcome run();

private:
    /// What one reconnect episode came to.
    enum class Episode
    {
        Rebound,
        GaveUp,
        TimedOut,
    };

    /// Delivers queued events to `observe`, noting the ones the run acts on.
    void deliver();
    [[nodiscard]] Episode reconnect(std::optional<TimePoint> deadline);
    [[nodiscard]] std::optional<TimePoint> next_wake(std::optional<TimePoint> deadline) const;

    SmpClient& client_;
    FirmwareUpdater& updater_;
    UpdateWait& wait_;
    UpdateRunSettings settings_;
    UpdateRunHooks hooks_;
    ReconnectPolicy policy_;
    std::shared_ptr<std::deque<UpdateEvent>> inbox_;

    bool reconnect_pending_ = false;
    bool confirm_pending_ = false;
    std::optional<Result<UpdateReport>> finished_;
};

} // namespace smply::dfu_app

#endif // SMPLY_DFU_APP_UPDATE_RUN_HPP
