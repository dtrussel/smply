// SPDX-License-Identifier: Apache-2.0
//
// The update decision table from docs/design.md section 8, row by row. No
// client, no transport, no clock -- which is the whole point of splitting the
// machine out of `FirmwareUpdater`: every recovery rule is a value-in,
// value-out assertion instead of a scenario needing a device.
//
// Three rules the suite exists to protect, each read out of the server's
// source (docs/protocol-notes.md section 7):
//
//   * a rollback is recognised from the FLAGS, not from a hash alone;
//   * a refused mark-for-test is recoverable exactly once, in EITHER shape --
//     `ImageAlreadyPending` over v2 and a group-less `BadState` over v1;
//   * a refused confirm is fatal *and* leaves the device about to revert.

#include "dfu/update_state_machine.hpp"

#include "smply/dfu/firmware_updater.hpp"
#include "smply/error.hpp"
#include "smply/groups/image.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using smply::ConstBytes;
using smply::Error;
using smply::ErrorCode;
using smply::Group;
using smply::ImageError;
using smply::ImageHash;
using smply::ImageSlot;
using smply::ImageState;
using smply::MgmtError;
using smply::SmpError;
using smply::UpdateMode;
using smply::UpdatePlan;
using smply::UpdateState;
using smply::dfu::advance;
using smply::dfu::Context;
using smply::dfu::Effect;
using smply::dfu::Event;
using smply::dfu::Step;
using smply::dfu::Target;

namespace Catch {
template<>
struct StringMaker<smply::UpdateState>
{
    static std::string convert(smply::UpdateState state)
    {
        return std::string{smply::to_string(state)};
    }
};

template<>
struct StringMaker<smply::ErrorCode>
{
    static std::string convert(smply::ErrorCode code)
    {
        return std::string{smply::to_string(code)};
    }
};
} // namespace Catch

namespace {

/// A distinguishable 32-byte hash.
///
/// No `REQUIRE` here: these are built during static initialisation, where
/// Catch2 has no result capture and an assertion aborts the process before a
/// single test runs. A 32-byte value is always accepted, so there is nothing to
/// assert anyway.
[[nodiscard]] ImageHash hash_of(std::uint8_t seed)
{
    std::vector<std::byte> bytes(32);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>((i + seed) & 0xFFU);
    }
    return ImageHash::from(ConstBytes{bytes}).value_or(ImageHash{});
}

const ImageHash kTarget = hash_of(1);
const ImageHash kOther = hash_of(200);

/// One slot, spelled so a test states only what it is about.
struct SlotSpec
{
    std::uint32_t image = 0;
    std::uint32_t slot = 0;
    ImageHash hash;
    bool active = false;
    bool pending = false;
    bool confirmed = false;
};

[[nodiscard]] ImageState state_of(std::initializer_list<SlotSpec> specs)
{
    ImageState out;
    for (const SlotSpec& spec : specs) {
        ImageSlot slot;
        slot.image = spec.image;
        slot.slot = spec.slot;
        slot.version = "1.0.0";
        slot.hash = spec.hash;
        slot.bootable = true;
        slot.active = spec.active;
        slot.pending = spec.pending;
        slot.confirmed = spec.confirmed;
        out.slots.push_back(slot);
    }
    return out;
}

/// The ordinary steady state: the old image running and confirmed, the new one
/// sitting in the secondary slot.
[[nodiscard]] ImageState running_old_holding_new()
{
    return state_of({SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true},
                     SlotSpec{.slot = 1, .hash = kTarget}});
}

/// A trial boot in progress: the new image running unconfirmed, the old one
/// marked confirmed. Reads backwards, and is meant to.
[[nodiscard]] ImageState trial_boot()
{
    return state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true},
                     SlotSpec{.slot = 1, .hash = kOther, .confirmed = true}});
}

[[nodiscard]] Event state_read(const ImageState& state)
{
    Event event;
    event.kind = Event::Kind::StateRead;
    event.state = &state;
    return event;
}

[[nodiscard]] Event just(Event::Kind kind)
{
    Event event;
    event.kind = kind;
    return event;
}

[[nodiscard]] Event failed(Error error)
{
    Event event;
    event.kind = Event::Kind::Failed;
    event.error = std::move(error);
    return event;
}

[[nodiscard]] Event failed(ErrorCode code)
{
    return failed(Error{code});
}

/// A device-reported image-group failure, in the group-scoped shape.
[[nodiscard]] Event image_failure(ImageError code)
{
    return failed(Error{ErrorCode::ProtocolError,
                        MgmtError::scoped(Group::Image, static_cast<std::uint16_t>(code))});
}

/// The same refusal as a **v1** server reports it: a flat `rc` with no group.
///
/// This helper is the reason A24 survived review. Until it existed the suite
/// could only express the group-scoped shape, so every recovery test asked the
/// machine a question no SMP v1 device ever asks it (docs/protocol-notes.md
/// section 9, A16).
[[nodiscard]] Event flat_failure(SmpError code)
{
    return failed(
        Error{ErrorCode::ProtocolError, MgmtError::smp(static_cast<std::uint16_t>(code))});
}

/// A context for one `Client` image, \p image, whose file is `kTarget`.
[[nodiscard]] Context fresh(std::uint32_t image = 0)
{
    return smply::dfu::make_context({Target{.image = image, .hash = kTarget}});
}

/// Every state an update can be in that is not terminal.
constexpr std::array<UpdateState, 15> kNonTerminal{
    UpdateState::Idle,
    UpdateState::QueryingParameters,
    UpdateState::InspectingImages,
    UpdateState::Planning,
    UpdateState::Uploading,
    UpdateState::VerifyingUpload,
    UpdateState::MarkingForTest,
    UpdateState::Resetting,
    UpdateState::AwaitingDisconnect,
    UpdateState::AwaitingReconnect,
    UpdateState::VerifyingBooted,
    UpdateState::AwaitingDeviceApply,
    UpdateState::AwaitingConfirmation,
    UpdateState::Confirming,
    UpdateState::VerifyingConfirmed,
};

} // namespace

// --- The happy path, state by state -----------------------------------------

TEST_CASE("an update starts by asking for the device's buffer budget", "[dfu][machine]")
{
    Context context = fresh();
    const Step step = advance(UpdateState::Idle, just(Event::Kind::Start), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::QueryingParameters);
    CHECK(step.effect == Effect::QueryParameters);
}

