// SPDX-License-Identifier: Apache-2.0

#include "smply/clock.hpp"

#include "../support/manual_clock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using smply::Duration;
using smply::test::ManualClock;

TEST_CASE("ManualClock only moves when the test moves it", "[clock]")
{
    ManualClock clock;
    const auto start = clock.now();

    REQUIRE(clock.now() == start);

    clock.advance(std::chrono::milliseconds{250});
    REQUIRE(clock.now() == start + std::chrono::milliseconds{250});

    // Reading the clock does not advance it.
    REQUIRE(clock.now() == clock.now());
}

TEST_CASE("ManualClock is monotonic across many advances", "[clock]")
{
    ManualClock clock;
    auto previous = clock.now();

    for (int i = 0; i < 100; ++i) {
        clock.advance(std::chrono::milliseconds{i});
        REQUIRE(clock.now() >= previous);
        previous = clock.now();
    }
}

TEST_CASE("advancing by zero leaves the clock where it was", "[clock]")
{
    ManualClock clock;
    const auto start = clock.now();

    clock.advance(Duration::zero());
    REQUIRE(clock.now() == start);
}

TEST_CASE("ManualClock can be seeded and set forward", "[clock]")
{
    const auto epoch = smply::TimePoint{} + std::chrono::hours{3};
    ManualClock clock{epoch};

    REQUIRE(clock.now() == epoch);

    clock.set(epoch + std::chrono::seconds{30});
    REQUIRE(clock.now() == epoch + std::chrono::seconds{30});
}

TEST_CASE("ManualClock is usable through the Clock interface", "[clock]")
{
    ManualClock clock;
    const smply::Clock& as_interface = clock;

    const auto before = as_interface.now();
    clock.advance(std::chrono::seconds{5});

    REQUIRE(as_interface.now() == before + std::chrono::seconds{5});
}

TEST_CASE("system_clock is monotonic and stable in identity", "[clock]")
{
    const smply::Clock& clock = smply::system_clock();

    // The one place a real clock is read: proving the production Clock works.
    const auto first = clock.now();
    const auto second = clock.now();
    REQUIRE(second >= first);

    // The reference is to a single static instance.
    REQUIRE(&smply::system_clock() == &clock);
}
