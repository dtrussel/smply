// SPDX-License-Identifier: Apache-2.0
//
// Send admission for a transport with one background writer.
//
// This is the bookkeeping behind the spurious `TransportBusy` that hardware
// found in P17b (protocol-notes section 9, A22): a boolean "a write is in
// progress" refuses the next message during the window between the device
// answering and the local write's continuation running. `SendQueue` lets one
// message wait instead, so the handover stops being a refusal.
//
// It lives here rather than under transports/winrt_ble/ for the reason P15a
// established: none of it is Windows-specific, the adapter cannot be built or
// run on a Linux machine at all, and this directory — unlike the adapter — is
// seen by clang-tidy, cppcheck and the coverage filter. What no test here can
// check is the adapter's *use* of the type; the header says which three rules
// those are.

#include "common/send_queue.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <vector>

using smply::transport::Admission;
using smply::transport::SendQueue;

namespace {

/// A message every byte of which identifies it, so a queue that hands back the
/// wrong one shows up as wrong *content* rather than merely a wrong count.
[[nodiscard]] std::vector<std::byte> tagged(unsigned char tag, std::size_t size = 4)
{
    return std::vector<std::byte>(size, static_cast<std::byte>(tag));
}

/// The invariant the type exists to make unrepresentable: a message never waits
/// unless a writer has been promised. Asserted after every operation below.
void check_invariant(const SendQueue& queue)
{
    if (queue.queued()) {
        REQUIRE(queue.writing());
    }
}

} // namespace

TEST_CASE("a fresh queue is idle and has no writer", "[ble][send-queue]")
{
    const SendQueue queue;

    CHECK_FALSE(queue.writing());
    CHECK_FALSE(queue.queued());
    CHECK(queue.counters().deferred == 0);
    CHECK(queue.counters().refused == 0);
}

TEST_CASE("the first message starts the writer", "[ble][send-queue]")
{
    SendQueue queue;

    CHECK(queue.offer(tagged('A')) == Admission::StartWriter);
    CHECK(queue.writing());
    CHECK(queue.queued()); // waiting for the promised writer to take it
    CHECK(queue.counters().deferred == 0);
    check_invariant(queue);
}

TEST_CASE("a second message is queued rather than refused", "[ble][send-queue]")
{
    // The regression this type exists for. The writer is mid-message when the
    // device's answer arrives and the core offers the next chunk; before A22
    // that was `TransportBusy` and the upload died on it.
    SendQueue queue;
    REQUIRE(queue.offer(tagged('A')) == Admission::StartWriter);
    REQUIRE(queue.next_for_writer().has_value()); // the writer takes A

    CHECK(queue.offer(tagged('B')) == Admission::Queued);
    CHECK(queue.writing());
    CHECK(queue.queued());
    CHECK(queue.counters().deferred == 1);
    CHECK(queue.counters().refused == 0);
    check_invariant(queue);
}

TEST_CASE("a third message is refused", "[ble][send-queue]")
{
    // One being written, one waiting: a third means the medium is not draining,
    // which is what `TransportBusy` is actually for.
    SendQueue queue;
    REQUIRE(queue.offer(tagged('A')) == Admission::StartWriter);
    REQUIRE(queue.next_for_writer().has_value());
    REQUIRE(queue.offer(tagged('B')) == Admission::Queued);

    CHECK(queue.offer(tagged('C')) == Admission::Busy);
    CHECK(queue.counters().refused == 1);
    check_invariant(queue);
}

TEST_CASE("a refused message is not stored", "[ble][send-queue]")
{
    // A `Busy` must not overwrite what is waiting, nor append behind it: the
    // caller is about to report a failure and owns the retry.
    SendQueue queue;
    REQUIRE(queue.offer(tagged('A')) == Admission::StartWriter);
    REQUIRE(queue.next_for_writer() == tagged('A'));
    REQUIRE(queue.offer(tagged('B')) == Admission::Queued);
    REQUIRE(queue.offer(tagged('C')) == Admission::Busy);

    CHECK(queue.next_for_writer() == tagged('B'));
    CHECK_FALSE(queue.next_for_writer().has_value()); // C is nowhere
}

TEST_CASE("the writer drains in the order the messages were offered", "[ble][send-queue]")
{
    SendQueue queue;
    REQUIRE(queue.offer(tagged('A')) == Admission::StartWriter);
    CHECK(queue.next_for_writer() == tagged('A'));
    REQUIRE(queue.offer(tagged('B')) == Admission::Queued);
    CHECK(queue.next_for_writer() == tagged('B'));
    REQUIRE(queue.offer(tagged('C')) == Admission::Queued);
    CHECK(queue.next_for_writer() == tagged('C'));
    CHECK_FALSE(queue.next_for_writer().has_value());
}

TEST_CASE("the writer keeps its claim while a message is waiting", "[ble][send-queue]")
{
    // If the claim were released here, `offer()` would promise a second writer
    // and two messages' fragments could interleave on one medium (ADR-0006).
    SendQueue queue;
    REQUIRE(queue.offer(tagged('A')) == Admission::StartWriter);
    REQUIRE(queue.next_for_writer().has_value());
    REQUIRE(queue.offer(tagged('B')) == Admission::Queued);

    CHECK(queue.writing());
    CHECK(queue.next_for_writer() == tagged('B'));
    CHECK(queue.writing()); // still claimed: the writer has not asked again yet
}