TEST_CASE("the buffer budget is remembered, and its absence is not fatal", "[dfu][machine]")
{
    // The command is optional; a device without it is ordinary, not broken
    // (docs/protocol-notes.md section 9, A8).
    Context context = fresh();
    Event read;
    read.kind = Event::Kind::ParametersRead;
    read.buf_size = 512;
    const Step got = advance(UpdateState::QueryingParameters, read, UpdatePlan{}, context);
    CHECK(got.next == UpdateState::InspectingImages);
    CHECK(got.effect == Effect::ReadState);
    CHECK(context.buf_size == 512);

    Context without = fresh();
    const Step missing = advance(UpdateState::QueryingParameters,
                                 just(Event::Kind::ParametersUnavailable), UpdatePlan{}, without);
    CHECK(missing.next == UpdateState::InspectingImages);
    CHECK(missing.effect == Effect::ReadState);
    CHECK(without.buf_size == 0);
}

TEST_CASE("reading the slot table leads to a planning step", "[dfu][machine]")
{
    Context context = fresh();
    const ImageState state = running_old_holding_new();
    const Step step =
        advance(UpdateState::InspectingImages, state_read(state), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Planning);
    CHECK(step.effect == Effect::Continue);
    REQUIRE(context.device.has_value());
    CHECK(context.device->slots.size() == 2);
}

TEST_CASE("a failure while inspecting is fatal and changes nothing", "[dfu][machine]")
{
    Context context = fresh();
    const Step step =
        advance(UpdateState::InspectingImages, failed(ErrorCode::Timeout), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Failed);
    CHECK(step.effect == Effect::Finish);
    REQUIRE(context.report.cause.has_value());
    CHECK(context.report.cause->code() == ErrorCode::Timeout);
    CHECK_FALSE(context.report.revert_pending);
}

// --- Planning ---------------------------------------------------------------

TEST_CASE("an image the device is already running and has confirmed is done",
          "[dfu][machine][planning]")
{
    Context context = fresh();
    const ImageState state =
        state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true, .confirmed = true}});
    context.device = state;

    const Step step =
        advance(UpdateState::Planning, just(Event::Kind::Continue), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Completed);
    CHECK(step.effect == Effect::Finish);
    CHECK(context.report.upload_skipped);
}

TEST_CASE("an image already running unconfirmed lands in the confirmation window",
          "[dfu][machine][planning]")
{
    // A trial boot somebody else started -- an application restarted mid-update
    // arrives here, and must not re-upload or re-reset.
    Context context = fresh();
    context.device = trial_boot();

    const Step asked =
        advance(UpdateState::Planning, just(Event::Kind::Continue), UpdatePlan{}, context);
    CHECK(asked.next == UpdateState::AwaitingConfirmation);
    CHECK(asked.effect == Effect::RequestConfirmation);
    CHECK(context.swap_scheduled);

    Context automatic = fresh();
    automatic.device = trial_boot();
    UpdatePlan plan;
    plan.mode = UpdateMode::ConfirmImmediately;
    const Step confirmed =
        advance(UpdateState::Planning, just(Event::Kind::Continue), plan, automatic);
    CHECK(confirmed.next == UpdateState::Confirming);
    CHECK(confirmed.effect == Effect::Confirm);

    Context upload_only = fresh();
    upload_only.device = trial_boot();
    UpdatePlan stop_early;
    stop_early.mode = UpdateMode::UploadOnly;
    const Step stopped =
        advance(UpdateState::Planning, just(Event::Kind::Continue), stop_early, upload_only);
    CHECK(stopped.next == UpdateState::Completed);
}

TEST_CASE("an image already marked for the next boot skips to the reset",
          "[dfu][machine][planning]")
{
    // The mark succeeded even if its response was lost. Re-sending it would be
    // refused with ImageAlreadyPending, so the plan steps over it.
    Context context = fresh();
    context.device =
        state_of({SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true},
                  SlotSpec{.slot = 1, .hash = kTarget, .pending = true}});

    const Step step =
        advance(UpdateState::Planning, just(Event::Kind::Continue), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Resetting);
    CHECK(step.effect == Effect::Reset);
    CHECK(context.swap_scheduled);
    CHECK(context.report.upload_skipped);
}

TEST_CASE("an image present but unmarked is marked without uploading", "[dfu][machine][planning]")
{
    Context context = fresh();
    context.device = running_old_holding_new();

    const Step step =
        advance(UpdateState::Planning, just(Event::Kind::Continue), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::MarkingForTest);
    CHECK(step.effect == Effect::MarkForTest);
    CHECK(context.report.upload_skipped);
    CHECK_FALSE(context.upload_in_progress);
}

TEST_CASE("the pre-flight skip can be switched off", "[dfu][machine][planning]")
{
    // Turning it off costs a round trip rather than the transfer: the server
    // runs the same check on the first packet (section 6, rule 9a).
    Context context = fresh();
    context.device = running_old_holding_new();
    UpdatePlan plan;
    plan.skip_if_already_present = false;

    const Step step = advance(UpdateState::Planning, just(Event::Kind::Continue), plan, context);
    CHECK(step.next == UpdateState::Uploading);
    CHECK(step.effect == Effect::StartUpload);
    CHECK(context.upload_in_progress);
}

TEST_CASE("an image the device does not hold is uploaded", "[dfu][machine][planning]")
{
    Context context = fresh();
    context.device =
        state_of({SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true}});

    const Step step =
        advance(UpdateState::Planning, just(Event::Kind::Continue), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Uploading);
    CHECK(step.effect == Effect::StartUpload);
    CHECK_FALSE(context.report.upload_skipped);
}

// --- Uploading --------------------------------------------------------------

TEST_CASE("a finished upload is verified, unless the caller only wanted the upload",
          "[dfu][machine]")
{
    Context context = fresh();
    context.upload_in_progress = true;
    Event done;
    done.kind = Event::Kind::UploadFinished;
    done.transferred = 4096;

    const Step step = advance(UpdateState::Uploading, done, UpdatePlan{}, context);
    CHECK(step.next == UpdateState::VerifyingUpload);
    CHECK(step.effect == Effect::ReadState);
    CHECK(context.report.bytes_transferred == 4096);
    CHECK_FALSE(context.upload_in_progress);

    Context stop_early = fresh();
    stop_early.upload_in_progress = true;
    UpdatePlan plan;
    plan.mode = UpdateMode::UploadOnly;
    const Step stopped = advance(UpdateState::Uploading, done, plan, stop_early);
    CHECK(stopped.next == UpdateState::Completed);
    CHECK(stopped.effect == Effect::Finish);
}

