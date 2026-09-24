// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_ASYNC_FUTURE_HPP
#define SMPLY_ASYNC_FUTURE_HPP

/// \file
/// `std::future` over smply's callbacks, for code on a thread **other than**
/// the pump (target `smply::asyncutil`, ADR-0019).
///
/// **Never call `get()` or `wait()` on one of these futures from the pump
/// thread.** smply completes an operation only from `SmpClient::poll()` or
/// `on_bytes()` on the pump thread (ADR-0004). A pump thread blocked on the
/// future therefore waits for a callback that only it can run, and never
/// returns. Nothing can detect this reliably, so it is a rule, not a check.
/// Code that runs on the pump thread should use `smply/async/task.hpp`.
///
/// The operation is started through a `smply::Dispatcher`, the same queue an
/// adapter uses to hand inbound bytes to the pump. So it begins on the pump
/// thread the next time the application drains that dispatcher, which is what
/// ADR-0004 requires of every call into the client:
/// \code
///     // On a worker thread, while the pump thread drains `dispatcher`:
///     std::future<smply::Result<smply::ImageState>> state =
///         smply::async::post_for_future<smply::ImageState>(
///             dispatcher, [&](auto done) { images.get_state(std::move(done)); });
///     const smply::Result<smply::ImageState> result = state.get();
/// \endcode
///
/// If the dispatcher is cleared or destroyed before the work runs, the promise
/// is broken, and `get()` throws `std::future_error` rather than blocking
/// forever.

#include "smply/result.hpp"
#include "smply/util/dispatcher.hpp"

#include <future>
#include <memory>
#include <utility>

namespace smply::async {

/// Starts an operation on the pump thread and returns a future for its result.
///
/// \tparam T         The operation's result type: its callback is
///                   `Callback<T>`.
/// \param dispatcher The dispatcher the pump thread drains. Posting to it is
///                   thread-safe.
/// \param start      Called on the pump thread with the callback to hand to
///                   the operation. Copyable, because `Dispatcher` stores
///                   `std::function`. Whatever it captures must stay valid
///                   until the pump runs it.
/// \return A future fulfilled with the operation's `Result<T>` when its
///         callback runs.
template<class T, class Start>
[[nodiscard]] std::future<Result<T>> post_for_future(Dispatcher& dispatcher, Start start)
{
    // Shared, because std::promise is move-only and Dispatcher stores
    // std::function, which must be copyable.
    auto promise = std::make_shared<std::promise<Result<T>>>();
    std::future<Result<T>> future = promise->get_future();
    dispatcher.post([promise, start = std::move(start)]() mutable {
        start(Callback<T>{[promise](Result<T> result) { promise->set_value(std::move(result)); }});
    });
    return future;
}

} // namespace smply::async

#endif // SMPLY_ASYNC_FUTURE_HPP
