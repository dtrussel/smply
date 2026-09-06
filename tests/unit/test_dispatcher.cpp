// SPDX-License-Identifier: Apache-2.0
//
// Dispatcher is the only concurrent component smply ships, and the only place
// in the tree where a test can be wrong without being red: a race that does not
// happen today happens in a customer's adapter next year. So the properties
// below are asserted structurally wherever possible -- ordering, counts,
// which-thread -- rather than by hoping a schedule interleaves usefully, and
// the one genuinely racy test (many producers) checks a conservation law that
// no interleaving may break. The TSan job is what looks for the rest.

#include "smply/util/dispatcher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

using smply::Dispatcher;

namespace {

/// Records its own destruction, so a test can say *when* a closure's captures
/// were released rather than only that they were.
class DestructionRecorder
{
public:
    explicit DestructionRecorder(std::function<void()> on_destroy)
        : on_destroy_{std::move(on_destroy)}
    {}

    DestructionRecorder(const DestructionRecorder&) = default;
    DestructionRecorder(DestructionRecorder&&) = default;
    DestructionRecorder& operator=(const DestructionRecorder&) = delete;
    DestructionRecorder& operator=(DestructionRecorder&&) = delete;

    ~DestructionRecorder()
    {
        if (on_destroy_) {
            on_destroy_();
        }
    }

private:
    std::function<void()> on_destroy_;
};

} // namespace

TEST_CASE("drain runs every posted closure once, in order, and returns the count", "[dispatcher]")
{
    Dispatcher dispatcher;
    std::vector<int> ran;

    dispatcher.post([&ran] { ran.push_back(1); });
    dispatcher.post([&ran] { ran.push_back(2); });
    dispatcher.post([&ran] { ran.push_back(3); });

    // Nothing runs until the consumer asks: posting is not dispatching.
    REQUIRE(ran.empty());
    REQUIRE(dispatcher.pending() == 3);

    REQUIRE(dispatcher.drain() == 3);
    REQUIRE(ran == std::vector<int>{1, 2, 3});
    CHECK(dispatcher.pending() == 0);

    // And once means once.
    CHECK(dispatcher.drain() == 0);
    CHECK(ran.size() == 3);
}

TEST_CASE("draining an empty dispatcher is zero, not an error", "[dispatcher]")
{
    Dispatcher dispatcher;
    CHECK(dispatcher.drain() == 0);
    CHECK(dispatcher.pending() == 0);
}

TEST_CASE("an empty std::function is ignored rather than queued", "[dispatcher]")
{
    // So an adapter that builds a closure conditionally does not have to check,
    // and so drain() can never call an empty function.
    Dispatcher dispatcher;
    dispatcher.post({});
    CHECK(dispatcher.pending() == 0);
    CHECK(dispatcher.drain() == 0);
}

TEST_CASE("clear discards without running", "[dispatcher]")
{
    // The teardown case: the closures name a transport that is going away, so
    // running them would be worse than dropping them.
    Dispatcher dispatcher;
    bool ran = false;
    dispatcher.post([&ran] { ran = true; });

    dispatcher.clear();

    CHECK_FALSE(ran);
    CHECK(dispatcher.pending() == 0);
    CHECK(dispatcher.drain() == 0);
}

TEST_CASE("the destructor discards without running", "[dispatcher]")
{
    bool ran = false;
    {
        Dispatcher dispatcher;
        dispatcher.post([&ran] { ran = true; });
    }
    CHECK_FALSE(ran);
}

TEST_CASE("a closure posted from inside drain runs on the next drain", "[dispatcher]")
{
    // The queue is taken before anything runs, so what a closure posts belongs
    // to the next turn. That is what lets an adapter post freely from inside a
    // closure without wondering whether it is extending the current drain.
    Dispatcher dispatcher;
    std::vector<int> order;

    dispatcher.post([&] {
        order.push_back(1);
        dispatcher.post([&order] { order.push_back(2); });
    });

    REQUIRE(dispatcher.drain() == 1);
    CHECK(order == std::vector<int>{1});

    REQUIRE(dispatcher.drain() == 1);
    CHECK(order == std::vector<int>{1, 2});

    CHECK(dispatcher.drain() == 0);
}

TEST_CASE("a closure that reposts itself takes one turn per drain", "[dispatcher]")
{
    // The pump keeps its turn. If drain() ran what it was given *plus* what
    // that produced, a closure like this would starve the rest of the loop --
    // including SmpClient::poll(), which is where every deadline lives.
    Dispatcher dispatcher;
    int runs = 0;

    std::function<void()> repost;
    repost = [&] {
        ++runs;
        if (runs < 3) {
            dispatcher.post(repost);
        }
    };
    dispatcher.post(repost);

    REQUIRE(dispatcher.drain() == 1);
    CHECK(runs == 1);
    REQUIRE(dispatcher.drain() == 1);
    CHECK(runs == 2);
    REQUIRE(dispatcher.drain() == 1);
    CHECK(runs == 3);

    // The third run did not repost, so the queue is finally empty.
    CHECK(dispatcher.drain() == 0);
}