TEST_CASE("a dropped link suspends the upload rather than ending it", "[dfu][machine]")
{
    // The device keeps its session and resumes by `sha` (section 6, rule 6).
    Context context = fresh();
    context.upload_in_progress = true;

    const Step step =
        advance(UpdateState::Uploading, failed(ErrorCode::Disconnected), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::AwaitingReconnect);
    CHECK(step.effect == Effect::RequestReconnect);
    CHECK(context.upload_in_progress);
    CHECK_FALSE(context.report.cause.has_value());
}

TEST_CASE("any other upload failure is fatal", "[dfu][machine]")
{
    Context context = fresh();
    const Step step =
        advance(UpdateState::Uploading, failed(ErrorCode::ImageMismatch), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Failed);
    REQUIRE(context.report.cause.has_value());
    CHECK(context.report.cause->code() == ErrorCode::ImageMismatch);
}

// --- Verifying the upload ---------------------------------------------------

TEST_CASE("an uploaded image must appear in the slot table", "[dfu][machine]")
{
    Context present = fresh();
    const ImageState holding = running_old_holding_new();
    const Step step =
        advance(UpdateState::VerifyingUpload, state_read(holding), UpdatePlan{}, present);
    CHECK(step.next == UpdateState::MarkingForTest);
    CHECK(step.effect == Effect::MarkForTest);

    Context absent = fresh();
    const ImageState empty =
        state_of({SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true}});
    const Step missing =
        advance(UpdateState::VerifyingUpload, state_read(empty), UpdatePlan{}, absent);
    CHECK(missing.next == UpdateState::Failed);
    REQUIRE(absent.report.cause.has_value());
    CHECK(absent.report.cause->code() == ErrorCode::ImageMismatch);
}

TEST_CASE("a failed verification read is fatal", "[dfu][machine]")
{
    Context context = fresh();
    const Step step =
        advance(UpdateState::VerifyingUpload, failed(ErrorCode::Timeout), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Failed);
}

// --- Marking for test -------------------------------------------------------

TEST_CASE("marking for test schedules a swap and resets", "[dfu][machine]")
{
    Context context = fresh();
    const Step step = advance(UpdateState::MarkingForTest, just(Event::Kind::MarkedForTest),
                              UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Resetting);
    CHECK(step.effect == Effect::Reset);
    CHECK(context.swap_scheduled);
}

TEST_CASE("ImageAlreadyPending is recoverable exactly once", "[dfu][machine]")
{
    // Re-reading the state and finding our own image marked means the previous
    // attempt worked and its response was lost. A second one is a real refusal.
    Context context = fresh();
    const Step retried =
        advance(UpdateState::MarkingForTest, image_failure(ImageError::ImageAlreadyPending),
                UpdatePlan{}, context);
    CHECK(retried.next == UpdateState::InspectingImages);
    CHECK(retried.effect == Effect::ReadState);
    CHECK(context.mark_retried);
    CHECK_FALSE(context.report.cause.has_value());

    const Step again =
        advance(UpdateState::MarkingForTest, image_failure(ImageError::ImageAlreadyPending),
                UpdatePlan{}, context);
    CHECK(again.next == UpdateState::Failed);
    REQUIRE(context.report.cause.has_value());
}

TEST_CASE("a group-less BadState recovers the mark exactly once", "[dfu][machine]")
{
    // The same rule over SMP v1. A server with
    // CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL translates the image code
    // onto `mcumgr_err_t` and drops the group, so the recovery above never
    // sees `ImageAlreadyPending`. Branching on that code alone, it could not
    // fire at all.
    Context context = fresh();
    const Step retried = advance(UpdateState::MarkingForTest, flat_failure(SmpError::BadState),
                                 UpdatePlan{}, context);
    CHECK(retried.next == UpdateState::InspectingImages);
    CHECK(retried.effect == Effect::ReadState);
    CHECK(context.mark_retried);
    CHECK_FALSE(context.report.cause.has_value());

    const Step again = advance(UpdateState::MarkingForTest, flat_failure(SmpError::BadState),
                               UpdatePlan{}, context);
    CHECK(again.next == UpdateState::Failed);
    REQUIRE(context.report.cause.has_value());
}

TEST_CASE("the budget is one recovery, not one of each shape", "[dfu][machine]")
{
    // Spending it on either shape spends it for both: a device that answers
    // v2 once and v1 once is still one lost response, not two. The v1 shape
    // goes first deliberately -- that ordering is the one that fails if the
    // flat arm is ever removed again, and an invariant test that passes either
    // way protects nothing.
    Context context = fresh();
    const Step first = advance(UpdateState::MarkingForTest, flat_failure(SmpError::BadState),
                               UpdatePlan{}, context);
    CHECK(first.next == UpdateState::InspectingImages);

    const Step second =
        advance(UpdateState::MarkingForTest, image_failure(ImageError::ImageAlreadyPending),
                UpdatePlan{}, context);
    CHECK(second.next == UpdateState::Failed);
}

TEST_CASE("a group-less code that is not BadState is still fatal", "[dfu][machine]")
{
    // `Unknown` is what A24 actually measured on the bench, and it is where
    // the widening deliberately stops: the same translation table gives it to
    // eighteen other image codes, every flash failure among them, so treating
    // it as recoverable would retry genuine refusals.
    Context context = fresh();
    const Step step = advance(UpdateState::MarkingForTest, flat_failure(SmpError::Unknown),
                              UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Failed);
    CHECK_FALSE(context.mark_retried);
    REQUIRE(context.report.cause.has_value());
    CHECK(smply::smp_error(*context.report.cause) == SmpError::Unknown);
}

