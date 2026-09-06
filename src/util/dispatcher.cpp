// SPDX-License-Identifier: Apache-2.0

#include "smply/util/dispatcher.hpp"

#include <cstddef>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace smply {
namespace {

/// Clears a flag on the way out, however the scope is left.
///
/// `drain()` runs application closures. The library throws nothing itself, but
/// those do not belong to it, and a dispatcher left permanently "draining"
/// would silently stop delivering inbound bytes for the rest of the process's
/// life -- a far worse failure than the exception that caused it.
class FlagGuard
{
public:
    explicit FlagGuard(bool& flag) noexcept : flag_{&flag} {}

    FlagGuard(const FlagGuard&) = delete;
    FlagGuard(FlagGuard&&) = delete;
    FlagGuard& operator=(const FlagGuard&) = delete;
    FlagGuard& operator=(FlagGuard&&) = delete;

    ~FlagGuard()
    {
        *flag_ = false;
    }

private:
    bool* flag_;
};

} // namespace

// on_wake_ is written once here and only ever read afterwards, so post() can
// read it from any thread without synchronisation: constructing the dispatcher
// necessarily happens-before any call on it.
Dispatcher::Dispatcher(std::function<void()> on_wake) : on_wake_{std::move(on_wake)} {}

Dispatcher::~Dispatcher()
{
    clear();
}

void Dispatcher::post(std::function<void()> work)
{
    if (!work) {
        return;
    }

    {
        const std::lock_guard<std::mutex> lock{mutex_};
        queue_.push_back(std::move(work));
    }

    // Outside the lock, deliberately. The wake callback is application code: it
    // may take the application's own locks, and holding ours across it is how a
    // lock-order inversion gets built by accident.
    if (on_wake_) {
        on_wake_();
    }
}

std::vector<std::function<void()>> Dispatcher::take()
{
    const std::lock_guard<std::mutex> lock{mutex_};
    std::vector<std::function<void()>> taken;
    taken.swap(queue_);
    return taken;
}

std::size_t Dispatcher::drain()
{
    if (draining_) {
        // Re-entrant call from inside a closure. Refused rather than recursed
        // into: the work stays queued and the outer drain takes it, so nothing
        // is lost and the stack cannot grow without bound. See the header.
        return 0;
    }

    const std::vector<std::function<void()>> work = take();
    if (work.empty()) {
        return 0;
    }

    draining_ = true;
    const FlagGuard guard{draining_};

    for (const std::function<void()>& item : work) {
        item();
    }

    // `work` is destroyed after this returns, outside the lock: a closure's
    // captures may own anything, and their destructors are application code
    // too.
    return work.size();
}

void Dispatcher::clear() noexcept
{
    // Taken under the lock, destroyed outside it -- destroying a closure runs
    // its captures' destructors, which are application code that may itself
    // post. The temporary dies at the end of this full expression, by which
    // point take() has released the lock.
    static_cast<void>(take());
}

std::size_t Dispatcher::pending() const
{
    const std::lock_guard<std::mutex> lock{mutex_};
    return queue_.size();
}

} // namespace smply
