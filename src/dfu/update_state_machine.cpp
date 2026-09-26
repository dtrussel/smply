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
    context.report.revert_pending = context.swap_scheduled;
    context.report.cause = std::move(error);
    return Step{UpdateState::Failed, Effect::Finish};
}

[[nodiscard]] Step fail(Context& context, ErrorCode code, const char* where)
{
    return fail(context, Error{code, where});
}

/// The slot of \p image that is running.
[[nodiscard]] const ImageSlot* active_of(const Context& context, std::uint32_t image)
{
    if (!context.device.has_value()) {
        return nullptr;
    }
    return context.device->active_slot(image);
}

/// Any slot of \p image marked to boot next.
///
/// Scoped to one image: on a multi-image device another image's pending swap
/// says nothing about this one (ADR-0021).
[[nodiscard]] bool anything_pending(const Context& context, std::uint32_t image)
{
    if (!context.device.has_value()) {
        return false;
    }
    const std::vector<ImageSlot>& slots = context.device->slots;
    return std::ranges::any_of(
        slots, [image](const ImageSlot& slot) { return slot.image == image && slot.pending; });
}

/// Whether \p slot carries \p target's hash.
[[nodiscard]] bool holds(const ImageSlot* slot, const Target& target)
{
    return slot != nullptr && !target.hash.empty() && slot->hash.has_value() &&
           *slot->hash == target.hash;
}

/// The slot of \p target's image holding its file, or nullptr.
///
/// The device finds a hash in any image (protocol-notes section 6), but only a
/// slot of the target's own image counts as holding it.
[[nodiscard]] const ImageSlot* slot_holding(const Context& context, const Target& target)
{
    if (!context.device.has_value()) {
        return nullptr;
    }
    const std::vector<ImageSlot>& slots = context.device->slots;
    const auto found = std::ranges::find_if(slots, [&target](const ImageSlot& slot) {
        return slot.image == target.image && holds(&slot, target);
    });
    return found == slots.end() ? nullptr : &*found;
}

/// Whether the update should stop once the device merely holds the image.
[[nodiscard]] bool upload_only(const UpdatePlan& plan)
{
    return plan.mode == UpdateMode::UploadOnly;
}

/// Whether \p target is a `Client` image running its file, unconfirmed: a
/// trial boot, which only a reset can have started.
[[nodiscard]] bool running_on_trial(const Context& context, const Target& target)
{
    const ImageSlot* active = active_of(context, target.image);
    return target.commit == CommitBy::Client && holds(active, target) && !active->confirmed;
}

/// The first `Client` target still owed a confirm, or `targets.size()`.
[[nodiscard]] std::size_t next_in_trial(const Context& context, std::size_t from)
{
    for (std::size_t index = from; index < context.targets.size(); ++index) {
        if (context.targets[index].in_trial) {
            return index;
        }
    }
    return context.targets.size();
}

/// The fork ADR-0014 introduced: ask, or confirm without asking. Either way
/// the first image owed a confirm becomes the current one.
[[nodiscard]] Step confirmation_fork(const UpdatePlan& plan, Context& context)
{
    context.current = next_in_trial(context, 0);
    if (plan.mode == UpdateMode::ConfirmImmediately) {
        return Step{UpdateState::Confirming, Effect::Confirm};
    }
    return Step{UpdateState::AwaitingConfirmation, Effect::RequestConfirmation};
}

/// What the device reports about a `Device` image after the reset
/// (docs/multi-image.md, "The contract a device-committed image must honour").
enum class Apply : std::uint8_t
{
    Applying,
    /// The other MCU runs it, not yet committed.
    OnTrial,
    Committed,
    Failed,
};

/// Slot 0 reports what the other MCU runs (ADR-0022), so the target there
/// means applied -- on trial until confirmed. The target pending in a slot
/// means still applying. Anything else is a failed apply.
[[nodiscard]] Apply apply_of(const Context& context, const Target& target)
{
    const ImageSlot* active = active_of(context, target.image);
    if (holds(active, target)) {
        return active->confirmed ? Apply::Committed : Apply::OnTrial;
    }
    const ImageSlot* holder = slot_holding(context, target);
    return holder != nullptr && holder->pending ? Apply::Applying : Apply::Failed;
}