TEST_CASE("marking the running slot for test is fatal with the device's own code", "[dfu][machine]")
{
    Context context = fresh();
    const Step step =
        advance(UpdateState::MarkingForTest,
                image_failure(ImageError::ImageSettingTestToActiveDenied), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Failed);
    REQUIRE(context.report.cause.has_value());
    CHECK(smply::image_error(*context.report.cause) == ImageError::ImageSettingTestToActiveDenied);
    // Nothing was scheduled, so nothing will revert.
    CHECK_FALSE(context.report.revert_pending);
}

// --- Resetting --------------------------------------------------------------

TEST_CASE("an accepted reset waits for the link to drop", "[dfu][machine]")
{
    Context context = fresh();
    const Step step =
        advance(UpdateState::Resetting, just(Event::Kind::ResetAccepted), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::AwaitingDisconnect);
    CHECK(step.effect == Effect::AwaitDisconnect);
}

TEST_CASE("a busy reset is retried once with force", "[dfu][machine]")
{
    Context context = fresh();
    const Error busy{ErrorCode::ProtocolError,
                     MgmtError::smp(static_cast<std::uint16_t>(SmpError::Busy))};

    const Step forced = advance(UpdateState::Resetting, failed(busy), UpdatePlan{}, context);
    CHECK(forced.next == UpdateState::Resetting);
    CHECK(forced.effect == Effect::ForceReset);
    CHECK(context.reset_forced);

    const Step again = advance(UpdateState::Resetting, failed(busy), UpdatePlan{}, context);
    CHECK(again.next == UpdateState::Failed);
}

TEST_CASE("a lost reset response is treated as the reset happening", "[dfu][machine]")
{
    // The device may reset before the answer goes out (A3). Failing here would
    // abandon a device that is already swapping; the verify after the reboot is
    // the real check.
    for (const ErrorCode code : {ErrorCode::Disconnected, ErrorCode::Timeout}) {
        Context context = fresh();
        const Step step = advance(UpdateState::Resetting, failed(code), UpdatePlan{}, context);
        CHECK(step.next == UpdateState::AwaitingDisconnect);
        CHECK(step.effect == Effect::AwaitDisconnect);
        CHECK_FALSE(context.report.cause.has_value());
    }
}

TEST_CASE("a refused reset is fatal, and the scheduled swap is reported", "[dfu][machine]")
{
    Context context = fresh();
    context.swap_scheduled = true;
    const Step step =
        advance(UpdateState::Resetting, failed(ErrorCode::ProtocolError), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Failed);
    CHECK(context.report.revert_pending);
}

// --- Disconnect and reconnect -----------------------------------------------

TEST_CASE("the reconnect is requested whether or not the link actually dropped", "[dfu][machine]")
{
    // A link still up is not proof the device ignored the reset.
    for (const Event::Kind kind : {Event::Kind::Disconnected, Event::Kind::GraceExpired}) {
        Context context = fresh();
        const Step step =
            advance(UpdateState::AwaitingDisconnect, just(kind), UpdatePlan{}, context);
        CHECK(step.next == UpdateState::AwaitingReconnect);
        CHECK(step.effect == Effect::RequestReconnect);
    }
}

TEST_CASE("a reconnect resumes an upload, or verifies the boot", "[dfu][machine]")
{
    Context mid_upload = fresh();
    mid_upload.upload_in_progress = true;
    const Step resumed = advance(UpdateState::AwaitingReconnect, just(Event::Kind::Reconnected),
                                 UpdatePlan{}, mid_upload);
    CHECK(resumed.next == UpdateState::Uploading);
    CHECK(resumed.effect == Effect::ResumeUpload);

    Context after_reset = fresh();
    const Step verified = advance(UpdateState::AwaitingReconnect, just(Event::Kind::Reconnected),
                                  UpdatePlan{}, after_reset);
    CHECK(verified.next == UpdateState::VerifyingBooted);
    CHECK(verified.effect == Effect::ReadState);
}

TEST_CASE("a failed reconnect is fatal and says a revert is pending", "[dfu][machine]")
{
    Context context = fresh();
    context.swap_scheduled = true;
    Event event;
    event.kind = Event::Kind::ReconnectFailed;
    event.error = Error{ErrorCode::Disconnected};

    const Step step = advance(UpdateState::AwaitingReconnect, event, UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Failed);
    CHECK(context.report.revert_pending);
}

// --- Verifying what booted --------------------------------------------------

TEST_CASE("a trial boot is recognised and leads to the confirmation fork", "[dfu][machine]")
{
    Context asked = fresh();
    const ImageState trial = trial_boot();
    const Step step = advance(UpdateState::VerifyingBooted, state_read(trial), UpdatePlan{}, asked);
    CHECK(step.next == UpdateState::AwaitingConfirmation);
    CHECK(step.effect == Effect::RequestConfirmation);
    CHECK(asked.swap_scheduled);

    Context automatic = fresh();
    UpdatePlan plan;
    plan.mode = UpdateMode::ConfirmImmediately;
    const Step confirmed =
        advance(UpdateState::VerifyingBooted, state_read(trial), plan, automatic);
    CHECK(confirmed.next == UpdateState::Confirming);
    CHECK(confirmed.effect == Effect::Confirm);
}

TEST_CASE("an image that booted already confirmed needs nothing further", "[dfu][machine]")
{
    // Where a ConfirmImmediately update whose confirm response was lost lands.
    Context context = fresh();
    context.swap_scheduled = true;
    const ImageState state =
        state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true, .confirmed = true},
                  SlotSpec{.slot = 1, .hash = kOther}});

    const Step step =
        advance(UpdateState::VerifyingBooted, state_read(state), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Completed);
    CHECK(step.effect == Effect::Finish);
    CHECK_FALSE(context.swap_scheduled);
}

TEST_CASE("the old image running with nothing pending is a rollback", "[dfu][machine]")
{
    // The rule the flags exist to protect: decided from them. "Not confirmed"
    // cannot mean "wrong image", because a trial boot reports exactly that.
    Context context = fresh();
    context.swap_scheduled = true;
    const ImageState reverted =
        state_of({SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true},
                  SlotSpec{.slot = 1, .hash = kTarget}});

    const Step step =
        advance(UpdateState::VerifyingBooted, state_read(reverted), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Failed);
    CHECK(context.report.rolled_back);
    // A revert already happened, so nothing further is pending.
    CHECK_FALSE(context.report.revert_pending);
}