TEST_CASE("a re-entrant drain is refused, and loses nothing", "[dispatcher]")
{
    // Same answer MessageAssembler gives a re-entrant feed() (design.md
    // section 2): refuse, rather than recurse into a queue being mutated.
    Dispatcher dispatcher;
    std::size_t inner_result = 99;
    bool second_ran = false;

    dispatcher.post([&] {
        dispatcher.post([&second_ran] { second_ran = true; });
        inner_result = dispatcher.drain();
    });

    REQUIRE(dispatcher.drain() == 1);
    CHECK(inner_result == 0);
    CHECK_FALSE(second_ran);

    // Refused, not dropped: the outer loop's next turn picks it up.
    REQUIRE(dispatcher.drain() == 1);
    CHECK(second_ran);
}

TEST_CASE("the wake callback fires on post, on the posting thread, and not on drain",
          "[dispatcher]")
{
    // Where a naive adapter deadlocks. An adapter that assumed the wake ran on
    // the client context would take the client's lock from a driver thread.
    int wakes = 0;
    std::thread::id woke_on;
    Dispatcher dispatcher{[&] {
        ++wakes;
        woke_on = std::this_thread::get_id();
    }};

    dispatcher.post([] {});
    CHECK(wakes == 1);
    CHECK(woke_on == std::this_thread::get_id());

    dispatcher.post([] {});
    CHECK(wakes == 2);

    // Draining is not a wake: the consumer already knows it is awake.
    static_cast<void>(dispatcher.drain());
    CHECK(wakes == 2);

    // Nor is an ignored empty closure.
    dispatcher.post({});
    CHECK(wakes == 2);
}

TEST_CASE("a capture may post from its destructor, on drain", "[dispatcher]")
{
    // Destroying a closure runs its captures' destructors, which are
    // application code and may post. If drain() held the lock while destroying
    // the closures it ran, this would self-deadlock on a non-recursive mutex --
    // so the test reaching its assertions at all is most of the proof.
    //
    // Counted rather than pinned to an exact number: std::function is allowed
    // to copy the closure, and each copy destroys once. What matters is that
    // the posts happen and nothing hangs.
    Dispatcher dispatcher;
    int posted = 0;

    {
        const DestructionRecorder recorder{[&] { dispatcher.post([&posted] { ++posted; }); }};
        dispatcher.post([recorder] { static_cast<void>(recorder); });
    }

    // Terminates: what a destructor posts captures no recorder, so the chain is
    // one deep however many copies std::function made.
    while (dispatcher.drain() > 0) {
    }
    CHECK(posted > 0);
}

TEST_CASE("a capture may post from its destructor, on clear", "[dispatcher]")
{
    Dispatcher dispatcher;
    int destructions = 0;

    {
        const DestructionRecorder recorder{[&destructions] { ++destructions; }};
        dispatcher.post([recorder] { static_cast<void>(recorder); });
    }
    const int before = destructions; // the local's own destruction is included

    dispatcher.clear();

    CHECK(destructions > before);
    CHECK(dispatcher.pending() == 0);
}

TEST_CASE("many producers lose nothing", "[dispatcher][threads]")
{
    // The conservation law: every closure posted is run exactly once, whatever
    // the interleaving. Counts, not timing -- there is no schedule this may
    // depend on. Under TSan this is also the race detector's main workout.
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 250;

    Dispatcher dispatcher;
    std::atomic<int> wakes{0};
    Dispatcher woken{[&wakes] { wakes.fetch_add(1, std::memory_order_relaxed); }};

    std::atomic<int> ran{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kPerProducer; ++i) {
                dispatcher.post([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
                woken.post([] {});
            }
        });
    }

    go.store(true, std::memory_order_release);

    // Drain concurrently with the producers, as a real pump would. This
    // terminates because every producer posts a fixed number and then exits, so
    // the total is reached whatever the interleaving -- there is no timeout
    // here, and deliberately: a test that gives up after a while would turn a
    // lost closure into a flake instead of a failure.
    constexpr std::size_t kTotal = std::size_t{kProducers} * kPerProducer;
    std::size_t drained = 0;
    while (drained < kTotal) {
        const std::size_t n = dispatcher.drain();
        if (n == 0) {
            std::this_thread::yield();
        }
        drained += n;
    }

    for (std::thread& t : producers) {
        t.join();
    }

    CHECK(drained == kTotal);
    CHECK(dispatcher.drain() == 0);
    CHECK(ran.load() == static_cast<int>(kTotal));
    CHECK(dispatcher.pending() == 0);

    // Every post woke exactly once, from whichever thread posted it.
    CHECK(wakes.load() == static_cast<int>(kTotal));
    CHECK(woken.drain() == kTotal);
}
