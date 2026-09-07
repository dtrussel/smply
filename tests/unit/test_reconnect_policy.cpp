// SPDX-License-Identifier: Apache-2.0
//
// The reconnect schedule: how long to wait, and when to stop waiting.
//
// This is the portable half of what examples/winrt_ble_dfu/ does and CI cannot
// run (P16). It is also the first test anywhere of retry-with-backoff-then-
// give-up: FirmwareUpdater::reconnect_failed() had been public API reached only
// by the component suite, because cli_dfu reconnects instantly to an in-process
// stub. Every delay below is an exact integer -- the schedule uses no floating
// point precisely so that a test can say what it should be rather than
// approximately what it should be.

#include "dfu_app/reconnect_policy.hpp"

#include "smply/clock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using smply::Duration;
using smply::dfu_app::ReconnectPolicy;
using smply::dfu_app::ReconnectSettings;

namespace {

using namespace std::chrono_literals;

/// A schedule small enough to read at a glance: 10, 20, 40, 80, 100, 100 ms.
[[nodiscard]] ReconnectSettings schedule()
{
    return ReconnectSettings{.first_delay = 10ms, .max_delay = 100ms, .max_attempts = 6};
}

} // namespace

TEST_CASE("a fresh policy has spent nothing", "[reconnect]")
{
    const ReconnectPolicy policy{schedule()};
    CHECK(policy.attempts() == 0);
    CHECK_FALSE(policy.exhausted());
}

TEST_CASE("the first wait is the configured first delay", "[reconnect]")
{
    ReconnectPolicy policy{schedule()};
    CHECK(policy.next_delay() == 10ms);
    CHECK(policy.attempts() == 1);
}

TEST_CASE("each wait doubles until it reaches the ceiling", "[reconnect]")
{
    // The whole schedule in one place. 10, 20, 40, 80 double; 160 would exceed
    // the 100 ms ceiling, so it and everything after it is the ceiling.
    ReconnectPolicy policy{schedule()};
    CHECK(policy.next_delay() == 10ms);
    CHECK(policy.next_delay() == 20ms);
    CHECK(policy.next_delay() == 40ms);
    CHECK(policy.next_delay() == 80ms);
    CHECK(policy.next_delay() == 100ms);
    CHECK(policy.next_delay() == 100ms);
    CHECK(policy.exhausted());
}

TEST_CASE("the ceiling is a clamp, not a step in the sequence", "[reconnect]")
{
    // A ceiling that is not a power-of-two multiple of the first delay: the
    // clamp must produce exactly max_delay, not the doubling that overshot it.
    ReconnectPolicy policy{
        ReconnectSettings{.first_delay = 10ms, .max_delay = 35ms, .max_attempts = 4}};
    CHECK(policy.next_delay() == 10ms);
    CHECK(policy.next_delay() == 20ms);
    CHECK(policy.next_delay() == 35ms); // 40 would overshoot
    CHECK(policy.next_delay() == 35ms);
}

TEST_CASE("a policy is exhausted after exactly max_attempts", "[reconnect]")
{
    // The boundary, counted by hand: three attempts means three delays and then
    // exhaustion -- not two, and not four.
    ReconnectPolicy policy{
        ReconnectSettings{.first_delay = 5ms, .max_delay = 1s, .max_attempts = 3}};

    REQUIRE_FALSE(policy.exhausted());
    static_cast<void>(policy.next_delay());
    CHECK_FALSE(policy.exhausted());
    static_cast<void>(policy.next_delay());
    CHECK_FALSE(policy.exhausted());
    static_cast<void>(policy.next_delay());
    CHECK(policy.exhausted());
    CHECK(policy.attempts() == 3);
}

TEST_CASE("a policy of no attempts is exhausted before it starts", "[reconnect]")
{
    // Configuring zero is a caller saying "fail rather than hang". Rounding it
    // up to one attempt would be answering a question nobody asked.
    const ReconnectPolicy policy{
        ReconnectSettings{.first_delay = 10ms, .max_delay = 1s, .max_attempts = 0}};
    CHECK(policy.exhausted());
    CHECK(policy.attempts() == 0);
}

TEST_CASE("an exhausted policy waits rather than spinning", "[reconnect]")
{
    // A caller that forgot to check exhausted() should wait too long, never
    // busy-loop. Zero here would turn a give-up into a spin.
    ReconnectPolicy policy{
        ReconnectSettings{.first_delay = 10ms, .max_delay = 100ms, .max_attempts = 1}};
    static_cast<void>(policy.next_delay());
    REQUIRE(policy.exhausted());
    CHECK(policy.next_delay() == 100ms);
    CHECK(policy.attempts() == 1); // and it consumes nothing further
}

TEST_CASE("success returns the policy to the start", "[reconnect]")
{
    // A device may reboot more than once in an update -- test, reset, confirm,
    // reset again -- so the second episode must get the full budget, not
    // whatever the first one left.
    ReconnectPolicy policy{schedule()};
    static_cast<void>(policy.next_delay());
    static_cast<void>(policy.next_delay());
    REQUIRE(policy.attempts() == 2);

    policy.succeeded();
    CHECK(policy.attempts() == 0);
    CHECK_FALSE(policy.exhausted());
    CHECK(policy.next_delay() == 10ms); // the first delay again, not 40ms
}

TEST_CASE("begin restarts an episode that was abandoned midway", "[reconnect]")
{
    ReconnectPolicy policy{schedule()};
    static_cast<void>(policy.next_delay());
    static_cast<void>(policy.next_delay());
    static_cast<void>(policy.next_delay());

    policy.begin();
    CHECK(policy.attempts() == 0);
    CHECK(policy.next_delay() == 10ms);
}

TEST_CASE("a huge attempt budget saturates instead of wrapping", "[reconnect]")
{
    // The doubling is guarded because max_attempts is caller-supplied: without
    // the guard, shifting past the representation's width is undefined and a
    // wrapped value would turn a patient retry into a spin. 64 attempts is well
    // past where a millisecond count would overflow.
    ReconnectPolicy policy{
        ReconnectSettings{.first_delay = 1ms, .max_delay = 30s, .max_attempts = 64}};
    Duration previous{0};
    for (unsigned i = 0; i < 64; ++i) {
        const Duration delay = policy.next_delay();
        INFO("attempt " << i);
        CHECK(delay >= previous);   // never goes backwards
        CHECK(delay <= 30s);        // never exceeds the ceiling
        CHECK(delay > Duration{0}); // and never collapses to a spin
        previous = delay;
    }
    CHECK(policy.exhausted());
}

TEST_CASE("a zero first delay means retry at once", "[reconnect]")
{
    ReconnectPolicy policy{
        ReconnectSettings{.first_delay = 0ms, .max_delay = 1s, .max_attempts = 3}};
    CHECK(policy.next_delay() == Duration{0});
    CHECK(policy.next_delay() == Duration{0});
    CHECK_FALSE(policy.exhausted());
}

TEST_CASE("a default-constructed policy is usable", "[reconnect]")
{
    // The settings' own defaults: 500 ms doubling to 8 s, six attempts.
    ReconnectPolicy policy;
    CHECK(policy.next_delay() == 500ms);
    CHECK(policy.next_delay() == 1s);
    CHECK_FALSE(policy.exhausted());
}
