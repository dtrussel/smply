// SPDX-License-Identifier: Apache-2.0

#include "dfu_app/update_run.hpp"

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

UpdateRunOutcome UpdateRun::run()
{
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
            finished_.reset();
            return outcome;
        }

        if (reconnect_pending_) {
            reconnect_pending_ = false;
            switch (reconnect(deadline)) {
            case Episode::Rebound:
                break;
            case Episode::GaveUp:
                gave_up = true;
                break;
            case Episode::TimedOut:
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
                // Refused only outside AwaitingConfirmation, which a cancel in
                // the meantime would explain; the updater reports that itself.
                static_cast<void>(updater_.confirm());
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
    // Popped one at a time: observe() may not touch the library, but the queue
    // must stay consistent if it does.
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
                // Refused only outside AwaitingReconnect, which a cancel in
                // the meantime would explain; the updater reports that itself.
                static_cast<void>(updater_.resume_after_reconnect());
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
