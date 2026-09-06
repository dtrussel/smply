// SPDX-License-Identifier: Apache-2.0

#include "dfu/update_state_machine.hpp"

#include "smply/error.hpp"
#include "smply/groups/image.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace smply::dfu {
namespace {

/// Terminal failure, recording why.
[[nodiscard]] Step fail(Context& context, Error error)
{
    // A swap that was scheduled and never confirmed does not un-schedule itself
    // because the client gave up: the device will revert on its next reset, and
    // a caller that reads only "failed" would believe nothing had changed.
    context.revert_pending = context.swap_scheduled;
    context.cause = std::move(error);
    return Step{UpdateState::Failed, Effect::Finish};
}

[[nodiscard]] Step fail(Context& context, ErrorCode code, const char* where)
{
    return fail(context, Error{code, where});
}

/// The slot of \p state that is running.
[[nodiscard]] const ImageSlot* active_of(const Context& context, std::uint32_t image)
{
    if (!context.device.has_value()) {
        return nullptr;
    }
    return context.device->active_slot(image);
}

/// Any slot marked to boot next.
[[nodiscard]] bool anything_pending(const Context& context)
{
    if (!context.device.has_value()) {
        return false;
    }
    const std::vector<ImageSlot>& slots = context.device->slots;
    return std::ranges::any_of(slots, [](const ImageSlot& slot) { return slot.pending; });
}

/// The slot holding the image being installed, or nullptr.
[[nodiscard]] const ImageSlot* slot_with_target(const Context& context)
{
    if (!context.device.has_value() || context.target.empty()) {
        return nullptr;
    }
    return context.device->find_by_hash(context.target);
}

/// Whether the update should stop once the device merely holds the image.
[[nodiscard]] bool upload_only(const UpdatePlan& plan)
{
    return plan.mode == UpdateMode::UploadOnly;
}

/// The fork ADR-0014 introduced: ask, or confirm without asking.
[[nodiscard]] Step confirmation_fork(const UpdatePlan& plan)
{
    if (plan.mode == UpdateMode::ConfirmImmediately) {
        return Step{UpdateState::Confirming, Effect::Confirm};
    }
    return Step{UpdateState::AwaitingConfirmation, Effect::RequestConfirmation};
}

/// Decides what, if anything, needs doing -- from the slot table alone.
///
/// Four cases, in the order they are checked. The first two exist because an
/// update may be resumed by a *new* process against a device that is already
/// part-way through one, which is exactly what happens when an application is
/// restarted mid-update.
[[nodiscard]] Step plan_from_state(const UpdatePlan& plan, Context& context)
{
    const ImageSlot* active = active_of(context, plan.image);
    const ImageSlot* holder = slot_with_target(context);

    // 1. The device is already running the image being installed.
    if (active != nullptr && holder == active) {
        if (active->confirmed || upload_only(plan)) {
            context.upload_skipped = true;
            return Step{UpdateState::Completed, Effect::Finish};
        }
        // Running it unconfirmed: a trial boot is in progress and this is the
        // confirmation window, whoever started it.
        context.upload_skipped = true;
        context.swap_scheduled = true;
        return confirmation_fork(plan);
    }

    // 2. Another slot holds it, already marked for the next boot. The mark
    //    succeeded at some point even if we never saw the response.
    if (holder != nullptr && holder->pending) {
        context.upload_skipped = true;
        context.swap_scheduled = true;
        return upload_only(plan) ? Step{UpdateState::Completed, Effect::Finish}
                                 : Step{UpdateState::Resetting, Effect::Reset};
    }

    // 3. Another slot holds it, unmarked.
    if (holder != nullptr && plan.skip_if_already_present) {
        context.upload_skipped = true;
        return upload_only(plan) ? Step{UpdateState::Completed, Effect::Finish}
                                 : Step{UpdateState::MarkingForTest, Effect::MarkForTest};
    }

    // 4. Upload it. Even here the device may answer "already present" on the
    //    first packet and finish without a transfer (rule 9a).
    context.upload_in_progress = true;
    return Step{UpdateState::Uploading, Effect::StartUpload};
}

/// `VerifyingBooted`: did the device come up on the new image, or revert?
[[nodiscard]] Step inspect_boot(const UpdatePlan& plan, Context& context)
{
    const ImageSlot* active = active_of(context, plan.image);
    if (active == nullptr) {
        return fail(context, ErrorCode::UpdateFailed, "dfu: no active slot after reboot");
    }

    const bool running_target = active->hash.has_value() && *active->hash == context.target;
    if (!running_target) {
        // The signature of a revert: the device is running something else and
        // nothing is queued to change that. If something *is* pending the swap
        // simply has not happened, which is a different failure.
        if (!anything_pending(context)) {
            context.rolled_back = true;
            context.swap_scheduled = false;
            return fail(context, ErrorCode::UpdateFailed, "dfu: device reverted to the old image");
        }
        return fail(context, ErrorCode::UpdateFailed, "dfu: device did not boot the new image");
    }

    // It booted ours. An already-confirmed image needs nothing further --
    // a `ConfirmImmediately` update whose confirm response was lost lands here.
    if (active->confirmed) {
        context.swap_scheduled = false;
        return Step{UpdateState::Completed, Effect::Finish};
    }

    context.swap_scheduled = true;
    return confirmation_fork(plan);
}

} // namespace