TEST_CASE("the writer is released only when it finds nothing left", "[ble][send-queue]")
{
    SendQueue queue;
    REQUIRE(queue.offer(tagged('A')) == Admission::StartWriter);
    REQUIRE(queue.next_for_writer().has_value());

    CHECK_FALSE(queue.next_for_writer().has_value());
    CHECK_FALSE(queue.writing());
    CHECK_FALSE(queue.queued());

    // Idle again, so the next message promises a fresh writer rather than
    // being queued for one that has already exited.
    CHECK(queue.offer(tagged('B')) == Admission::StartWriter);
}

TEST_CASE("a message never waits without a writer", "[ble][send-queue]")
{
    // The invariant, over a mixed sequence rather than one path.
    SendQueue queue;
    check_invariant(queue);

    static_cast<void>(queue.offer(tagged('A')));
    check_invariant(queue);
    static_cast<void>(queue.offer(tagged('B')));
    check_invariant(queue);
    static_cast<void>(queue.next_for_writer());
    check_invariant(queue);
    static_cast<void>(queue.offer(tagged('C')));
    check_invariant(queue);
    queue.discard_queued();
    check_invariant(queue);
    static_cast<void>(queue.next_for_writer());
    check_invariant(queue);
    static_cast<void>(queue.next_for_writer());
    check_invariant(queue);
    CHECK_FALSE(queue.writing());
}

TEST_CASE("discarding drops a message the writer has not taken yet", "[ble][send-queue]")
{
    // A closing link: nothing accepted but not started may reach the medium.
    // The claim survives, so the promised writer starts, finds nothing and
    // exits — rather than a second writer being promised alongside it.
    SendQueue queue;
    REQUIRE(queue.offer(tagged('A')) == Admission::StartWriter);

    queue.discard_queued();

    CHECK(queue.writing());
    CHECK_FALSE(queue.queued());
    CHECK_FALSE(queue.next_for_writer().has_value());
    CHECK_FALSE(queue.writing());
}

TEST_CASE("discarding leaves the message the writer already took", "[ble][send-queue]")
{
    // The writer stops between fragments on its own cancellation flag; the
    // queue has no say over a message already in its hands.
    SendQueue queue;
    REQUIRE(queue.offer(tagged('A')) == Admission::StartWriter);
    REQUIRE(queue.next_for_writer() == tagged('A'));
    REQUIRE(queue.offer(tagged('B')) == Admission::Queued);

    queue.discard_queued();

    CHECK(queue.writing());
    CHECK_FALSE(queue.queued());
    CHECK_FALSE(queue.next_for_writer().has_value()); // B is gone, A was not recalled
}

TEST_CASE("discarding an idle queue is harmless", "[ble][send-queue]")
{
    SendQueue queue;

    queue.discard_queued();

    CHECK_FALSE(queue.writing());
    CHECK_FALSE(queue.queued());
    CHECK(queue.offer(tagged('A')) == Admission::StartWriter);
}

TEST_CASE("a hundred messages pass through one writer", "[ble][send-queue]")
{
    // FIFO and no-interleave as a property: one writer is promised, every
    // message comes back exactly once, in order.
    SendQueue queue;
    unsigned starts = 0;
    std::vector<std::byte> seen;

    for (unsigned i = 0; i < 100; ++i) {
        const auto tag = static_cast<unsigned char>(i);
        if (queue.offer(tagged(tag, 1)) == Admission::StartWriter) {
            ++starts;
        }
        const auto next = queue.next_for_writer();
        REQUIRE(next.has_value());
        REQUIRE(next->size() == 1);
        seen.push_back(next->front());
    }

    CHECK(starts == 1);
    REQUIRE(seen.size() == 100);
    for (unsigned i = 0; i < 100; ++i) {
        REQUIRE(seen[i] == static_cast<std::byte>(static_cast<unsigned char>(i)));
    }
    CHECK(queue.counters().refused == 0);
}

TEST_CASE("the counters record every deferral and every refusal", "[ble][send-queue]")
{
    // A bench run reads these: `deferred == 0` would mean the race never
    // happened, not that deferring works.
    SendQueue queue;
    REQUIRE(queue.offer(tagged('A')) == Admission::StartWriter);
    REQUIRE(queue.next_for_writer().has_value());
    REQUIRE(queue.offer(tagged('B')) == Admission::Queued);
    REQUIRE(queue.offer(tagged('C')) == Admission::Busy);
    REQUIRE(queue.offer(tagged('D')) == Admission::Busy);

    CHECK(queue.counters().deferred == 1);
    CHECK(queue.counters().refused == 2);

    // A `StartWriter` is neither a deferral nor a refusal.
    REQUIRE(queue.next_for_writer().has_value());
    REQUIRE_FALSE(queue.next_for_writer().has_value());
    REQUIRE(queue.offer(tagged('E')) == Admission::StartWriter);
    CHECK(queue.counters().deferred == 1);
    CHECK(queue.counters().refused == 2);
}

TEST_CASE("an empty message is stored as offered", "[ble][send-queue]")
{
    // The adapter refuses an empty SMP message before it ever gets here, so
    // this pins that the queue neither special-cases nor loses one — an empty
    // optional and an empty vector must not be confused.
    SendQueue queue;

    REQUIRE(queue.offer(std::vector<std::byte>{}) == Admission::StartWriter);
    CHECK(queue.queued());
    const auto taken = queue.next_for_writer();
    REQUIRE(taken.has_value());
    CHECK(taken->empty());
}