TEST_CASE("the old image running with a swap still pending is not a rollback", "[dfu][machine]")
{
    // The swap has not happened yet -- a different failure, and calling it a
    // rollback would tell the caller the device had rejected the image.
    Context context = fresh();
    const ImageState not_yet =
        state_of({SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true},
                  SlotSpec{.slot = 1, .hash = kTarget, .pending = true}});

    const Step step =
        advance(UpdateState::VerifyingBooted, state_read(not_yet), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Failed);
    CHECK_FALSE(context.report.rolled_back);
}

TEST_CASE("a device reporting no active slot after the reboot is a failure", "[dfu][machine]")
{
    Context context = fresh();
    const ImageState nothing = state_of({SlotSpec{.slot = 1, .hash = kTarget}});
    const Step step =
        advance(UpdateState::VerifyingBooted, state_read(nothing), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Failed);
    CHECK_FALSE(context.report.rolled_back);
}

TEST_CASE("the image the update inspects is the one it uploads", "[dfu][machine]")
{
    // Each target names its image. Image 0 running the target says nothing
    // about image 1, which has no active slot at all.
    Context context = fresh(1);
    ImageState booted = state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true}});
    const UpdatePlan plan;
    const Step other = advance(UpdateState::VerifyingBooted, state_read(booted), plan, context);
    CHECK(other.next == UpdateState::Failed);

    for (ImageSlot& slot : booted.slots) {
        slot.image = 1;
    }
    Context same = fresh(1);
    const Step ours = advance(UpdateState::VerifyingBooted, state_read(booted), plan, same);
    CHECK(ours.next != UpdateState::Failed);
}

TEST_CASE("a failed boot verification is fatal", "[dfu][machine]")
{
    Context context = fresh();
    const Step step =
        advance(UpdateState::VerifyingBooted, failed(ErrorCode::Timeout), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Failed);
}

// --- Confirming (ADR-0014) --------------------------------------------------

TEST_CASE("the application's approval moves the update on", "[dfu][machine][confirm]")
{
    Context context = fresh();
    const Step step = advance(UpdateState::AwaitingConfirmation, just(Event::Kind::ConfirmApproved),
                              UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Confirming);
    CHECK(step.effect == Effect::Confirm);
}

TEST_CASE("declining to confirm ends the update with a revert pending", "[dfu][machine][confirm]")
{
    // Not the same as "nothing happened": the device is running the new image
    // and will undo that on its next reset.
    Context context = fresh();
    context.swap_scheduled = true;
    const Step step = advance(UpdateState::AwaitingConfirmation, just(Event::Kind::Cancel),
                              UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Cancelled);
    CHECK(step.effect == Effect::Finish);
    CHECK(context.report.revert_pending);
}

TEST_CASE("an accepted confirm is verified", "[dfu][machine][confirm]")
{
    Context context = fresh();
    context.swap_scheduled = true;
    const Step step =
        advance(UpdateState::Confirming, just(Event::Kind::Confirmed), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::VerifyingConfirmed);
    CHECK(step.effect == Effect::ReadState);
    CHECK_FALSE(context.swap_scheduled);
}

TEST_CASE("a refused confirm is fatal and leaves the device about to revert",
          "[dfu][machine][confirm]")
{
    Context context = fresh();
    context.swap_scheduled = true;
    const Step step =
        advance(UpdateState::Confirming, image_failure(ImageError::ImageConfirmationDenied),
                UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Failed);
    CHECK(context.report.revert_pending);
    REQUIRE(context.report.cause.has_value());
    CHECK(smply::image_error(*context.report.cause) == ImageError::ImageConfirmationDenied);
}

TEST_CASE("the confirmation is checked against the device's own report", "[dfu][machine]")
{
    Context good = fresh();
    const ImageState confirmed =
        state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true, .confirmed = true}});
    const Step done =
        advance(UpdateState::VerifyingConfirmed, state_read(confirmed), UpdatePlan{}, good);
    CHECK(done.next == UpdateState::Completed);
    CHECK(done.effect == Effect::Finish);

    Context bad = fresh();
    const ImageState unconfirmed = state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true}});
    const Step refused =
        advance(UpdateState::VerifyingConfirmed, state_read(unconfirmed), UpdatePlan{}, bad);
    CHECK(refused.next == UpdateState::Failed);
    CHECK(bad.report.revert_pending);
}

TEST_CASE("a failed confirmation read reports the pending revert", "[dfu][machine]")
{
    Context context = fresh();
    const Step step =
        advance(UpdateState::VerifyingConfirmed, failed(ErrorCode::Timeout), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Failed);
    CHECK(context.report.revert_pending);
}

// --- Cancellation and terminal states ---------------------------------------

TEST_CASE("cancellation is legal in every non-terminal state", "[dfu][machine]")
{
    for (const UpdateState state : kNonTerminal) {
        Context context = fresh();
        const Step step = advance(state, just(Event::Kind::Cancel), UpdatePlan{}, context);
        CHECK(step.next == UpdateState::Cancelled);
        CHECK(step.effect == Effect::Finish);
        CHECK_FALSE(context.report.revert_pending);
    }
}

TEST_CASE("a terminal state absorbs everything", "[dfu][machine]")
{
    for (const UpdateState state :
         {UpdateState::Completed, UpdateState::Failed, UpdateState::Cancelled}) {
        Context context = fresh();
        const Step cancelled = advance(state, just(Event::Kind::Cancel), UpdatePlan{}, context);
        CHECK(cancelled.next == state);
        CHECK(cancelled.effect == Effect::None);

        const Step other = advance(state, just(Event::Kind::ResetAccepted), UpdatePlan{}, context);
        CHECK(other.next == state);
        CHECK(other.effect == Effect::None);
    }
}

TEST_CASE("an event with no rule for the state is an internal error", "[dfu][machine]")
{
    // Ignoring it would hide a driver bug; failing makes it visible where it
    // happens. Every state, because "this one silently swallows a stray event"
    // is exactly the kind of hole a spot check leaves.
    for (const UpdateState state : kNonTerminal) {
        Context context = fresh();
        const Step step = advance(state, just(Event::Kind::MarkedForTest), UpdatePlan{}, context);
        if (state == UpdateState::MarkingForTest) {
            CHECK(step.next == UpdateState::Resetting);
            continue;
        }
        CHECK(step.next == UpdateState::Failed);
        REQUIRE(context.report.cause.has_value());
        CHECK(context.report.cause->code() == ErrorCode::Internal);
    }
}

