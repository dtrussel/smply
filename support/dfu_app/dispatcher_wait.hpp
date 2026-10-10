// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_DFU_APP_DISPATCHER_WAIT_HPP
#define SMPLY_DFU_APP_DISPATCHER_WAIT_HPP

/// \file
/// The update run's real wait: the steady clock, and a condition variable
/// woken by the application's `Dispatcher`.
///
/// It is the mutex, condition variable and flag every example used to declare
/// beside its pump. The application still owns the `Dispatcher` (several links
/// may share one, docs/handoff.md); it builds it with `waker()` and names it
/// with `deliver_from()`, and this adapter drains it after every wake, on the
/// client context -- which is the application's own drain, not an adapter's.
///
/// \code
/// DispatcherWait wait;
/// Dispatcher inbound{wait.waker()};
/// wait.deliver_from(inbound);
/// \endcode

#include "dfu_app/update_run.hpp"

#include "smply/clock.hpp"
#include "smply/util/dispatcher.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace smply::dfu_app {

class DispatcherWait final : public UpdateWait
{
public:
    /// The longest wait without a deadline. No deadline means the library is
    /// waiting on the application -- during a reboot, say -- and the run must
    /// still wake to check its overall deadline.
    static constexpr Duration kIdleWait{std::chrono::milliseconds{50}};

    DispatcherWait();

    /// The function to build the application's `Dispatcher` with. Callable
    /// from any thread; it signals and returns. It shares the signal it sets,
    /// so it stays safe to call if the dispatcher outlives this adapter.
    [[nodiscard]] std::function<void()> waker() const;

    /// The dispatcher to drain after every wake. Without one, nothing is
    /// drained.
    void deliver_from(Dispatcher& dispatcher) noexcept
    {
        dispatcher_ = &dispatcher;
    }

    /// `std::chrono::steady_clock`.
    [[nodiscard]] TimePoint now() override;

    /// Blocks until \p deadline (at most `kIdleWait` without one) or a wake,
    /// then drains the dispatcher.
    void wait_until(std::optional<TimePoint> deadline) override;

    /// Sleeps for real. Work posted meanwhile waits for the next drain.
    void sleep_for(Duration delay) override;

private:
    struct Signal;

    std::shared_ptr<Signal> signal_;
    Dispatcher* dispatcher_ = nullptr;
};

} // namespace smply::dfu_app

#endif // SMPLY_DFU_APP_DISPATCHER_WAIT_HPP
