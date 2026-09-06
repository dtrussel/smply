// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_UTIL_DISPATCHER_HPP
#define SMPLY_UTIL_DISPATCHER_HPP

/// \file
/// Marshalling helper for transport adapters. **Not part of the core.**
///
/// The core has exactly one client context and contains no mutex, no atomic and
/// no thread (ADR-0004). That pushes one obligation onto every adapter: a
/// driver that delivers on its own thread -- WinRT's thread pool, a serial
/// reader thread -- must hand the bytes to the client context before calling
/// `TransportListener::on_bytes()`. Only the adapter knows its threading
/// environment, so only the adapter can do it; this class exists so that each
/// adapter does not reinvent it.
///
/// It ships in a **separate target**, `smply::util`. Nothing in `libsmply`
/// links it, and that is structural rather than a convention: the core does not
/// depend on it, so a change that makes it do so fails to build.
///
/// \code
/// smply::Dispatcher inbound{[&] { wake_the_pump(); }};
///
/// // driver thread:
/// inbound.post([this, bytes = std::vector<std::byte>{data, data + size}] {
///     client_.on_bytes(smply::ConstBytes{bytes});   // now on the client context
/// });
///
/// // pump thread:
/// while (running) {
///     inbound.drain();
///     client.poll(now());
/// }
/// \endcode
///
/// Note the copy in the driver-thread lambda. Inbound buffers are borrowed for
/// the duration of the transport callback (docs/design.md section 9), so
/// anything crossing threads has to own its bytes.

#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

namespace smply {

/// A thread-safe queue of closures, posted from any thread and run on one.
///
/// Multi-producer, single-consumer. `post()` may be called from any thread;
/// `drain()` must only ever be called from the client context, because that is
/// the thread the closures are written to run on.
///
/// Non-copyable and non-movable: a queue with a mutex in it has no meaningful
/// move, and adapters hold it by reference.
class Dispatcher
{
public:
    /// \param on_wake Called after a closure is queued, to nudge a pump that
    ///                may be blocked waiting for work. Optional.
    ///
    ///                **It runs inside `post()`, on the posting thread**, with
    ///                the dispatcher's lock *not* held. It must not call
    ///                `drain()`, and it must not block on the client context --
    ///                that is a lock-order inversion waiting to deadlock. Set an
    ///                event, signal a condition variable, post a window
    ///                message; do not do work.
    explicit Dispatcher(std::function<void()> on_wake = {});

    Dispatcher(const Dispatcher&) = delete;
    Dispatcher(Dispatcher&&) = delete;
    Dispatcher& operator=(const Dispatcher&) = delete;
    Dispatcher& operator=(Dispatcher&&) = delete;

    /// Destroys any closures still queued **without running them**, like
    /// `clear()`. A pending closure names a client context that is, by the time
    /// this runs, going away.
    ~Dispatcher();

    /// Queues \p work. Callable from any thread.
    ///
    /// An empty `std::function` is ignored rather than queued, so a caller that
    /// builds a closure conditionally does not have to check.
    void post(std::function<void()> work);

    /// Runs everything queued *at the moment of the call*, in the order it was
    /// posted, and returns how many ran. **Client context only.**
    ///
    /// The queue is taken under the lock and run outside it, which has two
    /// consequences that are the point rather than side effects:
    ///
    /// * a closure may `post()` freely -- it cannot deadlock, and what it posts
    ///   runs on the **next** drain, not this one, so a closure that reposts
    ///   itself cannot spin here forever;
    /// * a closure may destroy things, block, or call back into the client,
    ///   without holding up a producer thread.
    ///
    /// Re-entrant use is refused: a `drain()` called from inside a closure
    /// returns 0 immediately and runs nothing, the same answer
    /// `MessageAssembler` gives a re-entrant `feed()` (docs/design.md section
    /// 2). The work stays queued for the outer drain to pick up.
    std::size_t drain();

    /// Discards everything queued without running it.
    ///
    /// For an adapter tearing down a link: the closures reference a transport
    /// that is going away, and running them would be worse than dropping them.
    /// Callable from any thread.
    void clear() noexcept;

    /// How many closures are queued. Racy by nature -- a producer may add one
    /// before the value is read. Intended for diagnostics and tests, not for
    /// deciding whether to drain.
    [[nodiscard]] std::size_t pending() const;

private:
    /// Takes the queue and leaves an empty one behind, under the lock.
    [[nodiscard]] std::vector<std::function<void()>> take();

    mutable std::mutex mutex_;
    std::vector<std::function<void()>> queue_;
    std::function<void()> on_wake_;
    /// Guards against re-entrant `drain()`. Only ever touched on the client
    /// context, so it needs no synchronisation of its own.
    bool draining_ = false;
};

} // namespace smply

#endif // SMPLY_UTIL_DISPATCHER_HPP