TEST_CASE("planning without a slot table uploads rather than guessing", "[dfu][machine]")
{
    // Every "does the device already have it?" answer needs the table. With no
    // table there is no evidence, and the safe reading is that it does not.
    Context context = fresh();
    const Step step =
        advance(UpdateState::Planning, just(Event::Kind::Continue), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Uploading);
    CHECK(step.effect == Effect::StartUpload);
}

TEST_CASE("planning with no active slot still finds the image", "[dfu][machine][planning]")
{
    // A device reports only *valid* images, so a freshly erased primary slot is
    // simply missing from the table.
    Context context = fresh();
    context.device = state_of({SlotSpec{.slot = 1, .hash = kTarget}});

    const Step step =
        advance(UpdateState::Planning, just(Event::Kind::Continue), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::MarkingForTest);
    CHECK(context.report.upload_skipped);
}

TEST_CASE("UploadOnly stops at every point the image is already there", "[dfu][machine][planning]")
{
    UpdatePlan plan;
    plan.mode = UpdateMode::UploadOnly;

    // Already marked for the next boot.
    Context marked = fresh();
    marked.device =
        state_of({SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true},
                  SlotSpec{.slot = 1, .hash = kTarget, .pending = true}});
    const Step from_marked =
        advance(UpdateState::Planning, just(Event::Kind::Continue), plan, marked);
    CHECK(from_marked.next == UpdateState::Completed);
    CHECK(from_marked.effect == Effect::Finish);

    // Present but unmarked: marking it is activation, which UploadOnly does not
    // do.
    Context present = fresh();
    present.device = running_old_holding_new();
    const Step from_present =
        advance(UpdateState::Planning, just(Event::Kind::Continue), plan, present);
    CHECK(from_present.next == UpdateState::Completed);
    CHECK(from_present.effect == Effect::Finish);
}

TEST_CASE("a slot the device reports without a hash cannot be the target", "[dfu][machine]")
{
    // `hash` is optional on the wire. A slot without one is not evidence of
    // anything, and must never be read as a match.
    Context booted = fresh();
    ImageState nameless = state_of({SlotSpec{.slot = 0, .hash = kOther, .active = true}});
    nameless.slots[0].hash.reset();
    const Step step =
        advance(UpdateState::VerifyingBooted, state_read(nameless), UpdatePlan{}, booted);
    CHECK(step.next == UpdateState::Failed);
    CHECK(booted.report.rolled_back);

    Context confirmed = fresh();
    const Step checked =
        advance(UpdateState::VerifyingConfirmed, state_read(nameless), UpdatePlan{}, confirmed);
    CHECK(checked.next == UpdateState::Failed);
}

TEST_CASE("a confirmation check with no active slot fails", "[dfu][machine]")
{
    Context context = fresh();
    const ImageState nothing = state_of({SlotSpec{.slot = 1, .hash = kTarget}});
    const Step step =
        advance(UpdateState::VerifyingConfirmed, state_read(nothing), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Failed);
    CHECK(context.report.revert_pending);
}

TEST_CASE("every state has a name", "[dfu][machine]")
{
    for (const UpdateState state : kNonTerminal) {
        CHECK_FALSE(smply::to_string(state).empty());
    }
    CHECK(smply::to_string(UpdateState::Completed) == "Completed");
    CHECK(smply::to_string(UpdateState::Failed) == "Failed");
    CHECK(smply::to_string(UpdateState::Cancelled) == "Cancelled");
    CHECK(smply::is_terminal(UpdateState::Completed));
    CHECK_FALSE(smply::is_terminal(UpdateState::Uploading));
}

// --- Several images: every decision is about the target's image (ADR-0021) --

TEST_CASE("another image's pending swap does not hide a revert of this one",
          "[dfu][machine][multi]")
{
    // Image 1 came back on its old image with nothing of its own pending: a
    // rollback. Image 0 happens to have a swap queued, which says nothing
    // about image 1 -- and before scoping, it turned the verdict into "did not
    // boot the new image" instead.
    Context context = fresh(1);
    context.swap_scheduled = true;
    const ImageState state = state_of(
        {SlotSpec{.image = 0, .slot = 0, .hash = kOther, .active = true, .confirmed = true},
         SlotSpec{.image = 0, .slot = 1, .hash = hash_of(50), .pending = true},
         SlotSpec{.image = 1, .slot = 0, .hash = kOther, .active = true, .confirmed = true},
         SlotSpec{.image = 1, .slot = 1, .hash = kTarget}});
    const UpdatePlan plan;

    const Step step = advance(UpdateState::VerifyingBooted, state_read(state), plan, context);
    CHECK(step.next == UpdateState::Failed);
    CHECK(context.report.rolled_back);
}

TEST_CASE("the target held by another image is not held by this one", "[dfu][machine][multi]")
{
    // The device finds a hash in any image, but a copy sitting in image 0's
    // secondary is not an image-1 update that is already staged.
    Context context = fresh(1);
    context.device = state_of(
        {SlotSpec{.image = 0, .slot = 0, .hash = kOther, .active = true, .confirmed = true},
         SlotSpec{.image = 0, .slot = 1, .hash = kTarget, .pending = true},
         SlotSpec{.image = 1, .slot = 0, .hash = hash_of(60), .active = true, .confirmed = true}});
    const UpdatePlan plan;

    const Step step = advance(UpdateState::Planning, just(Event::Kind::Continue), plan, context);
    CHECK(step.next == UpdateState::Uploading);
    CHECK(step.effect == Effect::StartUpload);
    CHECK_FALSE(context.report.upload_skipped);
}

// --- Several images in one update (ADR-0021) --------------------------------

