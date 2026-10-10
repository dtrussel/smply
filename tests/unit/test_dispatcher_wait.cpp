// SPDX-License-Identifier: Apache-2.0
//
// The update run's real wait (support/dfu_app/dispatcher_wait.hpp).
//
// Only what can be said without waiting: a deadline already in the past
// returns at once, and a wake that came first is not lost. The run's own
// behaviour is the component suite's (tests/component/test_update_run.cpp);
// the blocking itself is what the examples' ctests exercise end to end.

#include "dfu_app/dispatcher_wait.hpp"

#include "smply/clock.hpp"
#include "smply/util/dispatcher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <optional>

using smply::Dispatcher;
using smply::TimePoint;
using smply::dfu_app::DispatcherWait;

namespace {

/// A deadline that has already passed: the wait returns without blocking.
constexpr TimePoint kLongAgo{};

} // namespace

TEST_CASE("a wait delivers what the application's dispatcher holds", "[dfu_app][wait]")
{
    DispatcherWait wait;
    Dispatcher inbound{wait.waker()};
    wait.deliver_from(inbound);

    int ran = 0;
    inbound.post([&] { ++ran; });
    wait.wait_until(kLongAgo);

    CHECK(ran == 1);
    CHECK(inbound.pending() == 0);
}

TEST_CASE("a wake that came before the wait is not lost", "[dfu_app][wait]")
{
    // No deadline means "wait a while"; the post before it has already set
    // the signal, so the wait returns and delivers at once.
    DispatcherWait wait;
    Dispatcher inbound{wait.waker()};
    wait.deliver_from(inbound);

    int ran = 0;
    inbound.post([&] { ++ran; });
    wait.wait_until(std::nullopt);

    CHECK(ran == 1);
}

TEST_CASE("without a dispatcher, a wait delivers nothing", "[dfu_app][wait]")
{
    DispatcherWait wait;
    Dispatcher inbound{wait.waker()};

    int ran = 0;
    inbound.post([&] { ++ran; });
    wait.wait_until(kLongAgo);

    CHECK(ran == 0);
    CHECK(inbound.pending() == 1);
}

TEST_CASE("the waker stays safe to call after the wait is gone", "[dfu_app][wait]")
{
    std::function<void()> waker;
    {
        const DispatcherWait wait;
        waker = wait.waker();
    }
    // A dispatcher declared before the wait outlives it, and still posts.
    waker();
    SUCCEED();
}
