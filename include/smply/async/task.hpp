// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_ASYNC_TASK_HPP
#define SMPLY_ASYNC_TASK_HPP

/// \file
/// C++20 coroutines over smply's callbacks (target `smply::asyncutil`,
/// ADR-0019).
///
/// Every smply operation reports through a `Callback<T>`. `await_result<T>()`
/// turns one of them into something a coroutine can `co_await`, and `Task<T>`
/// is a minimal coroutine type to write such coroutines in:
/// \code
///     smply::async::Task<void> install(smply::ImageManagement& images, Ui& ui)
///     {
///         const smply::Result<smply::ImageState> state =
///             co_await smply::async::await_result<smply::ImageState>(
///                 [&](auto done) { images.get_state(std::move(done)); });
///         if (!state) {
///             ui.error(smply::to_string(state.error()));
///             co_return;
///         }
///         ui.show(*state);
///     }
/// \endcode
///
/// **Nothing about the threading model changes** (ADR-0004). A coroutine
/// resumes inside the completion callback, which smply only ever runs from
/// `SmpClient::poll()` or `on_bytes()` on the client context. So the coroutine
/// always runs on the pump thread, exactly where a callback chain would
/// continue, and the application still has to pump for anything to happen.
///
/// **Destroying a suspended `Task` does not cancel what it awaits.** The
/// operation still completes, and its result is dropped instead of resuming a
/// coroutine that no longer exists. Cancel through the handle the operation
/// returned if cancellation matters.

#include "smply/result.hpp"

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace smply::async {

namespace detail {

/// Where an awaited result waits for its coroutine. Shared between the
/// awaitable and the callback, so either may outlive the other.
template<class T>
struct AwaitState
{
    std::optional<Result<T>> result;
    std::coroutine_handle<> waiter;
};

} // namespace detail

/// Awaits one callback-reporting operation. Made by `await_result()`.
template<class T, class Start>
class ResultAwaitable
{
public:
    /// \param start Called with the `Callback<T>` to pass to the operation.
    explicit ResultAwaitable(Start start)
        : start_{std::move(start)}, state_{std::make_shared<detail::AwaitState<T>>()}
    {}

    ResultAwaitable(const ResultAwaitable&) = delete;
    ResultAwaitable& operator=(const ResultAwaitable&) = delete;
    ResultAwaitable(ResultAwaitable&&) noexcept = default;
    ResultAwaitable& operator=(ResultAwaitable&&) noexcept = default;

    /// Detaches from a result that has not arrived: the coroutine is being
    /// destroyed, so the callback must not resume it.
    ~ResultAwaitable()
    {
        if (state_) {
            state_->waiter = {};
        }
    }

    /// Never ready before the operation has even started.
    ///
    /// Not `static`, although it could be: the coroutine machinery calls every
    /// awaiter and promise hook through an object, so a static one trips
    /// `readability-static-accessed-through-instance` at every `co_await` in
    /// the application instead. The same holds for the promise hooks below.
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    [[nodiscard]] bool await_ready() const noexcept
    {
        return false;
    }

    /// Starts the operation, and suspends unless it has already completed.
    ///
    /// smply never completes inside the call that started an operation
    /// (ADR-0003), but an application's own operation might, so that case
    /// continues without suspending rather than resuming from inside itself.
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> waiter)
    {
        start_(Callback<T>{[state = state_](Result<T> result) {
            state->result.emplace(std::move(result));
            if (const std::coroutine_handle<> resume = std::exchange(state->waiter, {})) {
                resume.resume();
            }
        }});
        if (state_->result.has_value()) {
            return false;
        }
        state_->waiter = waiter;
        return true;
    }

    /// The operation's result.
    [[nodiscard]] Result<T> await_resume()
    {
        return std::move(*state_->result);
    }

private:
    Start start_;
    std::shared_ptr<detail::AwaitState<T>> state_;
};

/// Makes a smply operation awaitable.
///
/// \tparam T    The operation's result type: its callback is `Callback<T>`.
/// \param start Called once, when the coroutine awaits, with the callback to
///              hand to the operation:
///              `[&](auto done) { images.get_state(std::move(done)); }`.
///              Whatever it returns (a `RequestHandle`, an `UploadHandle`) is
///              discarded; start the operation yourself, outside the lambda's
///              return, if you need the handle.
/// \return An awaitable whose `co_await` yields the `Result<T>`.
template<class T, class Start>
[[nodiscard]] ResultAwaitable<T, std::decay_t<Start>> await_result(Start&& start)
{
    return ResultAwaitable<T, std::decay_t<Start>>{std::forward<Start>(start)};
}

/// Forward declaration; defined below.
template<class T = void>
class Task;