namespace {

const ImageHash kRadio = hash_of(100);
const ImageHash kRadioOld = hash_of(150);

/// Image 0 committed by smply, image 1 by the device: the coordinating-MCU
/// product of docs/multi-image.md.
[[nodiscard]] Context app_and_radio()
{
    return smply::dfu::make_context(
        {Target{.image = 0, .commit = smply::CommitBy::Client, .hash = kTarget},
         Target{.image = 1, .commit = smply::CommitBy::Device, .hash = kRadio}});
}

/// After the one reset: image 0 on trial, image 1 as \p radio describes it.
[[nodiscard]] ImageState after_reset(SlotSpec radio_primary, SlotSpec radio_secondary)
{
    radio_primary.image = 1;
    radio_primary.slot = 0;
    radio_primary.active = true;
    radio_secondary.image = 1;
    radio_secondary.slot = 1;
    return state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true},
                     SlotSpec{.slot = 1, .hash = kOther, .confirmed = true}, radio_primary,
                     radio_secondary});
}

/// The device-commit contract's three answers.
[[nodiscard]] ImageState radio_applying()
{
    return after_reset(SlotSpec{.hash = kRadioOld, .confirmed = true},
                       SlotSpec{.hash = kRadio, .pending = true});
}

[[nodiscard]] ImageState radio_applied()
{
    return after_reset(SlotSpec{.hash = kRadio, .confirmed = true}, SlotSpec{.hash = kRadioOld});
}

[[nodiscard]] ImageState radio_failed()
{
    return after_reset(SlotSpec{.hash = kRadioOld, .confirmed = true}, SlotSpec{.hash = kRadio});
}

} // namespace

TEST_CASE("every image is staged before the one reset", "[dfu][machine][multi]")
{
    Context context = app_and_radio();
    context.device = state_of(
        {SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true},
         SlotSpec{.image = 1, .slot = 0, .hash = kRadioOld, .active = true, .confirmed = true},
         SlotSpec{.image = 1, .slot = 1, .hash = kRadio}});
    const UpdatePlan plan;

    // Image 0 is not on the device: upload it first.
    const Step first = advance(UpdateState::Planning, just(Event::Kind::Continue), plan, context);
    CHECK(first.next == UpdateState::Uploading);
    CHECK(context.current == 0);

    // Marked: on to image 1 rather than to the reset.
    const Step marked =
        advance(UpdateState::MarkingForTest, just(Event::Kind::MarkedForTest), plan, context);
    CHECK(marked.next == UpdateState::Planning);
    CHECK(marked.effect == Effect::Continue);
    CHECK(context.current == 1);

    // Image 1 is already on the device, unmarked: only the mark is needed.
    const Step second = advance(UpdateState::Planning, just(Event::Kind::Continue), plan, context);
    CHECK(second.next == UpdateState::MarkingForTest);
    CHECK(context.report.images[1].upload_skipped);
    CHECK_FALSE(context.report.upload_skipped); // image 0 was not skipped

    const Step last =
        advance(UpdateState::MarkingForTest, just(Event::Kind::MarkedForTest), plan, context);
    CHECK(last.next == UpdateState::Resetting);
    CHECK(last.effect == Effect::Reset);
}

TEST_CASE("the mark-for-test recovery is one per image", "[dfu][machine][multi]")
{
    Context context = app_and_radio();
    const UpdatePlan plan;
    const Event refused = image_failure(ImageError::ImageAlreadyPending);

    CHECK(advance(UpdateState::MarkingForTest, refused, plan, context).next ==
          UpdateState::InspectingImages);
    context.current = 0;
    CHECK(advance(UpdateState::MarkingForTest, just(Event::Kind::MarkedForTest), plan, context)
              .next == UpdateState::Planning);
    CHECK_FALSE(context.mark_retried);
    CHECK(advance(UpdateState::MarkingForTest, refused, plan, context).next ==
          UpdateState::InspectingImages);
}

TEST_CASE("a device image still applying is waited for", "[dfu][machine][multi]")
{
    Context context = app_and_radio();
    const UpdatePlan plan;

    const ImageState applying = radio_applying();
    const Step booted = advance(UpdateState::VerifyingBooted, state_read(applying), plan, context);
    CHECK(booted.next == UpdateState::AwaitingDeviceApply);
    CHECK(booted.effect == Effect::AwaitApply);
    CHECK(context.swap_scheduled); // image 0 is on trial

    const Step due =
        advance(UpdateState::AwaitingDeviceApply, just(Event::Kind::ApplyPollDue), plan, context);
    CHECK(due.next == UpdateState::AwaitingDeviceApply);
    CHECK(due.effect == Effect::ReadState);

    const Step again =
        advance(UpdateState::AwaitingDeviceApply, state_read(applying), plan, context);
    CHECK(again.effect == Effect::AwaitApply);

    // Applied: only now does the confirmation window open, for image 0.
    const ImageState applied = radio_applied();
    const Step done = advance(UpdateState::AwaitingDeviceApply, state_read(applied), plan, context);
    CHECK(done.next == UpdateState::AwaitingConfirmation);
    CHECK(context.current == 0);
    CHECK(context.report.images[1].applied);
}

TEST_CASE("the swap variant's unconfirmed primary is still applying", "[dfu][machine][multi]")
{
    // The device's own MCUboot swapped image 1 in as a trial and is forwarding
    // it (docs/multi-image.md, the nRF5340-style variant).
    Context context = app_and_radio();
    const ImageState swapped =
        after_reset(SlotSpec{.hash = kRadio}, SlotSpec{.hash = kRadioOld, .confirmed = true});
    const Step step =
        advance(UpdateState::VerifyingBooted, state_read(swapped), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::AwaitingDeviceApply);
    CHECK_FALSE(context.report.images[1].applied);
}

TEST_CASE("a failed device apply confirms nothing", "[dfu][machine][multi]")
{
    Context context = app_and_radio();
    const ImageState failed_apply = radio_failed();
    const Step step =
        advance(UpdateState::VerifyingBooted, state_read(failed_apply), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Failed);
    REQUIRE(context.report.cause.has_value());
    CHECK(context.report.cause->code() == ErrorCode::UpdateFailed);
    // Image 0 is left on trial, so it reverts on the next reset.
    CHECK(context.report.revert_pending);
    CHECK_FALSE(context.report.images[1].applied);
    CHECK_FALSE(context.report.rolled_back);
}

