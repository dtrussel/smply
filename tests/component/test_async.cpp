// SPDX-License-Identifier: Apache-2.0
//
// smply::asyncutil (ADR-0019): coroutines and futures over the callback core,
// driven against the simulated device.
//
// The coroutine resumes inside the completion callback, on the pump thread, so
// a coroutine that awaits one operation after another is a callback chain
// written straight. The cases that earn their keep are the ones about that
// chain: two uploads back to back from one coroutine (the second starts from
// inside the first's completion), and a coroutine destroyed while it waits.
// Both are here, and both run under the sanitizer presets.

#include "harness.hpp"

#include "smply/async/future.hpp"
#include "smply/async/task.hpp"
#include "smply/error.hpp"
#include "smply/groups/image.hpp"
#include "smply/groups/image_upload.hpp"
#include "smply/image_source.hpp"
#include "smply/limits.hpp"
#include "smply/result.hpp"
#include "smply/util/dispatcher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using smply::ConstBytes;
using smply::ErrorCode;
using smply::ImageState;
using smply::MemoryImageSource;
using smply::Result;
using smply::UploadOptions;
using smply::UploadResult;
using smply::async::await_result;
using smply::async::Task;
using smply::test::Fixture;
using smply::test::make_firmware;

namespace {

constexpr std::uint32_t kBodySize = 600;

/// Awaits one whole upload.
[[nodiscard]] auto upload(Fixture& fixture, MemoryImageSource& source)
{
    return await_result<UploadResult>([&fixture, &source](auto done) {
        static_cast<void>(fixture.management.upload(source, UploadOptions{}, {}, std::move(done)));
    });
}

/// Awaits one image-state read.
[[nodiscard]] auto read_state(Fixture& fixture)
{
    return await_result<ImageState>([&fixture](auto done) {
        static_cast<void>(fixture.management.get_state(std::move(done)));
    });
}

[[nodiscard]] bool slot_holds(const Fixture& fixture, const std::vector<std::byte>& image)
{
    const ConstBytes flashed = fixture.simulator.slot_content(1);
    return flashed.size() == image.size() &&
           std::equal(flashed.begin(), flashed.end(), image.begin());
}

} // namespace

// --- coroutines -------------------------------------------------------------

TEST_CASE("a coroutine reads, uploads and reads again, in order", "[async][task]")
{
    const std::vector<std::byte> firmware = make_firmware(kBodySize);
    Fixture fixture;
    MemoryImageSource source{ConstBytes{firmware}};

    const auto body = [&]() -> Task<std::size_t> {
        const Result<ImageState> before = co_await read_state(fixture);
        REQUIRE(before.has_value());
        const Result<UploadResult> uploaded = co_await upload(fixture, source);
        REQUIRE(uploaded.has_value());
        CHECK(uploaded->transferred == firmware.size());
        const Result<ImageState> after = co_await read_state(fixture);
        REQUIRE(after.has_value());
        co_return after->slots.size();
    };
    Task<std::size_t> task = body();

    // Eager, but nothing completes inside the call that started it (ADR-0003).
    CHECK_FALSE(task.done());
    REQUIRE(fixture.run_until([&] { return task.done(); }));
    CHECK(task.result() >= 1);
    CHECK(slot_holds(fixture, firmware));
}

TEST_CASE("one coroutine can run two uploads back to back", "[async][task]")
{
    // The second upload starts from inside the first one's completion callback,
    // which is where the coroutine resumes. That is a chained callback, and the
    // upload driver clears its session before it calls out.
    const std::vector<std::byte> first = make_firmware(kBodySize, 1);
    const std::vector<std::byte> second = make_firmware(kBodySize, 2);
    Fixture fixture;
    MemoryImageSource first_source{ConstBytes{first}};
    MemoryImageSource second_source{ConstBytes{second}};

    const auto body = [&]() -> Task<void> {
        const Result<UploadResult> one = co_await upload(fixture, first_source);
        REQUIRE(one.has_value());
        const Result<UploadResult> two = co_await upload(fixture, second_source);
        REQUIRE(two.has_value());
    };
    Task<void> task = body();

    REQUIRE(fixture.run_until([&] { return task.done(); }));
    task.result();
    CHECK(slot_holds(fixture, second));
}