/// Once every `Client` image is confirmed, or none is on trial: wait until the
/// device has committed every `Device` image (ADR-0022).
///
/// Image 0 is confirmed by now, so nothing reverts: a failure here is
/// reported with `revert_pending` false, and the device's boot-time logic
/// owns what follows.
[[nodiscard]] Step await_commit(Context& context)
{
    bool committing = false;
    for (std::size_t index = 0; index < context.targets.size(); ++index) {
        const Target& target = context.targets[index];
        if (target.commit != CommitBy::Device) {
            continue;
        }
        switch (apply_of(context, target)) {
        case Apply::Committed:
            context.report.images[index].applied = true;
            context.report.images[index].committed = true;
            break;
        case Apply::OnTrial:
            context.report.images[index].applied = true;
            committing = true;
            break;
        case Apply::Applying:
        case Apply::Failed:
            // It ran on trial and no longer does: the other MCU reverted it.
            context.report.images[index].applied = false;
            return fail(context, ErrorCode::UpdateFailed, "dfu: device did not commit an image");
        }
    }
    if (committing) {
        return Step{UpdateState::AwaitingDeviceCommit, Effect::AwaitApply};
    }
    return Step{UpdateState::Completed, Effect::Finish};
}

/// After the reset, once every `Client` image has been checked: wait for the
/// `Device` images to run on trial, then confirm.
///
/// `Client` images are confirmed only after every `Device` image is running
/// on trial, and the device commits those only after that confirm (ADR-0021,
/// ADR-0022). A failed apply therefore leaves the `Client` images unconfirmed,
/// and `fail()` reports the revert that follows.
[[nodiscard]] Step await_device(const UpdatePlan& plan, Context& context)
{
    bool applying = false;
    for (std::size_t index = 0; index < context.targets.size(); ++index) {
        const Target& target = context.targets[index];
        if (target.commit != CommitBy::Device) {
            continue;
        }
        switch (apply_of(context, target)) {
        case Apply::Committed:
            context.report.images[index].committed = true;
            context.report.images[index].applied = true;
            break;
        case Apply::OnTrial:
            context.report.images[index].applied = true;
            break;
        case Apply::Applying:
            applying = true;
            break;
        case Apply::Failed:
            return fail(context, ErrorCode::UpdateFailed, "dfu: device did not apply an image");
        }
    }
    if (applying) {
        return Step{UpdateState::AwaitingDeviceApply, Effect::AwaitApply};
    }
    if (!context.swap_scheduled) {
        // No `Client` image is on trial, so no confirm will come: the device
        // commits on its own (ADR-0022). A `ConfirmImmediately` update whose
        // confirm response was lost lands here too.
        return await_commit(context);
    }
    return confirmation_fork(plan, context);
}

/// A failed read while waiting on the device. The link may be down because the
/// device is updating the MCU it runs through -- on a coordinating MCU the BLE
/// controller is that MCU -- so a drop asks for a reconnect and a lost answer
/// is simply asked again (ADR-0022). The deadline keeps running either way.
[[nodiscard]] Step wait_read_failed(UpdateState state, const Error& error, Context& context)
{
    if (error.code() == ErrorCode::Disconnected) {
        return Step{UpdateState::AwaitingReconnect, Effect::RequestReconnect};
    }
    if (error.code() == ErrorCode::Timeout) {
        return Step{state, Effect::AwaitApply};
    }
    return fail(context, error);
}

/// `VerifyingBooted`: did the device come up on every new `Client` image, or
/// revert one?
[[nodiscard]] Step inspect_boot(const UpdatePlan& plan, Context& context)
{
    for (const Target& target : context.targets) {
        if (target.commit == CommitBy::Client && active_of(context, target.image) == nullptr) {
            return fail(context, ErrorCode::UpdateFailed, "dfu: no active slot after reboot");
        }
    }

    bool reverted = false;
    bool not_booted = false;
    for (std::size_t index = 0; index < context.targets.size(); ++index) {
        Target& target = context.targets[index];
        target.in_trial = false;
        if (target.commit != CommitBy::Client) {
            continue;
        }
        const ImageSlot* active = active_of(context, target.image);
        if (!holds(active, target)) {
            // The signature of a revert: the device is running something else
            // and nothing is queued to change that. If something *is* pending
            // the swap simply has not happened, which is a different failure.
            if (anything_pending(context, target.image)) {
                not_booted = true;
            } else {
                context.report.images[index].rolled_back = true;
                reverted = true;
            }
            continue;
        }
        target.in_trial = !active->confirmed;
    }

    // Whatever is still on trial, or still queued, happens at the next reset.
    context.swap_scheduled = not_booted || next_in_trial(context, 0) < context.targets.size();
    if (reverted) {
        return fail(context, ErrorCode::UpdateFailed, "dfu: device reverted to the old image");
    }
    if (not_booted) {
        return fail(context, ErrorCode::UpdateFailed, "dfu: device did not boot the new image");
    }
    return await_device(plan, context);
}