namespace detail {

/// What every `Task` promise shares: eager start, and a final suspension that
/// hands control to whoever awaited the task.
class TaskPromiseBase
{
public:
    /// Resumes the awaiting coroutine, if there is one, when the task ends.
    struct FinalAwaiter
    {
        // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
        [[nodiscard]] bool await_ready() const noexcept
        {
            return false;
        }

        template<class Promise>
        [[nodiscard]] std::coroutine_handle<>
        await_suspend(std::coroutine_handle<Promise> finished) const noexcept
        {
            if (std::coroutine_handle<> next = finished.promise().continuation) {
                return next;
            }
            return std::noop_coroutine();
        }

        void await_resume() const noexcept {}
    };

    /// Eager: the body runs as soon as the coroutine is called.
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    [[nodiscard]] std::suspend_never initial_suspend() const noexcept
    {
        return {};
    }

    /// Stays suspended at the end, so the result outlives the body.
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    [[nodiscard]] FinalAwaiter final_suspend() const noexcept
    {
        return {};
    }

    /// Kept for `Task::result()` to rethrow.
    void unhandled_exception() noexcept
    {
        exception = std::current_exception();
    }

    /// The coroutine awaiting this task, if any.
    std::coroutine_handle<> continuation;
    /// What escaped the body, if anything.
    std::exception_ptr exception;
};

/// The promise of a `Task<T>` that returns a value.
template<class T>
class TaskPromise : public TaskPromiseBase
{
public:
    [[nodiscard]] Task<T> get_return_object() noexcept;

    /// Stores what `co_return` returns.
    template<class U>
    void return_value(U&& returned)
    {
        value.emplace(std::forward<U>(returned));
    }

    /// Set by `co_return`.
    std::optional<T> value;
};

/// The promise of a `Task<void>`.
template<>
class TaskPromise<void> : public TaskPromiseBase
{
public:
    [[nodiscard]] Task<void> get_return_object() noexcept;

    /// `co_return;`, or falling off the end.
    void return_void() const noexcept {}
};

} // namespace detail

/// A coroutine that starts at once and runs, on the pump thread, until it
/// finishes.
///
/// Owns its coroutine frame: destroying a `Task` destroys the coroutine, even
/// one still suspended in an `await_result()` (see the file comment). Move-only.
///
/// A `Task` can itself be awaited from another `Task`. The awaited task must
/// not be destroyed while the awaiting coroutine is suspended on it, which is
/// automatic when the awaited task is a local or a temporary of the awaiting
/// coroutine.
template<class T>
class [[nodiscard]] Task
{
public:
    /// The coroutine promise.
    using promise_type = detail::TaskPromise<T>;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    /// Takes over \p other's coroutine; \p other is left empty.
    Task(Task&& other) noexcept : handle_{std::exchange(other.handle_, {})} {}

    /// Destroys this task's coroutine and takes over \p other's.
    Task& operator=(Task&& other) noexcept
    {
        if (this != &other) {
            destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    /// Destroys the coroutine frame, suspended or finished.
    ~Task()
    {
        destroy();
    }

    /// True once the body has finished, by `co_return` or by an exception.
    [[nodiscard]] bool done() const noexcept
    {
        return handle_ && handle_.done();
    }

    /// What the body returned. Rethrows what escaped it, if anything.
    ///
    /// Precondition: `done()`. Calling it earlier terminates.
    T result()
        requires(!std::is_void_v<T>)
    {
        check_done();
        return std::move(*handle_.promise().value);
    }

    /// \overload For `Task<void>`: returns once the body finished normally.
    void result()
        requires std::is_void_v<T>
    {
        check_done();
    }

    /// Awaiting a finished task does not suspend.
    [[nodiscard]] bool await_ready() const noexcept
    {
        return done();
    }

    /// Resumes \p awaiting when this task finishes.
    void await_suspend(std::coroutine_handle<> awaiting) noexcept
    {
        handle_.promise().continuation = awaiting;
    }

    /// The task's result, as `result()` gives it.
    decltype(auto) await_resume()
    {
        return result();
    }

private:
    friend promise_type;

    explicit Task(std::coroutine_handle<promise_type> handle) noexcept : handle_{handle} {}

    void check_done()
    {
        if (!done()) {
            std::terminate(); // LCOV_EXCL_LINE: a precondition violation, not a path
        }
        if (handle_.promise().exception) {
            std::rethrow_exception(handle_.promise().exception);
        }
    }

    void destroy() noexcept
    {
        if (handle_) {
            handle_.destroy();
            handle_ = {};
        }
    }

    std::coroutine_handle<promise_type> handle_;
};

namespace detail {

template<class T>
Task<T> TaskPromise<T>::get_return_object() noexcept
{
    return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept
{
    return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

} // namespace detail

} // namespace smply::async

#endif // SMPLY_ASYNC_TASK_HPP
