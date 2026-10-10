// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TESTS_COMPONENT_SIMULATED_WAIT_HPP
#define SMPLY_TESTS_COMPONENT_SIMULATED_WAIT_HPP

/// \file
/// The update run's wait seam, under `ManualClock`, over the component fixture.
///
/// `dfu_app::UpdateRun` waits through `dfu_app::UpdateWait`. The real adapter
/// blocks on a condition variable and reads the steady clock; this one is the
/// other adapter: a wait is one *device turn* of the simulation. Time moves
/// only here and in `sleep_for()`, so every reconnect delay a test asserts is
/// exact, and no component test reads the real clock (docs/testing.md
/// section 2).
///
/// It is also where a test models the device **between** turns -- a reboot, a
/// link that drops, an answer that goes missing -- never inside a library
/// callback: `between_turns()` registers what the device does, and
/// `when()` registers a one-shot fault that fires once its condition holds.

#include "harness.hpp"

#include "dfu_app/update_run.hpp"

#include "smply/clock.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace smply::test {

class SimulatedWait final : public dfu_app::UpdateWait
{
public:
    /// \param step How far one turn moves the clock: the simulation's
    ///             granularity. A deadline is reached to within one step, as
    ///             it is by `Fixture::run_until()`.
    explicit SimulatedWait(Fixture& fixture, Duration step = std::chrono::milliseconds{10})
        : fixture_{fixture}, step_{step}
    {}

    [[nodiscard]] TimePoint now() override
    {
        return fixture_.clock.now();
    }

    /// One device turn: the clock moves one step, the device's between-turn
    /// behaviour and any due fault run, and the device answers what it was
    /// sent. The run then polls the client and the updater itself.
    void wait_until(std::optional<TimePoint> /*deadline*/) override
    {
        ++turns_;
        fixture_.clock.advance(step_);
        if (between_turns_) {
            between_turns_();
        }
        for (Fault& fault : faults_) {
            if (!fault.fired && fault.condition()) {
                fault.fired = true;
                fault.action();
            }
        }
        fixture_.simulator.pump(fixture_.clock.now());
    }

    /// A reconnect delay: nothing happens on the device's side but time.
    void sleep_for(Duration delay) override
    {
        sleeps_.push_back(delay);
        fixture_.clock.advance(delay);
    }

    /// What the device does on every turn, before it answers.
    void between_turns(std::function<void()> behaviour)
    {
        between_turns_ = std::move(behaviour);
    }

    /// Runs \p action once, on the first turn \p condition holds.
    void when(std::function<bool()> condition, std::function<void()> action)
    {
        faults_.push_back(Fault{std::move(condition), std::move(action), false});
    }

    /// True once every fault registered with `when()` has fired. A test that
    /// injects a fault checks this, so it cannot pass without reaching the
    /// moment it is named after.
    [[nodiscard]] bool all_fired() const noexcept
    {
        for (const Fault& fault : faults_) {
            if (!fault.fired) {
                return false;
            }
        }
        return true;
    }

    /// Every reconnect delay waited, in order.
    [[nodiscard]] const std::vector<Duration>& sleeps() const noexcept
    {
        return sleeps_;
    }

    [[nodiscard]] std::size_t turns() const noexcept
    {
        return turns_;
    }

private:
    struct Fault
    {
        std::function<bool()> condition;
        std::function<void()> action;
        bool fired = false;
    };

    Fixture& fixture_;
    Duration step_;
    std::function<void()> between_turns_;
    std::vector<Fault> faults_;
    std::vector<Duration> sleeps_;
    std::size_t turns_ = 0;
};

} // namespace smply::test

#endif // SMPLY_TESTS_COMPONENT_SIMULATED_WAIT_HPP