/// Every image is staged, or needed nothing: reset, or judge the device as it
/// stands.
[[nodiscard]] Step staged(const UpdatePlan& plan, Context& context)
{
    if (upload_only(plan)) {
        return Step{UpdateState::Completed, Effect::Finish};
    }
    if (context.swap_scheduled) {
        return Step{UpdateState::Resetting, Effect::Reset};
    }
    // Nothing is queued, so a reset would change nothing: what the device runs
    // now is what it would run after one.
    return inspect_boot(plan, context);
}

/// Decides what, if anything, needs doing for each image in turn -- from the
/// slot table alone.
///
/// Four cases per image, in the order they are checked. The first two exist
/// because an update may be resumed by a *new* process against a device that
/// is already part-way through one, which is exactly what happens when an
/// application is restarted mid-update.
[[nodiscard]] Step plan_from_state(const UpdatePlan& plan, Context& context)
{
    // A `Client` image running its file on trial means the reset has already
    // happened: this is the confirmation window, or the wait for the device,
    // whoever started it. Staging anything now would need another reset, and
    // that would revert the trial.
    const bool after_reset =
        context.current == 0 && std::ranges::any_of(context.targets, [&context](const Target& t) {
            return running_on_trial(context, t);
        });
    if (after_reset && !upload_only(plan)) {
        for (ImageReport& image : context.report.images) {
            image.upload_skipped = true;
        }
        return inspect_boot(plan, context);
    }

    for (; context.current < context.targets.size(); ++context.current) {
        const Target& target = context.targets[context.current];
        ImageReport& report = context.report.images[context.current];
        const ImageSlot* holder = slot_holding(context, target);

        // 1. The device is already running it.
        if (holder != nullptr && holder == active_of(context, target.image)) {
            report.upload_skipped = true;
            continue;
        }

        // 2. Another slot holds it, already marked for the next boot. The mark
        //    succeeded at some point even if we never saw the response.
        if (holder != nullptr && holder->pending) {
            report.upload_skipped = true;
            context.swap_scheduled = context.swap_scheduled || !upload_only(plan);
            continue;
        }

        // 3. Another slot holds it, unmarked.
        if (holder != nullptr && plan.skip_if_already_present) {
            report.upload_skipped = true;
            if (upload_only(plan)) {
                continue;
            }
            return Step{UpdateState::MarkingForTest, Effect::MarkForTest};
        }

        // 4. Upload it. Even here the device may answer "already present" on
        //    the first packet and finish without a transfer (rule 9a).
        context.upload_in_progress = true;
        return Step{UpdateState::Uploading, Effect::StartUpload};
    }
    return staged(plan, context);
}

/// Moves on to the next image once the current one is staged.
[[nodiscard]] Step next_target(const UpdatePlan& plan, Context& context)
{
    ++context.current;
    context.mark_retried = false;
    if (context.current < context.targets.size()) {
        return Step{UpdateState::Planning, Effect::Continue};
    }
    return staged(plan, context);
}

/// Derives the report's summary fields from its images.
void summarise(Context& context)
{
    std::vector<ImageReport>& images = context.report.images;
    if (images.empty()) {
        return;
    }
    context.report.bytes_transferred = 0;
    for (const ImageReport& image : images) {
        context.report.bytes_transferred += image.bytes_transferred;
    }
    context.report.upload_skipped =
        std::ranges::all_of(images, [](const ImageReport& image) { return image.upload_skipped; });
    context.report.rolled_back =
        std::ranges::any_of(images, [](const ImageReport& image) { return image.rolled_back; });
}

