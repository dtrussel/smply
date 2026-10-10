// SPDX-License-Identifier: Apache-2.0

#include "dfu_app/dispatcher_wait.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace smply::dfu_app {

struct DispatcherWait::Signal
{
    std::mutex mutex;
    std::condition_variable wake;
    bool woken = false;
};

DispatcherWait::DispatcherWait() : signal_{std::make_shared<Signal>()} {}

std::function<void()> DispatcherWait::waker() const
{
    return [signal = signal_] {
        // Runs on the posting thread, inside Dispatcher::post(). Signal and
        // return: doing work here, or taking a lock the client context holds,
        // is how an adapter deadlocks.
        {
            const std::lock_guard<std::mutex> lock{signal->mutex};
            signal->woken = true;
        }
        signal->wake.notify_one();
    };
}

TimePoint DispatcherWait::now()
{
    return std::chrono::steady_clock::now();
}

void DispatcherWait::wait_until(std::optional<TimePoint> deadline)
{
    {
        std::unique_lock<std::mutex> lock{signal_->mutex};
        if (deadline.has_value()) {
            signal_->wake.wait_until(lock, *deadline, [this] { return signal_->woken; });
        } else {
            signal_->wake.wait_for(lock, kIdleWait, [this] { return signal_->woken; });
        }
        signal_->woken = false;
    }
    if (dispatcher_ != nullptr) {
        static_cast<void>(dispatcher_->drain());
    }
}

void DispatcherWait::sleep_for(Duration delay)
{
    std::this_thread::sleep_for(delay);
}

} // namespace smply::dfu_app
