// SPDX-License-Identifier: Apache-2.0

#include "dfu_app/update_run.hpp"

#include <chrono>
#include <utility>
#include <variant>

namespace smply::dfu_app {

namespace {

/// The earlier of two optional points, where absent means "never".
[[nodiscard]] std::optional<TimePoint> earliest(std::optional<TimePoint> a,
                                                std::optional<TimePoint> b) noexcept
{
    if (!a.has_value()) {
        return b;
    }
    if (!b.has_value()) {
        return a;
    }
    return *a < *b ? a : b;
}

} // namespace

UpdateRun::UpdateRun(SmpClient& client, FirmwareUpdater& updater, UpdateWait& wait,
                     UpdateRunSettings settings, UpdateRunHooks hooks)
    : client_{client}, updater_{updater}, wait_{wait}, settings_{std::move(settings)},
      hooks_{std::move(hooks)}, policy_{settings_.reconnect},
      inbox_{std::make_shared<std::deque<UpdateEvent>>()}
{}

UpdateEventCallback UpdateRun::event_handler() const
{
    // Runs inside poll(), or inside whatever library call raised the event --
    // so it does nothing but queue. Shared ownership of the queue, not a
    // pointer to the run: the updater's destructor completes a running update,
    // and a run declared after the updater is gone by then.
    return [inbox = inbox_](const UpdateEvent& event) { inbox->push_back(event); };
}

UpdateRunOutcome UpdateRun::run(ImageSource& source, const UpdatePlan& plan)
{
    return started(running_ ? fail(ErrorCode::InvalidState, "update run: an update is running")
                            : updater_.start(source, plan, event_handler()));
}

UpdateRunOutcome UpdateRun::run(std::span<const ImageTarget> targets, const UpdatePlan& plan)
{
    return started(running_ ? fail(ErrorCode::InvalidState, "update run: an update is running")
                            : updater_.start(targets, plan, event_handler()));
}

UpdateRunOutcome UpdateRun::started(Result<void> start)
{
    if (!start.has_value()) {
        return UpdateRunOutcome{RunEnd::Refused, fail(start.error()), false};
    }
    running_ = true;
    return run();
}

void UpdateRun::refused(const char* call)
{
    internal_ = Error{ErrorCode::Internal, call};
    updater_.cancel();
}

UpdateRunOutcome UpdateRun::run()
{
    if (!running_) {
        return UpdateRunOutcome{RunEnd::Refused,
                                fail(ErrorCode::InvalidState, "update run: no update to carry on"),
                                false};
    }
    const std::optional<TimePoint> deadline =
        settings_.overall_timeout.has_value()
            ? std::optional<TimePoint>{wait_.now() + *settings_.overall_timeout}
            : std::nullopt;
    bool gave_up = false;

    for (;;) {
        const TimePoint now = wait_.now();
        client_.poll(now);
        updater_.poll(now);
        deliver();

        if (finished_.has_value()) {
            UpdateRunOutcome outcome{RunEnd::Finished, std::move(*finished_), gave_up};
            if (internal_.has_value()) {
                // The cancellation was the run's own doing; say why.
                outcome.result = fail(std::move(*internal_));
                internal_.reset();
            }
            finished_.reset();
            running_ = false;
            return outcome;
        }

        if (reconnect_pending_) {
            switch (reconnect(deadline)) {
            case Episode::Rebound:
                reconnect_pending_ = false;
                break;
            case Episode::GaveUp:
                reconnect_pending_ = false;
                gave_up = true;
                break;
            case Episode::TimedOut:
                // Still owed: the updater waits in AwaitingReconnect with no
                // deadline of its own and will not ask again, so the next
                // run() must reconnect, with a fresh episode of its own.
                return UpdateRunOutcome{RunEnd::TimedOut,
                                        fail(ErrorCode::Timeout, "update run: overall deadline"),
                                        gave_up};
            }
            continue; // poll at once: the updater has work to do
        }

        if (confirm_pending_) {
            confirm_pending_ = false;
            const Approval approval = hooks_.approve ? hooks_.approve() : Approval::Confirm;
            switch (approval) {
            case Approval::Confirm:
                // The run saw ConfirmationRequired and nothing has polled
                // since, so the update is still waiting for exactly this.
                if (!updater_.confirm().has_value()) {
                    refused("update run: FirmwareUpdater::confirm() refused");
                }
                break;
            case Approval::Stop:
                return UpdateRunOutcome{
                    RunEnd::StoppedBeforeConfirm,
                    fail(ErrorCode::InvalidState, "update run: stopped before confirming"),
                    gave_up};
            }
            continue;
        }

        if (deadline.has_value() && wait_.now() >= *deadline) {
            return UpdateRunOutcome{RunEnd::TimedOut,
                                    fail(ErrorCode::Timeout, "update run: overall deadline"),
                                    gave_up};
        }

        wait_.wait_until(next_wake(deadline));
    }
}

void UpdateRun::deliver()
{
    // Each event is popped before observe() sees it, so the queue is whole at
    // every call: should observe() call the library after all -- it is meant
    // for output -- any event that raises is queued behind the rest and
    // delivered, in order, by this same loop.
    while (!inbox_->empty()) {
        const UpdateEvent event = std::move(inbox_->front());
        inbox_->pop_front();

        if (std::holds_alternative<ReconnectRequired>(event)) {
            reconnect_pending_ = true;
        } else if (std::holds_alternative<ConfirmationRequired>(event)) {
            confirm_pending_ = true;
        } else if (const auto* finished = std::get_if<UpdateFinished>(&event)) {
            finished_ = finished->result;
        }

        if (hooks_.observe) {
            hooks_.observe(event);
        }
    }
}

UpdateRun::Episode UpdateRun::reconnect(std::optional<TimePoint> deadline)
{
    std::optional<Error> why;
    if (hooks_.open_link) {
        policy_.begin();
        while (!policy_.exhausted()) {
            if (deadline.has_value() && wait_.now() >= *deadline) {
                return Episode::TimedOut;
            }
            const Duration delay = policy_.next_delay();
            if (deadline.has_value()) {
                // Never sleep past the deadline and then open a link anyway:
                // a delay that reaches it is cut to the time left, and ends
                // the episode there. Rounded up, so the comparison with a
                // whole-millisecond delay is exact.
                const Duration left = std::chrono::ceil<Duration>(*deadline - wait_.now());
                if (delay >= left) {
                    wait_.sleep_for(left);
                    return Episode::TimedOut;
                }
            }
            wait_.sleep_for(delay);

            const LinkAttempt attempt = hooks_.open_link(ReconnectAttempt{
                .number = policy_.attempts(),
                .max_attempts = policy_.settings().max_attempts,
                .waited = delay,
            });
            if (Transport* link = attempt.link(); link != nullptr) {
                // The order is the rule: the client must hold the new link
                // before the updater resumes and sends on it.
                client_.rebind_transport(*link);
                policy_.succeeded();
                // The run saw ReconnectRequired and nothing has polled since,
                // so the update is still waiting for exactly this.
                if (!updater_.resume_after_reconnect().has_value()) {
                    refused("update run: FirmwareUpdater::resume_after_reconnect() refused");
                }
                return Episode::Rebound;
            }
            if (attempt.error().has_value()) {
                why = attempt.error();
            }
            if (attempt.gives_up()) {
                break;
            }
        }
    }
    // Terminal, and the updater has to be told: it is waiting on the
    // application with no deadline of its own, and would wait for ever.
    updater_.reconnect_failed(why.value_or(settings_.unreachable));
    return Episode::GaveUp;
}

std::optional<TimePoint> UpdateRun::next_wake(std::optional<TimePoint> deadline) const
{
    return earliest(earliest(client_.next_deadline(), updater_.next_deadline()), deadline);
}

} // namespace smply::dfu_app