/// `Uploading`, on `UploadFinished`.
[[nodiscard]] Step upload_finished(const Event& event, const UpdatePlan& plan, Context& context)
{
    context.upload_in_progress = false;
    ImageReport& report = context.report.images[context.current];
    report.bytes_transferred = event.transferred;
    // The pre-flight check is not the only way a transfer gets skipped: the
    // server runs the same check on the first packet and can answer "complete"
    // before any image data is really sent (rule 9a), and
    // UploadResult::already_present says so.
    report.upload_skipped = report.upload_skipped || event.already_present;
    if (upload_only(plan)) {
        return next_target(plan, context);
    }
    return Step{UpdateState::VerifyingUpload, Effect::ReadState};
}

/// `Uploading`, on a failure.
[[nodiscard]] Step upload_failed(const Error& error, Context& context)
{
    // A dropped link suspends the transfer rather than ending it: the device
    // keeps the session and resumes by `sha` (section 6, rule 6).
    if (error.code() == ErrorCode::Disconnected) {
        return Step{UpdateState::AwaitingReconnect, Effect::RequestReconnect};
    }
    return fail(context, error);
}

/// `MarkingForTest`, on a refusal: recover once from a lost response, or fail.
///
/// `ImageAlreadyPending` means a swap is scheduled -- possibly the one this
/// request was asking for, whose response was lost. Read the state back and let
/// the planner decide; case 2 there sees our own image already marked and moves
/// on.
///
/// A group-less `BadState` gets the same treatment, and that is the whole of
/// the A24 fix. A server built with `CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL`
/// translates an image-group code onto `mcumgr_err_t` for a v1 client and drops
/// the group, so `image_error()` is always `nullopt` there and a branch on it
/// alone cannot fire at all (docs/protocol-notes.md section 9, A16 and A24).
/// Exactly three image codes translate to `EBADSTATE` -- `NoFreeSlot`,
/// `CurrentVersionIsNewer` and `ImageAlreadyPending` -- and all three want the
/// same answer: re-read the state and let the planner decide, which fails
/// cleanly when the pending swap turns out not to be ours.
///
/// Deliberately not widened to `Unknown`: that is the catch-all the same table
/// gives eighteen other codes, every flash failure among them, so recovering
/// from it would retry genuine refusals. `mark_retried` still bounds this to
/// one extra round trip.
[[nodiscard]] Step mark_refused(const Error& error, Context& context)
{
    const std::optional<ImageError> code = image_error(error);
    const bool recoverable =
        code == ImageError::ImageAlreadyPending || smp_error(error) == SmpError::BadState;
    if (recoverable && !context.mark_retried) {
        context.mark_retried = true;
        return Step{UpdateState::InspectingImages, Effect::ReadState};
    }
    return fail(context, error);
}

/// `Resetting`, on a refusal or a lost answer.
[[nodiscard]] Step reset_refused(const Error& error, Context& context)
{
    // A reset hook may refuse with `Busy`, which invites one retry with `force`
    // (docs/protocol-notes.md section 5).
    if (smp_error(error) == SmpError::Busy && !context.reset_forced) {
        context.reset_forced = true;
        return Step{UpdateState::Resetting, Effect::ForceReset};
    }
    // Losing the response is normal: the device may reset before the answer
    // goes out (A3). A drop, or silence, is treated as the reset having happened
    // -- the verify step after the reboot is the real check, and failing here
    // would abandon a device that is already swapping.
    if (error.code() == ErrorCode::Disconnected || error.code() == ErrorCode::Timeout) {
        return Step{UpdateState::AwaitingDisconnect, Effect::AwaitDisconnect};
    }
    return fail(context, error);
}

} // namespace

Context make_context(std::vector<Target> targets)
{
    Context context;
    context.report.images.reserve(targets.size());
    for (const Target& target : targets) {
        context.report.images.push_back(ImageReport{
            .image = target.image, .commit = target.commit, .target_hash = target.hash});
    }
    context.targets = std::move(targets);
    return context;
}