Step advance(UpdateState state, const Event& event, const UpdatePlan& plan, Context& context)
{
    // Cancellation is legal everywhere and looks the same everywhere, so it is
    // handled once rather than as a row of every state below.
    if (event.kind == Event::Kind::Cancel) {
        if (is_terminal(state)) {
            return Step{state, Effect::None};
        }
        context.revert_pending = context.swap_scheduled;
        return Step{UpdateState::Cancelled, Effect::Finish};
    }

    switch (state) {
    case UpdateState::Idle:
        if (event.kind == Event::Kind::Start) {
            return Step{UpdateState::QueryingParameters, Effect::QueryParameters};
        }
        break;

    case UpdateState::QueryingParameters:
        // The command is optional, and a device without it is ordinary rather
        // than broken (docs/protocol-notes.md section 9, A8).
        if (event.kind == Event::Kind::ParametersRead) {
            context.buf_size = event.buf_size;
            return Step{UpdateState::InspectingImages, Effect::ReadState};
        }
        if (event.kind == Event::Kind::ParametersUnavailable) {
            return Step{UpdateState::InspectingImages, Effect::ReadState};
        }
        break;

    case UpdateState::InspectingImages:
        if (event.kind == Event::Kind::StateRead) {
            context.device = *event.state;
            return Step{UpdateState::Planning, Effect::Continue};
        }
        if (event.kind == Event::Kind::Failed) {
            // Nothing has been changed on the device yet.
            return fail(context, event.error);
        }
        break;

    case UpdateState::Planning:
        if (event.kind == Event::Kind::Continue) {
            return plan_from_state(plan, context);
        }
        break;

    case UpdateState::Uploading:
        if (event.kind == Event::Kind::UploadFinished) {
            context.upload_in_progress = false;
            context.bytes_transferred = event.transferred;
            if (upload_only(plan)) {
                return Step{UpdateState::Completed, Effect::Finish};
            }
            return Step{UpdateState::VerifyingUpload, Effect::ReadState};
        }
        if (event.kind == Event::Kind::Failed) {
            // A dropped link suspends the transfer rather than ending it: the
            // device keeps the session and resumes by `sha` (section 6, rule 6).
            if (event.error.code() == ErrorCode::Disconnected) {
                return Step{UpdateState::AwaitingReconnect, Effect::RequestReconnect};
            }
            return fail(context, event.error);
        }
        break;

    case UpdateState::VerifyingUpload:
        if (event.kind == Event::Kind::StateRead) {
            context.device = *event.state;
            if (slot_with_target(context) == nullptr) {
                // The device does not report holding what was just sent.
                return fail(context, ErrorCode::ImageMismatch,
                            "dfu: uploaded image not present in any slot");
            }
            return Step{UpdateState::MarkingForTest, Effect::MarkForTest};
        }
        if (event.kind == Event::Kind::Failed) {
            return fail(context, event.error);
        }
        break;

    case UpdateState::MarkingForTest:
        if (event.kind == Event::Kind::MarkedForTest) {
            context.swap_scheduled = true;
            return Step{UpdateState::Resetting, Effect::Reset};
        }
        if (event.kind == Event::Kind::Failed) {
            // `ImageAlreadyPending` means a swap is scheduled -- possibly the
            // one this request was asking for, whose response was lost. Read
            // the state back and let the planner decide; case 2 there sees our
            // own image already marked and moves on.
            const std::optional<ImageError> code = image_error(event.error);
            if (code == ImageError::ImageAlreadyPending && !context.mark_retried) {
                context.mark_retried = true;
                return Step{UpdateState::InspectingImages, Effect::ReadState};
            }
            return fail(context, event.error);
        }
        break;

    case UpdateState::Resetting:
        if (event.kind == Event::Kind::ResetAccepted) {
            return Step{UpdateState::AwaitingDisconnect, Effect::AwaitDisconnect};
        }
        if (event.kind == Event::Kind::Failed) {
            // A reset hook may refuse with `Busy`, which invites one retry with
            // `force` (docs/protocol-notes.md section 5).
            if (smp_error(event.error) == SmpError::Busy && !context.reset_forced) {
                context.reset_forced = true;
                return Step{UpdateState::Resetting, Effect::ForceReset};
            }
            // Losing the response is normal: the device may reset before the
            // answer goes out (A3). A drop, or silence, is treated as the reset
            // having happened -- the verify step after the reboot is the real
            // check, and failing here would abandon a device that is already
            // swapping.
            if (event.error.code() == ErrorCode::Disconnected ||
                event.error.code() == ErrorCode::Timeout) {
                return Step{UpdateState::AwaitingDisconnect, Effect::AwaitDisconnect};
            }
            return fail(context, event.error);
        }
        break;

    case UpdateState::AwaitingDisconnect:
        // Either way the application is asked to reconnect. A link that never
        // dropped is not proof the device ignored the reset, so the grace
        // expiring is not a failure.
        if (event.kind == Event::Kind::Disconnected || event.kind == Event::Kind::GraceExpired) {
            return Step{UpdateState::AwaitingReconnect, Effect::RequestReconnect};
        }
        break;

    case UpdateState::AwaitingReconnect:
        if (event.kind == Event::Kind::Reconnected) {
            if (context.upload_in_progress) {
                return Step{UpdateState::Uploading, Effect::ResumeUpload};
            }
            return Step{UpdateState::VerifyingBooted, Effect::ReadState};
        }
        if (event.kind == Event::Kind::ReconnectFailed) {
            return fail(context, event.error);
        }
        break;

    case UpdateState::VerifyingBooted:
        if (event.kind == Event::Kind::StateRead) {
            context.device = *event.state;
            return inspect_boot(plan, context);
        }
        if (event.kind == Event::Kind::Failed) {
            return fail(context, event.error);
        }
        break;

    case UpdateState::AwaitingConfirmation:
        if (event.kind == Event::Kind::ConfirmApproved) {
            return Step{UpdateState::Confirming, Effect::Confirm};
        }
        break;

    case UpdateState::Confirming:
        if (event.kind == Event::Kind::Confirmed) {
            context.swap_scheduled = false;
            return Step{UpdateState::VerifyingConfirmed, Effect::ReadState};
        }
        if (event.kind == Event::Kind::Failed) {
            // A refused confirm leaves a running, unconfirmed image: the device
            // reverts on its next reset. fail() records that.
            return fail(context, event.error);
        }
        break;

    case UpdateState::VerifyingConfirmed:
        if (event.kind == Event::Kind::StateRead) {
            context.device = *event.state;
            const ImageSlot* active = active_of(context, plan.image);
            const bool ours =
                active != nullptr && active->hash.has_value() && *active->hash == context.target;
            if (ours && active->confirmed) {
                return Step{UpdateState::Completed, Effect::Finish};
            }
            context.swap_scheduled = true;
            return fail(context, ErrorCode::UpdateFailed,
                        "dfu: device did not report the image as confirmed");
        }
        if (event.kind == Event::Kind::Failed) {
            context.swap_scheduled = true;
            return fail(context, event.error);
        }
        break;

    case UpdateState::Completed:
    case UpdateState::Failed:
    case UpdateState::Cancelled:
        // Terminal: nothing moves them.
        return Step{state, Effect::None};
    }

    // An event this state has no rule for. Ignoring it would hide a driver bug;
    // failing makes it visible at the point it happens.
    return fail(context, ErrorCode::Internal, "dfu: event not legal in this state");
}

} // namespace smply::dfu
