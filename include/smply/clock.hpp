// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_CLOCK_HPP
#define SMPLY_CLOCK_HPP

/// \file
/// Time as an injected dependency (ADR-0003).
///
/// The core never reads a clock of its own. Time enters through a `Clock`
/// reference and is only sampled when the application calls `poll(now)`, which
/// is what makes every timeout, retry and late-response path reproducible in
/// tests without sleeping. Tests use `ManualClock`; production uses
/// `system_clock()`.

#include <chrono>

namespace smply {

/// Durations are milliseconds throughout smply. Sub-millisecond precision is
/// meaningless against BLE round-trip times.
using Duration = std::chrono::milliseconds;

/// A point on a monotonic clock. Steady, not wall-clock: deadlines must not be
/// affected by the system clock being adjusted mid-update.
using TimePoint = std::chrono::steady_clock::time_point;

/// A monotonic time source.
///
/// Implementations must be monotonic: successive calls to now() must never
/// return a decreasing value.
class Clock
{
public:
    Clock() = default;
    Clock(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock& operator=(Clock&&) = delete;
    virtual ~Clock() = default;

    [[nodiscard]] virtual TimePoint now() const noexcept = 0;
};

/// The process's steady clock. The default for production use.
///
/// The returned reference has static storage duration and is safe to hold.
[[nodiscard]] const Clock& system_clock() noexcept;

} // namespace smply

#endif // SMPLY_CLOCK_HPP
