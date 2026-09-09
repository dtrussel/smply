// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TRANSPORTS_COMMON_SEND_QUEUE_HPP
#define SMPLY_TRANSPORTS_COMMON_SEND_QUEUE_HPP

/// \file
/// The send-admission half of an asynchronous transport, as a value.
///
/// An adapter that writes on a background thread has to answer one question on
/// the client context, synchronously, without blocking: *may this message go
/// out now?* `Transport::send()` offers exactly three answers (smply/transport.hpp)
/// — accepted, `TransportBusy` meaning "retry when the medium drains", or a real
/// failure — and the core deliberately does not queue on the transport's behalf.
///
/// The obvious implementation is a single "a write is in progress" flag, and it
/// is wrong in a way only hardware shows. The flag can only be cleared by the
/// write's own completion, and the **device's answer can arrive first**: the
/// response travels device → radio → OS → a pool thread → the client, while the
/// local write's continuation waits for a thread of its own. So the core, having
/// been answered, offers the next message and is refused — although the previous
/// message is complete in every sense that matters. P17b measured this on a
/// NUCLEO-WB55RG: a plain upload died on `TransportBusy` six cases into a run
/// (docs/protocol-notes.md section 9, A22).
///
/// The fix is to stop conflating "a writer is running" with "no further message
/// may be accepted". This type holds that distinction: one message may **wait**
/// while another is being written, so the handover window stops being a refusal.
/// `TransportBusy` stays reachable — a third message, with one writing and one
/// waiting, means the medium genuinely is not draining — so the contract stays
/// honest rather than being quietly widened.
///
/// ### What is testable here, and what is not
///
/// The bookkeeping is: who may start a writer, when a writer may exit, and what
/// happens to a message that arrives in between. That is what this type owns and
/// what `tests/unit/test_send_queue.cpp` pins, on every platform. What it cannot
/// check is the adapter's *use* of it — that every call happens under the
/// adapter's mutex, that the mutex is never held across a suspension point, and
/// that a writer is started exactly on `Admission::StartWriter`. Those three are
/// the residual risk a bench run carries, and they are why the rules below are
/// stated as rules rather than left to be inferred.
///
/// Not thread-safe, deliberately — like `LinkState` next door. The adapter owns
/// one mutex and this is one of the things it guards; a type with its own lock
/// would invite two locks with no order between them.
///
/// Not `constexpr`, unlike `LinkState` and `Fragmenter`: it owns the bytes of a
/// message, so it allocates. Nothing here is a compile-time value, and trying to
/// make it one would mean handing ownership back to the caller — which is the
/// coupling this type exists to remove.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace smply::transport {

/// What `offer()` decided, and what the caller must do about it.
enum class Admission : std::uint8_t
{
    /// Accepted, and **the caller must start the writer**.
    ///
    /// Returned only when no writer is live. Starting a second writer on one
    /// medium would let two messages' fragments interleave, and reassembly
    /// rests entirely on them not doing so (ADR-0006, protocol-notes section 8):
    /// there is no framing to resynchronise on, so the damage would be a
    /// mis-framed message rather than an error anyone reports.
    StartWriter,
    /// Accepted and left for the writer already running. Nothing to start.
    Queued,
    /// Refused: one message is being written and another is already waiting.
    /// The adapter turns this into `ErrorCode::TransportBusy`.
    Busy,
};

/// How often the waiting slot was used, and how often it was full.
///
/// Diagnostics, and load-bearing ones. A bench run that passes with
/// `deferred == 0` has not shown that deferring a message works — it has shown
/// that the race did not happen that time. The counter is what makes the
/// difference visible (ADR-0015 asks evidence to distinguish those two).
struct SendCounters
{
    std::uint64_t deferred = 0; ///< `Queued` admissions.
    std::uint64_t refused = 0;  ///< `Busy` admissions.
};

/// Admission control for a transport with one background writer.
///
/// The invariant, and the reason the awkward states cannot be reached: the
/// writer's claim is taken by `offer()` and released **only** by
/// `next_for_writer()` finding nothing left. So a writer never exits while a
/// message is waiting, a second writer is never started, and "a message waits
/// but nobody is writing" is unrepresentable.
class SendQueue
{
public:
    /// Offers \p message for transmission.
    ///
    /// Takes ownership on `StartWriter` and `Queued`; on `Busy` the message is
    /// dropped, which is correct because the caller reports a failure and the
    /// layer above owns the retry.
    [[nodiscard]] Admission offer(std::vector<std::byte> message)
    {
        if (!writing_) {
            // The claim and the message are taken together: a writer that has
            // been promised must have something to find.
            writing_ = true;
            waiting_ = std::move(message);
            return Admission::StartWriter;
        }
        if (!waiting_.has_value()) {
            waiting_ = std::move(message);
            ++counters_.deferred;
            return Admission::Queued;
        }
        ++counters_.refused;
        return Admission::Busy;
    }

    /// The next message for the writer, or `std::nullopt`.
    ///
    /// **`std::nullopt` releases the writer**: the queue becomes idle and a
    /// later `offer()` will hand out a fresh `StartWriter`. A writer must
    /// therefore call this until it answers `nullopt` and then exit, and must
    /// not exit on any other condition — leaving the claim set would make every
    /// later `send()` answer `Busy` on a healthy link, and would hang a
    /// `close()` that waits for the writer to finish.
    [[nodiscard]] std::optional<std::vector<std::byte>> next_for_writer()
    {
        if (!waiting_.has_value()) {
            writing_ = false;
            return std::nullopt;
        }
        std::optional<std::vector<std::byte>> next = std::move(waiting_);
        waiting_.reset();
        return next;
    }

    /// Drops the **waiting** message, if any, and nothing else.
    ///
    /// For a link that is closing or has already failed: no message accepted
    /// but not yet started should reach the medium. It deliberately does not
    /// touch the writer's claim — the writer is mid-message and stops between
    /// fragments on its own cancellation flag, and clearing the claim here
    /// would let a second writer start alongside it.
    void discard_queued() noexcept
    {
        waiting_.reset();
    }

    /// Is a writer live? True from the `StartWriter` that promised one until
    /// the `next_for_writer()` that releases it.
    [[nodiscard]] bool writing() const noexcept
    {
        return writing_;
    }

    /// Is a message waiting for the writer to pick it up?
    ///
    /// Never true while `writing()` is false; that pairing is the invariant.
    [[nodiscard]] bool queued() const noexcept
    {
        return waiting_.has_value();
    }

    [[nodiscard]] SendCounters counters() const noexcept
    {
        return counters_;
    }

private:
    /// The message the writer has not taken yet. At most one: the writer may
    /// hold another, so two messages can be outbound and a third is refused.
    std::optional<std::vector<std::byte>> waiting_;
    bool writing_ = false;
    SendCounters counters_;
};

} // namespace smply::transport

#endif // SMPLY_TRANSPORTS_COMMON_SEND_QUEUE_HPP