TEST_CASE("a failed operation resumes the coroutine with its error", "[async][task]")
{
    Fixture fixture;
    const std::string too_long(smply::limits::kMaxEchoLength + 1, 'x');

    const auto body = [&]() -> Task<std::optional<ErrorCode>> {
        const Result<std::string> echoed = co_await await_result<std::string>(
            [&](auto done) { static_cast<void>(fixture.os.echo(too_long, std::move(done))); });
        co_return echoed.has_value() ? std::nullopt
                                     : std::optional<ErrorCode>{echoed.error().code()};
    };
    Task<std::optional<ErrorCode>> task = body();

    REQUIRE(fixture.run_until([&] { return task.done(); }));
    CHECK(task.result() == ErrorCode::InvalidArgument);
}

TEST_CASE("destroying a suspended task drops the result without resuming it", "[async][task]")
{
    Fixture fixture;
    bool resumed = false;

    {
        const auto body = [&]() -> Task<void> {
            static_cast<void>(co_await read_state(fixture));
            resumed = true;
        };
        const Task<void> task = body();
        CHECK_FALSE(task.done());
    } // The coroutine frame is destroyed here, suspended in await_result().

    // The read still completes. The awaitable was detached, so its callback
    // has nothing to resume, which the sanitizer presets would catch.
    for (int i = 0; i < 50; ++i) {
        fixture.step();
    }
    CHECK_FALSE(resumed);
}

TEST_CASE("a task can await another task", "[async][task]")
{
    Fixture fixture;

    const auto inner = [&]() -> Task<std::size_t> {
        const Result<ImageState> state = co_await read_state(fixture);
        co_return state.has_value() ? state->slots.size() + 100 : 0;
    };
    const auto outer = [&]() -> Task<std::size_t> { co_return co_await inner(); };
    Task<std::size_t> task = outer();

    REQUIRE(fixture.run_until([&] { return task.done(); }));
    CHECK(task.result() >= 100);
}

TEST_CASE("an exception escaping a task is rethrown by result()", "[async][task]")
{
    const auto body = []() -> Task<int> {
        throw std::runtime_error{"application code failed"};
        co_return 0; // NOLINT(clang-diagnostic-unreachable-code) -- makes it a coroutine
    };
    Task<int> task = body();

    REQUIRE(task.done());
    // Caught by hand: cppcheck cannot parse Catch2's THROWS macros.
    bool rethrown = false;
    try {
        static_cast<void>(task.result());
    } catch (const std::runtime_error&) {
        rethrown = true;
    }
    CHECK(rethrown);
}

TEST_CASE("an operation that completes inline does not suspend", "[async][task]")
{
    // smply never does this (ADR-0003), but an application's own operation
    // might, and resuming from inside await_suspend() would be a bug.
    const auto body = []() -> Task<int> {
        const Result<int> value = co_await await_result<int>([](auto done) { done(42); });
        co_return value.value_or(0);
    };
    Task<int> task = body();

    REQUIRE(task.done());
    CHECK(task.result() == 42);
}

// --- futures ----------------------------------------------------------------

TEST_CASE("a worker thread gets a result through a future", "[async][future]")
{
    Fixture fixture;
    smply::Dispatcher dispatcher;
    std::atomic<bool> finished{false};
    std::optional<Result<ImageState>> seen;

    // The worker blocks on the future; this thread is the pump, and never
    // does. That is the one rule future.hpp exists to state.
    std::thread worker{[&] {
        std::future<Result<ImageState>> state =
            smply::async::post_for_future<ImageState>(dispatcher, [&fixture](auto done) {
                static_cast<void>(fixture.management.get_state(std::move(done)));
            });
        seen.emplace(state.get());
        finished = true;
    }};

    for (int i = 0; i < smply::test::kDefaultBudget && !finished; ++i) {
        dispatcher.drain();
        fixture.step();
        std::this_thread::yield();
    }
    worker.join();

    REQUIRE(finished);
    REQUIRE(seen.has_value());
    CHECK(seen->has_value());
}

TEST_CASE("a dispatcher cleared before the work runs breaks the promise", "[async][future]")
{
    Fixture fixture;
    smply::Dispatcher dispatcher;

    std::future<Result<ImageState>> state =
        smply::async::post_for_future<ImageState>(dispatcher, [&fixture](auto done) {
            static_cast<void>(fixture.management.get_state(std::move(done)));
        });
    dispatcher.clear();

    // Throws rather than blocking forever: nothing will ever set the value.
    bool broken = false;
    try {
        static_cast<void>(state.get());
    } catch (const std::future_error&) {
        broken = true;
    }
    CHECK(broken);
}