namespace {

/// Whether \p state works on `context.current`, which must then name a target.
[[nodiscard]] bool needs_current(UpdateState state)
{
    return state == UpdateState::Uploading || state == UpdateState::VerifyingUpload ||
           state == UpdateState::MarkingForTest || state == UpdateState::Confirming;
}

[[nodiscard]] Step decide(UpdateState state, const Event& event, const UpdatePlan& plan,
                          Context& context)
{
    // Cancellation is legal everywhere and looks the same everywhere, so it is
    // handled once rather than as a row of every state below.
    if (event.kind == Event::Kind::Cancel) {
        if (is_terminal(state)) {
            return Step{state, Effect::None};
        }
        context.report.revert_pending = context.swap_scheduled;
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
            return upload_finished(event, plan, context);
        }
        if (event.kind == Event::Kind::Failed) {
            return upload_failed(event.error, context);
        }
        break;

    case UpdateState::VerifyingUpload:
        if (event.kind == Event::Kind::StateRead) {
            context.device = *event.state;
            if (slot_holding(context, context.targets[context.current]) == nullptr) {
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
            return next_target(plan, context);
        }
        if (event.kind == Event::Kind::Failed) {
            return mark_refused(event.error, context);
        }
        break;

    case UpdateState::Resetting:
        if (event.kind == Event::Kind::ResetAccepted) {
            return Step{UpdateState::AwaitingDisconnect, Effect::AwaitDisconnect};
        }
        if (event.kind == Event::Kind::Failed) {
            return reset_refused(event.error, context);
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

    case UpdateState::AwaitingDeviceApply:
        if (event.kind == Event::Kind::ApplyPollDue) {
            return Step{UpdateState::AwaitingDeviceApply, Effect::ReadState};
        }
        if (event.kind == Event::Kind::StateRead) {
            context.device = *event.state;
            return await_device(plan, context);
        }
        if (event.kind == Event::Kind::ApplyTimedOut) {
            return fail(context, ErrorCode::Timeout, "dfu: device did not apply an image in time");
        }
        if (event.kind == Event::Kind::Failed) {
            return wait_read_failed(state, event.error, context);
        }
        break;

    case UpdateState::AwaitingDeviceCommit:
        if (event.kind == Event::Kind::ApplyPollDue) {
            return Step{UpdateState::AwaitingDeviceCommit, Effect::ReadState};
        }
        if (event.kind == Event::Kind::StateRead) {
            context.device = *event.state;
            return await_commit(context);
        }
        if (event.kind == Event::Kind::ApplyTimedOut) {
            return fail(context, ErrorCode::Timeout, "dfu: device did not commit an image in time");
        }
        if (event.kind == Event::Kind::Failed) {
            return wait_read_failed(state, event.error, context);
        }
        break;

    case UpdateState::AwaitingConfirmation:
        if (event.kind == Event::Kind::ConfirmApproved) {
            return Step{UpdateState::Confirming, Effect::Confirm};
        }
        break;

    case UpdateState::Confirming:
        if (event.kind == Event::Kind::Confirmed) {
            // One image at a time, each by its own hash: a hashless confirm
            // reaches only the running image (protocol-notes section 6).
            context.targets[context.current].in_trial = false;
            context.current = next_in_trial(context, context.current);
            if (context.current < context.targets.size()) {
                return Step{UpdateState::Confirming, Effect::Confirm};
            }
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
            const bool all_confirmed =
                std::ranges::all_of(context.targets, [&context](const Target& target) {
                    const ImageSlot* active = active_of(context, target.image);
                    return target.commit != CommitBy::Client ||
                           (holds(active, target) && active->confirmed);
                });
            if (all_confirmed) {
                // The device commits its own images now (ADR-0022).
                return await_commit(context);
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

} // namespace

Step advance(UpdateState state, const Event& event, const UpdatePlan& plan, Context& context)
{
    // Both are the updater's to guarantee; a slip is a bug, reported as one
    // rather than read out of bounds.
    const bool consistent = !context.targets.empty() &&
                            context.report.images.size() == context.targets.size() &&
                            (!needs_current(state) || context.current < context.targets.size());
    const Step step = consistent || is_terminal(state)
                          ? decide(state, event, plan, context)
                          : fail(context, ErrorCode::Internal, "dfu: no image to work on");
    summarise(context);
    return step;
}

} // namespace smply::dfu
