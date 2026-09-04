// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TESTS_MANUAL_CLOCK_HPP
#define SMPLY_TESTS_MANUAL_CLOCK_HPP

/// \file
/// A Clock the test drives by hand.
///
/// No unit or component test may call std::chrono::steady_clock::now(): every
/// timeout, retry and late-response path must be reproducible without sleeping
/// (docs/testing.md section 7).

#include "smply/clock.hpp"

#include <cassert>

namespace smply::test {

class ManualClock final : public Clock
{
public:
    ManualClock() = default;

    explicit ManualClock(TimePoint start) noexcept : now_{start} {}

    [[nodiscard]] TimePoint now() const noexcept override
    {
        return now_;
    }

    /// Moves time forward. Never backwards: Clock promises monotonicity.
    void advance(Duration by) noexcept
    {
        assert(by.count() >= 0);
        now_ += by;
    }

    /// Jumps to an absolute point. Must not move time backwards.
    void set(TimePoint to) noexcept
    {
        assert(to >= now_);
        now_ = to;
    }

private:
    TimePoint now_{};
};

} // namespace smply::test

#endif // SMPLY_TESTS_MANUAL_CLOCK_HPP
