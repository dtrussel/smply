// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_DFU_APP_RECONNECT_POLICY_HPP
#define SMPLY_DFU_APP_RECONNECT_POLICY_HPP

/// \file
/// How long to wait before trying the link again, and when to stop trying.
///
/// **This is the application's decision, not the library's** (ADR-0005). smply
/// tells the application the link is needed again -- `ReconnectRequired` -- and
/// waits; deciding how patiently to chase a device that has just rebooted is
/// policy, and policy that depends on the medium, the device and the product.
/// So it is not in `include/smply/`, and it never will be.
///
/// It is here, in `support/`, rather than inside one example because **both**
/// examples need it and because it is the one part of a real reconnect that is
/// pure arithmetic. That matters more than it sounds: `examples/winrt_ble_dfu/`
/// cannot be run by CI at all, and before this existed nothing in the
/// repository exercised retry-with-backoff-then-give-up --
/// `FirmwareUpdater::reconnect_failed()` was public API reached only by the
/// component tests, because `cli_dfu` reconnects instantly to an in-process
/// stub. Now `cli_dfu --flaky-reconnect` drives this on every push.
///
/// The schedule is integer doubling, clamped:
///
///     attempt:  0     1     2     3     4     5   ...
///     delay:    d    2d    4d    8d   16d   max  (clamped at max_delay)
///
/// No floating point anywhere -- the same discipline as
/// `transports/common/ble_framing.hpp` -- so every delay a test asserts is
/// exact rather than nearly right.
///
/// ### What the defaults are for, after hardware (P17)
///
/// **This is a give-up bound, not a pacing schedule.** The defaults below were
/// chosen without a radio and P17 measured what they actually do on Windows:
/// nothing. `WinRtBleTransport::connect()` blocks inside the projection until
/// the device answers, so attempt 1 succeeded in every run and the doubling
/// never ran; and the bounded discovery retry now absorbs the platform's stale
/// GATT cache *inside* one attempt as well (design.md section 10).
///
/// The measurements, on a NUCLEO-WB55RG over a shared Intel radio
/// (`tests/hil/tools/measure_reset.py`, 20 plain resets, 16 complete rows):
///
///     the device advertises again      median 1.23 s   worst 1.79 s
///     the central reports the link gone  median 9.83 s
///     a GATT connection succeeds again   median 10.3 s
///
/// The middle row is the point. A reset is silent on the link, so the central
/// learns of it by supervision timeout (protocol-notes section 9, A20) -- by
/// which time the device has been advertising for eight seconds. A retry
/// schedule cannot pace a reconnect whose start it does not control, and no
/// delay this file could choose would make the device return sooner. A swap
/// reset measures about 6.3 s to a usable link, still inside one attempt.
///
/// So the defaults stay, and their justification changes: `max_attempts`
/// multiplied by `max_delay` is the **ceiling on how long an application waits
/// for a device that is never coming back** -- roughly 40 s here -- which is a
/// real decision an application should make. The delays matter only on a
/// transport whose connect fails fast, which is every transport except this
/// one; they are kept honest by `cli_dfu --flaky-reconnect` on every push.
/// Shortening `first_delay` to "reconnect sooner" on WinRT would change
/// nothing at all.

#include "smply/clock.hpp"

#include <chrono>
#include <cstdint>
#include <limits>

namespace smply::dfu_app {

/// What "keep trying" means for one application.
struct ReconnectSettings
{
    /// The wait before the first retry. Doubles from there.
    Duration first_delay{std::chrono::milliseconds{500}};

    /// The ceiling. Once the doubling reaches it, every later wait is this.
    Duration max_delay{std::chrono::seconds{8}};

    /// How many attempts before giving up altogether. Zero means "do not even
    /// try", which is a legitimate choice for a tool that would rather fail
    /// than hang.
    unsigned max_attempts = 6;
};

/// Tracks one episode of trying to get the link back.
///
/// An *episode* begins when the application is told to reconnect and ends when
/// it succeeds or gives up. A device may reboot more than once during an
/// update, so the policy is reusable: `succeeded()` returns it to the start.
///
/// \code
/// policy.begin();
/// while (!policy.exhausted()) {
///     sleep_for(policy.next_delay());
///     if (try_to_connect()) { policy.succeeded(); break; }
/// }
/// if (policy.exhausted()) { updater.reconnect_failed(...); }
/// \endcode
class ReconnectPolicy
{
public:
    ReconnectPolicy() = default;

    explicit ReconnectPolicy(ReconnectSettings settings) noexcept : settings_{settings} {}

    [[nodiscard]] const ReconnectSettings& settings() const noexcept
    {
        return settings_;
    }

    /// Starts a fresh episode. Idempotent, and safe to call at any time.
    void begin() noexcept
    {
        attempts_ = 0;
    }

    /// How many attempts this episode has already consumed.
    [[nodiscard]] unsigned attempts() const noexcept
    {
        return attempts_;
    }

    /// True once every permitted attempt has been used.
    ///
    /// **A policy of zero attempts is exhausted before it begins**, which is the
    /// answer a caller asked for by configuring zero -- not an edge case to
    /// round up to one.
    [[nodiscard]] bool exhausted() const noexcept
    {
        return attempts_ >= settings_.max_attempts;
    }

    /// The wait before the next attempt, consuming one attempt.
    ///
    /// Returns the ceiling once exhausted rather than anything surprising, so a
    /// caller that forgets to check `exhausted()` waits too long instead of
    /// spinning.
    [[nodiscard]] Duration next_delay() noexcept
    {
        if (exhausted()) {
            return settings_.max_delay;
        }
        const Duration delay = delay_for(attempts_);
        ++attempts_;
        return delay;
    }

    /// The link is back. Returns the policy to the start for a later episode --
    /// a device may reboot more than once in an update.
    void succeeded() noexcept
    {
        attempts_ = 0;
    }

private:
    /// `first_delay << attempt`, clamped to `max_delay`.
    ///
    /// The shift is done on the tick count and guarded rather than trusted:
    /// shifting a signed representation past its width is undefined, and
    /// `max_attempts` is caller-supplied, so a large one must saturate instead
    /// of wrapping to a tiny delay -- which would turn a patient retry into a
    /// spin.
    [[nodiscard]] Duration delay_for(unsigned attempt) const noexcept
    {
        const std::int64_t first = settings_.first_delay.count();
        const std::int64_t ceiling = settings_.max_delay.count();
        if (first <= 0) {
            return Duration{0}; // "retry at once" is a coherent thing to ask for
        }

        std::int64_t delay = first;
        for (unsigned i = 0; i < attempt; ++i) {
            if (delay >= ceiling || delay > (kMaxTicks / 2)) {
                return settings_.max_delay;
            }
            delay *= 2;
        }
        return delay >= ceiling ? settings_.max_delay : Duration{delay};
    }

    static constexpr std::int64_t kMaxTicks = std::numeric_limits<std::int64_t>::max();

    ReconnectSettings settings_{};
    unsigned attempts_ = 0;
};

} // namespace smply::dfu_app

#endif // SMPLY_DFU_APP_RECONNECT_POLICY_HPP