TEST_CASE("waiting for the device ends on a timeout or a failed read", "[dfu][machine][multi]")
{
    Context timed_out = app_and_radio();
    timed_out.swap_scheduled = true;
    const Step late = advance(UpdateState::AwaitingDeviceApply, just(Event::Kind::ApplyTimedOut),
                              UpdatePlan{}, timed_out);
    CHECK(late.next == UpdateState::Failed);
    REQUIRE(timed_out.report.cause.has_value());
    CHECK(timed_out.report.cause->code() == ErrorCode::Timeout);
    CHECK(timed_out.report.revert_pending);

    Context unreadable = app_and_radio();
    const Step lost = advance(UpdateState::AwaitingDeviceApply, failed(ErrorCode::Disconnected),
                              UpdatePlan{}, unreadable);
    CHECK(lost.next == UpdateState::Failed);
    REQUIRE(unreadable.report.cause.has_value());
    CHECK(unreadable.report.cause->code() == ErrorCode::Disconnected);
}

TEST_CASE("a revert of one client image fails the update and names it", "[dfu][machine][multi]")
{
    Context context = app_and_radio();
    const ImageState reverted = state_of(
        {SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true},
         SlotSpec{.slot = 1, .hash = kTarget},
         SlotSpec{.image = 1, .slot = 0, .hash = kRadio, .active = true, .confirmed = true}});
    const Step step =
        advance(UpdateState::VerifyingBooted, state_read(reverted), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Failed);
    CHECK(context.report.images[0].rolled_back);
    CHECK(context.report.rolled_back);
    CHECK_FALSE(context.report.revert_pending);
}

TEST_CASE("resuming after the reset goes straight to the wait", "[dfu][machine][multi]")
{
    // A new process, a device part-way through: image 0 on trial means the
    // reset happened, so nothing is staged again -- that would need another
    // reset, which would revert the trial.
    Context applying = app_and_radio();
    applying.device = radio_applying();
    const Step wait =
        advance(UpdateState::Planning, just(Event::Kind::Continue), UpdatePlan{}, applying);
    CHECK(wait.next == UpdateState::AwaitingDeviceApply);
    CHECK(applying.report.upload_skipped);

    // ... and after the device applied its image, to the confirmation window.
    Context applied = app_and_radio();
    applied.device = radio_applied();
    const Step window =
        advance(UpdateState::Planning, just(Event::Kind::Continue), UpdatePlan{}, applied);
    CHECK(window.next == UpdateState::AwaitingConfirmation);

    // ... and a failed apply is reported, not staged again.
    Context failed_apply = app_and_radio();
    failed_apply.device = radio_failed();
    const Step verdict =
        advance(UpdateState::Planning, just(Event::Kind::Continue), UpdatePlan{}, failed_apply);
    CHECK(verdict.next == UpdateState::Failed);
    CHECK(failed_apply.report.revert_pending);
}

TEST_CASE("nothing to stage and nothing on trial completes without a reset",
          "[dfu][machine][multi]")
{
    Context context = app_and_radio();
    context.device = state_of(
        {SlotSpec{.slot = 0, .hash = kTarget, .active = true, .confirmed = true},
         SlotSpec{.image = 1, .slot = 0, .hash = kRadio, .active = true, .confirmed = true}});
    const Step step =
        advance(UpdateState::Planning, just(Event::Kind::Continue), UpdatePlan{}, context);
    CHECK(step.next == UpdateState::Completed);
    CHECK(context.report.upload_skipped);
    CHECK(context.report.images[1].applied);
    CHECK_FALSE(context.report.revert_pending);
}

TEST_CASE("each client image is confirmed in turn, by its own hash", "[dfu][machine][multi]")
{
    Context context = smply::dfu::make_context(
        {Target{.image = 0, .hash = kTarget}, Target{.image = 1, .hash = kRadio}});
    const ImageState trial =
        state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true},
                  SlotSpec{.image = 1, .slot = 0, .hash = kRadio, .active = true}});
    UpdatePlan plan;
    plan.mode = UpdateMode::ConfirmImmediately;

    const Step first = advance(UpdateState::VerifyingBooted, state_read(trial), plan, context);
    CHECK(first.next == UpdateState::Confirming);
    CHECK(context.current == 0);

    const Step second =
        advance(UpdateState::Confirming, just(Event::Kind::Confirmed), plan, context);
    CHECK(second.next == UpdateState::Confirming);
    CHECK(second.effect == Effect::Confirm);
    CHECK(context.current == 1);
    CHECK(context.swap_scheduled); // image 1 is still on trial

    const Step verify =
        advance(UpdateState::Confirming, just(Event::Kind::Confirmed), plan, context);
    CHECK(verify.next == UpdateState::VerifyingConfirmed);
    CHECK_FALSE(context.swap_scheduled);

    // Every client image must read back confirmed, not just the last one.
    const ImageState half =
        state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true, .confirmed = true},
                  SlotSpec{.image = 1, .slot = 0, .hash = kRadio, .active = true}});
    CHECK(advance(UpdateState::VerifyingConfirmed, state_read(half), plan, context).next ==
          UpdateState::Failed);
}

TEST_CASE("UploadOnly uploads every image and stops", "[dfu][machine][multi]")
{
    Context context = app_and_radio();
    UpdatePlan plan;
    plan.mode = UpdateMode::UploadOnly;

    Event finished = just(Event::Kind::UploadFinished);
    finished.transferred = 100;
    const Step next = advance(UpdateState::Uploading, finished, plan, context);
    CHECK(next.next == UpdateState::Planning);
    CHECK(context.current == 1);

    finished.transferred = 50;
    const Step done = advance(UpdateState::Uploading, finished, plan, context);
    CHECK(done.next == UpdateState::Completed);
    CHECK(context.report.bytes_transferred == 150);
    CHECK(context.report.images[0].bytes_transferred == 100);
    CHECK(context.report.images[1].bytes_transferred == 50);
}

TEST_CASE("a context with no image to work on is an internal error", "[dfu][machine][multi]")
{
    Context empty;
    const Step none = advance(UpdateState::Idle, just(Event::Kind::Start), UpdatePlan{}, empty);
    CHECK(none.next == UpdateState::Failed);

    Context past_the_end = fresh();
    past_the_end.current = 1;
    const Step step =
        advance(UpdateState::Confirming, just(Event::Kind::Confirmed), UpdatePlan{}, past_the_end);
    CHECK(step.next == UpdateState::Failed);
    REQUIRE(past_the_end.report.cause.has_value());
    CHECK(past_the_end.report.cause->code() == ErrorCode::Internal);
}
