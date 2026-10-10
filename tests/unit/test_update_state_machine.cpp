// SPDX-License-Identifier: Apache-2.0
//
// The update decision table from docs/design.md section 8, row by row. No
// client, no transport, no clock -- which is the whole point of splitting the
// machine out of `FirmwareUpdater`: every recovery rule is a value-in,
// value-out assertion instead of a scenario needing a device.
//
// Every case drives `dfu::Machine` through its interface: events in, steps
// and the report out. A state is reached by the events that reach it, through
// `Scenario` below, never by setting the machine's working state -- so a state
// the machine cannot reach cannot be tested as if it could.
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
#include "smply/groups/os.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using smply::CommitBy;
using smply::ConstBytes;
using smply::Error;
using smply::ErrorCode;
using smply::Group;
using smply::ImageError;
using smply::ImageHash;
using smply::ImageSlot;
using smply::ImageState;
using smply::ImageVersion;
using smply::McubootMode;
using smply::MgmtError;
using smply::ModeSource;
using smply::Refusal;
using smply::SmpError;
using smply::UpdateMode;
using smply::UpdatePlan;
using smply::UpdateReport;
using smply::UpdateState;
using smply::dfu::Effect;
using smply::dfu::Event;
using smply::dfu::Machine;
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
const ImageHash kRadio = hash_of(100);
const ImageHash kRadioOld = hash_of(150);

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

[[nodiscard]] ImageSlot slot_of(const SlotSpec& spec)
{
    ImageSlot slot;
    slot.image = spec.image;
    slot.slot = spec.slot;
    slot.version = "1.0.0";
    slot.hash = spec.hash;
    slot.bootable = true;
    slot.active = spec.active;
    slot.pending = spec.pending;
    slot.confirmed = spec.confirmed;
    return slot;
}

[[nodiscard]] ImageState state_of(std::initializer_list<SlotSpec> specs)
{
    ImageState out;
    for (const SlotSpec& spec : specs) {
        out.slots.push_back(slot_of(spec));
    }
    return out;
}

/// The old image running, and nothing else: the next step would be an upload.
[[nodiscard]] ImageState running_old_only()
{
    return state_of({SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true}});
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

/// The new image running and confirmed: what a landed confirm reads back.
[[nodiscard]] ImageState booted_confirmed()
{
    return state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true, .confirmed = true},
                     SlotSpec{.slot = 1, .hash = kOther}});
}

[[nodiscard]] Event just(Event::Kind kind)
{
    Event event;
    event.kind = kind;
    return event;
}

[[nodiscard]] Event state_read(const ImageState& state)
{
    Event event;
    event.kind = Event::Kind::StateRead;
    event.state = &state;
    return event;
}

/// The device accepted a mark-for-test and answered with \p state.
[[nodiscard]] Event marked_for_test(const ImageState& state)
{
    Event event;
    event.kind = Event::Kind::MarkedForTest;
    event.state = &state;
    return event;
}

/// The device accepted a confirm and answered with \p state.
[[nodiscard]] Event confirmed(const ImageState& state)
{
    Event event;
    event.kind = Event::Kind::Confirmed;
    event.state = &state;
    return event;
}

[[nodiscard]] Event upload_finished(std::uint64_t transferred)
{
    Event event;
    event.kind = Event::Kind::UploadFinished;
    event.transferred = transferred;
    return event;
}

[[nodiscard]] Event bootloader_read(McubootMode mode, bool no_downgrade = false)
{
    Event event;
    event.kind = Event::Kind::BootloaderRead;
    event.bootloader.mode = mode;
    event.bootloader.raw_mode = static_cast<std::int64_t>(mode);
    event.bootloader.no_downgrade = no_downgrade;
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

/// One `Client` image, \p image, whose file is `kTarget`.
[[nodiscard]] std::vector<Target> one_image(std::uint32_t image = 0)
{
    return {Target{.image = image, .hash = kTarget}};
}

/// Image 0 committed by smply, image 1 by the device: the coordinating-MCU
/// product of docs/multi-image.md.
[[nodiscard]] std::vector<Target> app_and_radio()
{
    return {Target{.image = 0, .commit = CommitBy::Client, .hash = kTarget},
            Target{.image = 1, .commit = CommitBy::Device, .hash = kRadio}};
}

/// Two `Client` images, `kTarget` for image 0 and `kRadio` for image 1.
[[nodiscard]] std::vector<Target> two_client_images()
{
    return {Target{.image = 0, .hash = kTarget}, Target{.image = 1, .hash = kRadio}};
}

/// Drives a `dfu::Machine` the way `FirmwareUpdater` and a cooperative device
/// would, so that a test reaches a state through the events that reach it.
///
/// `next()` answers the last step's effect as the device would: a read returns
/// `device`, an upload puts the file in the image's free slot, a mark sets it
/// pending, the disconnect after a reset swaps every pending image in, a
/// confirm confirms. `reach()` repeats that until the machine is in the state
/// asked for. Between them, `feed()` hands the machine any event a test wants,
/// and the model carries on from whatever the test left.
///
/// The knobs are read when their question is asked, so set them first.
class Scenario
{
public:
    explicit Scenario(const UpdatePlan& plan = UpdatePlan{}) : Scenario{one_image(), plan} {}

    explicit Scenario(std::vector<Target> targets, const UpdatePlan& plan = UpdatePlan{})
        : targets_{std::move(targets)}, machine_{std::make_unique<Machine>(targets_, plan)}
    {
        // Each image runs an old build of its own, confirmed, in its primary.
        for (const Target& target : targets_) {
            device.slots.push_back(slot_of(SlotSpec{.image = target.image,
                                                    .slot = target.image * 2,
                                                    .hash = old_of(target.image),
                                                    .active = true,
                                                    .confirmed = true}));
        }
    }

    /// The slot table the device reports.
    ImageState device;
    /// The bootloader's answer; none when unset.
    std::optional<McubootMode> mode;
    bool no_downgrade = false;
    /// The parameters' answer; none when zero.
    std::uint32_t buf_size = 0;
    /// A `Device` image is still applying after the reset, and is applied at
    /// the first apply poll rather than at the reset.
    bool device_applies_late = false;

    /// Hands the machine \p event.
    Step feed(const Event& event)
    {
        last_ = machine_->apply(event);
        return last_;
    }

    /// Answers the last step as the device would.
    Step next()
    {
        if (machine_->state() == UpdateState::Idle) {
            return feed(just(Event::Kind::Start));
        }
        return feed(answer(last_));
    }

    /// `next()` until the machine is in \p state.
    Scenario& reach(UpdateState state)
    {
        for (int turns = 0; turns < 64 && machine_->state() != state; ++turns) {
            REQUIRE_FALSE(smply::is_terminal(machine_->state()));
            static_cast<void>(next());
        }
        REQUIRE(machine_->state() == state);
        return *this;
    }

    /// Reaches `Planning` and returns the planning decision.
    Step plan()
    {
        reach(UpdateState::Planning);
        return next();
    }

    [[nodiscard]] const Step& last() const noexcept
    {
        return last_;
    }

    [[nodiscard]] UpdateState state() const noexcept
    {
        return machine_->state();
    }

    [[nodiscard]] const UpdateReport& report() const noexcept
    {
        return machine_->report();
    }

private:
    /// The old build an image runs before the update.
    [[nodiscard]] static ImageHash old_of(std::uint32_t image)
    {
        return image == 0 ? kOther : hash_of(static_cast<std::uint8_t>(149 + image));
    }

    [[nodiscard]] Event answer(const Step& step)
    {
        switch (step.effect) {
        case Effect::None:
        case Effect::Finish:
            FAIL("the machine is waiting for nothing the device can answer");
            return Event{};
        case Effect::Continue:
            return just(Event::Kind::Continue);
        case Effect::QueryParameters: {
            if (buf_size == 0) {
                return just(Event::Kind::ParametersUnavailable);
            }
            Event event = just(Event::Kind::ParametersRead);
            event.buf_size = buf_size;
            return event;
        }
        case Effect::QueryBootloader:
            return mode.has_value() ? bootloader_read(*mode, no_downgrade)
                                    : just(Event::Kind::BootloaderUnavailable);
        case Effect::ReadState:
            return state_read(device);
        case Effect::StartUpload:
            upload_ = step;
            upload(step);
            return upload_finished(kUploadBytes);
        case Effect::ResumeUpload:
            upload(upload_);
            return upload_finished(kUploadBytes);
        case Effect::MarkForTest:
            for (ImageSlot& slot : device.slots) {
                slot.pending = slot.pending || (slot.image == step.image && slot.hash == step.hash);
            }
            return marked_for_test(device);
        case Effect::Reset:
        case Effect::ForceReset:
            return just(Event::Kind::ResetAccepted);
        case Effect::AwaitDisconnect:
            swap_in([this](std::uint32_t image) {
                return !device_applies_late || !is_device_image(image);
            });
            return just(Event::Kind::Disconnected);
        case Effect::RequestReconnect:
            return just(Event::Kind::Reconnected);
        case Effect::AwaitApply:
            if (machine_->state() == UpdateState::AwaitingDeviceApply) {
                swap_in([this](std::uint32_t image) { return is_device_image(image); });
            } else {
                for (ImageSlot& slot : device.slots) {
                    slot.confirmed = slot.confirmed || (slot.active && is_device_image(slot.image));
                }
            }
            return just(Event::Kind::ApplyPollDue);
        case Effect::RequestConfirmation:
            return just(Event::Kind::ConfirmApproved);
        case Effect::Confirm:
            for (ImageSlot& slot : device.slots) {
                if (slot.image == step.image) {
                    slot.confirmed = slot.hash == step.hash;
                }
            }
            return confirmed(device);
        }
        return Event{};
    }

    /// Puts the build \p step names in the free slot of its image.
    void upload(const Step& step)
    {
        const Target& target = targets_.at(step.target);
        const ImageHash hash =
            target.builds.has_value() ? (*target.builds).at(step.build).hash : target.hash;
        const ImageSlot* running = device.active_slot(step.image);
        const std::uint32_t base = step.image * 2;
        const std::uint32_t free =
            running != nullptr && running->slot == base + 1 ? base : base + 1;
        for (ImageSlot& slot : device.slots) {
            if (slot.image == step.image && slot.slot == free) {
                slot = slot_of(SlotSpec{.image = step.image, .slot = free, .hash = hash});
                return;
            }
        }
        device.slots.push_back(slot_of(SlotSpec{.image = step.image, .slot = free, .hash = hash}));
    }

    /// Boots the pending build of each image \p which selects, on trial: the
    /// old one stays beside it, confirmed, as MCUboot reports a test swap.
    template<class Which>
    void swap_in(Which which)
    {
        for (ImageSlot& incoming : device.slots) {
            if (!incoming.pending || !which(incoming.image)) {
                continue;
            }
            incoming.pending = false;
            ImageSlot* running = nullptr;
            for (ImageSlot& slot : device.slots) {
                if (slot.image == incoming.image && slot.active) {
                    running = &slot;
                }
            }
            if (running == nullptr) {
                incoming.active = true;
                continue;
            }
            std::swap(running->hash, incoming.hash);
            running->confirmed = false;
            incoming.confirmed = true;
        }
    }

    [[nodiscard]] bool is_device_image(std::uint32_t image) const
    {
        for (const Target& target : targets_) {
            if (target.image == image) {
                return target.commit == CommitBy::Device;
            }
        }
        return false;
    }

    static constexpr std::uint64_t kUploadBytes = 4096;

    std::vector<Target> targets_;
    std::unique_ptr<Machine> machine_;
    Step last_{};
    Step upload_{};
};

/// Every state an update can be in that is not terminal.
constexpr std::array<UpdateState, 17> kNonTerminal{
    UpdateState::Idle,
    UpdateState::QueryingParameters,
    UpdateState::QueryingBootloader,
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
    UpdateState::AwaitingDeviceCommit,
};

/// A scenario that passes through every one of `kNonTerminal`: a `Client` and
/// a `Device` image, the second still applying after the reset.
[[nodiscard]] std::unique_ptr<Scenario> through_every_state()
{
    auto scenario = std::make_unique<Scenario>(app_and_radio());
    scenario->device_applies_late = true;
    return scenario;
}

} // namespace

// --- The scenario helper itself ----------------------------------------------

TEST_CASE("the scenario reaches every non-terminal state through events", "[dfu][machine]")
{
    // The rest of the suite stands on this: if a state could not be reached,
    // a case that reaches it would fail on its own REQUIRE, not pass vacuously.
    for (const UpdateState state : kNonTerminal) {
        CAPTURE(state);
        through_every_state()->reach(state);
    }

    // And the ordinary single-image update runs to the end.
    Scenario single;
    single.reach(UpdateState::VerifyingConfirmed);
    CHECK(single.next().next == UpdateState::Completed);
    CHECK(single.report().final_state == UpdateState::Completed);
}

// --- The happy path, state by state -----------------------------------------

TEST_CASE("a machine starts idle and moves to the state of each step", "[dfu][machine]")
{
    Machine machine{one_image(), UpdatePlan{}};
    CHECK(machine.state() == UpdateState::Idle);
    REQUIRE(machine.report().images.size() == 1);
    CHECK(machine.report().images[0].target_hash == kTarget);

    const Step step = machine.apply(just(Event::Kind::Start));
    CHECK(step.next == UpdateState::QueryingParameters);
    CHECK(machine.state() == UpdateState::QueryingParameters);
}

TEST_CASE("an update starts by asking for the device's buffer budget", "[dfu][machine]")
{
    Scenario scenario;
    const Step step = scenario.next();
    CHECK(step.next == UpdateState::QueryingParameters);
    CHECK(step.effect == Effect::QueryParameters);
}

TEST_CASE("the buffer budget is remembered, and its absence is not fatal", "[dfu][machine]")
{
    // The command is optional; a device without it is ordinary, not broken
    // (docs/protocol-notes.md section 9, A8).
    Scenario with;
    with.buf_size = 512;
    with.reach(UpdateState::QueryingParameters);
    const Step got = with.next();
    CHECK(got.next == UpdateState::QueryingBootloader);
    CHECK(got.effect == Effect::QueryBootloader);
    with.reach(UpdateState::Uploading);
    CHECK(with.last().buf_size == 512);

    Scenario without;
    without.reach(UpdateState::QueryingParameters);
    const Step missing = without.next();
    CHECK(missing.next == UpdateState::QueryingBootloader);
    CHECK(missing.effect == Effect::QueryBootloader);
    without.reach(UpdateState::Uploading);
    CHECK(without.last().buf_size == 0);
}

TEST_CASE("a reported mode is recorded as reported, and the slot table is read next",
          "[dfu][machine][mode]")
{
    // The device's answer wins over the plan's fallback (ADR-0025, decision 1).
    UpdatePlan plan;
    plan.fallback_mode = McubootMode::UpgradeOnly;
    Scenario scenario{plan};
    scenario.reach(UpdateState::QueryingBootloader);
    const Step step = scenario.feed(bootloader_read(McubootMode::SwapUsingMove));
    CHECK(step.next == UpdateState::InspectingImages);
    CHECK(step.effect == Effect::ReadState);
    CHECK(scenario.report().bootloader_mode == McubootMode::SwapUsingMove);
    CHECK(scenario.report().mode_source == ModeSource::Reported);
}

TEST_CASE("no answer uses the plan's fallback, or assumes nothing", "[dfu][machine][mode]")
{
    UpdatePlan supplied;
    supplied.fallback_mode = McubootMode::SwapUsingScratch;

    Scenario with{supplied};
    with.reach(UpdateState::QueryingBootloader);
    const Step step = with.feed(just(Event::Kind::BootloaderUnavailable));
    CHECK(step.next == UpdateState::InspectingImages);
    CHECK(step.effect == Effect::ReadState);
    CHECK(with.report().bootloader_mode == McubootMode::SwapUsingScratch);
    CHECK(with.report().mode_source == ModeSource::Supplied);

    Scenario without;
    without.reach(UpdateState::QueryingBootloader);
    static_cast<void>(without.feed(just(Event::Kind::BootloaderUnavailable)));
    CHECK(without.report().bootloader_mode == McubootMode::Unknown);
    CHECK(without.report().mode_source == ModeSource::Assumed);

    // A fallback of Unknown is the same as none.
    UpdatePlan unknown;
    unknown.fallback_mode = McubootMode::Unknown;
    Scenario nothing{unknown};
    nothing.reach(UpdateState::QueryingBootloader);
    static_cast<void>(nothing.feed(just(Event::Kind::BootloaderUnavailable)));
    CHECK(nothing.report().mode_source == ModeSource::Assumed);
}

TEST_CASE("a reported unknown mode is no answer, so the fallback applies", "[dfu][machine][mode]")
{
    // -1, or a number newer than smply: neither can drive a decision (A38).
    UpdatePlan plan;
    plan.fallback_mode = McubootMode::SwapUsingOffset;
    Scenario scenario{plan};
    scenario.reach(UpdateState::QueryingBootloader);
    static_cast<void>(scenario.feed(bootloader_read(McubootMode::Unknown)));
    CHECK(scenario.report().bootloader_mode == McubootMode::SwapUsingOffset);
    CHECK(scenario.report().mode_source == ModeSource::Supplied);
}

namespace {

/// The planning decision on \p table, from a device reporting \p mode.
[[nodiscard]] Step planned(Scenario& scenario, McubootMode mode, const ImageState& table)
{
    scenario.mode = mode;
    scenario.device = table;
    return scenario.plan();
}

} // namespace

TEST_CASE("an upgrade-only device is refused before the upload unless the plan accepts it",
          "[dfu][machine][mode][refusal]")
{
    const UpdateMode mode = GENERATE(UpdateMode::TestThenConfirm, UpdateMode::ConfirmImmediately);

    UpdatePlan plan;
    plan.mode = mode;
    Scenario refused{plan};
    const Step step = planned(refused, McubootMode::UpgradeOnly, running_old_only());
    CHECK(step.next == UpdateState::Failed);
    REQUIRE(refused.report().cause.has_value());
    CHECK(refused.report().cause->code() == ErrorCode::UpdateRefused);
    CHECK(refused.report().refusal == Refusal::RevertUnavailable);
    CHECK_FALSE(refused.report().revert_pending);

    plan.allow_no_revert = true;
    Scenario accepted{plan};
    const Step upload = planned(accepted, McubootMode::UpgradeOnly, running_old_only());
    CHECK(upload.next == UpdateState::Uploading);
    CHECK_FALSE(accepted.report().refusal.has_value());
}

TEST_CASE("an upload-only update is not refused for a missing revert",
          "[dfu][machine][mode][refusal]")
{
    // UploadOnly promises no trial, so there is no promise to break.
    UpdatePlan plan;
    plan.mode = UpdateMode::UploadOnly;
    Scenario scenario{plan};
    const Step step = planned(scenario, McubootMode::UpgradeOnly, running_old_only());
    CHECK(step.next == UpdateState::Uploading);
}

TEST_CASE("a mode without an update path is refused in every update mode",
          "[dfu][machine][mode][refusal]")
{
    const McubootMode mode = GENERATE(McubootMode::SingleSlot, McubootMode::FirmwareLoader,
                                      McubootMode::RamLoad, McubootMode::SingleSlotRamLoad);
    const UpdateMode update = GENERATE(UpdateMode::TestThenConfirm, UpdateMode::UploadOnly);

    UpdatePlan plan;
    plan.mode = update;
    plan.allow_no_revert = true; // not a way past this one
    Scenario scenario{plan};
    const Step step = planned(scenario, mode, running_old_only());
    CHECK(step.next == UpdateState::Failed);
    CHECK(scenario.report().refusal == Refusal::UnsupportedMode);
}

TEST_CASE("the refusal is checked before marking an image already present",
          "[dfu][machine][mode][refusal]")
{
    Scenario scenario;
    const Step step = planned(scenario, McubootMode::UpgradeOnly, running_old_holding_new());
    CHECK(step.next == UpdateState::Failed);
    CHECK(scenario.report().refusal == Refusal::RevertUnavailable);
}

TEST_CASE("nothing to do is never refused", "[dfu][machine][mode][refusal]")
{
    // The device already runs the image, confirmed: no command would change
    // it, so there is nothing for a refusal to protect.
    const ImageState done =
        state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true, .confirmed = true}});
    Scenario scenario;
    const Step step = planned(scenario, McubootMode::UpgradeOnly, done);
    CHECK(step.next == UpdateState::Completed);
    CHECK_FALSE(scenario.report().refusal.has_value());
}

TEST_CASE("swap modes and an unknown mode are never refused", "[dfu][machine][mode][refusal]")
{
    const McubootMode mode = GENERATE(McubootMode::Unknown, McubootMode::SwapUsingScratch,
                                      McubootMode::SwapUsingMove, McubootMode::SwapUsingOffset);
    Scenario scenario;
    const Step step = planned(scenario, mode, running_old_only());
    CHECK(step.next == UpdateState::Uploading);
}

namespace {

/// The device runs `running` and reports downgrade prevention; the file is
/// `file`.
[[nodiscard]] std::unique_ptr<Scenario>
downgrade_case(const ImageVersion& file, const char* running, const UpdatePlan& plan = UpdatePlan{})
{
    auto scenario = std::make_unique<Scenario>(
        std::vector<Target>{Target{.image = 0, .hash = kTarget, .version = file}}, plan);
    scenario->mode = McubootMode::SwapUsingMove;
    scenario->no_downgrade = true;
    scenario->device = running_old_only();
    scenario->device.slots[0].version = running;
    return scenario;
}

} // namespace

TEST_CASE("an older image is refused when the device prevents downgrades",
          "[dfu][machine][mode][refusal][downgrade]")
{
    // Lower in each position, the higher ones equal.
    const ImageVersion file = GENERATE(ImageVersion{.major = 1, .minor = 2, .revision = 2},
                                       ImageVersion{.major = 1, .minor = 1, .revision = 9},
                                       ImageVersion{.major = 0, .minor = 9, .revision = 9});
    const std::unique_ptr<Scenario> scenario = downgrade_case(file, "1.2.3");
    const Step step = scenario->plan();
    CHECK(step.next == UpdateState::Failed);
    REQUIRE(scenario->report().cause.has_value());
    CHECK(scenario->report().cause->code() == ErrorCode::UpdateRefused);
    CHECK(scenario->report().refusal == Refusal::Downgrade);
}

TEST_CASE("an equal or newer image is not a downgrade", "[dfu][machine][mode][downgrade]")
{
    const ImageVersion file = GENERATE(ImageVersion{.major = 1, .minor = 2, .revision = 3},
                                       ImageVersion{.major = 1, .minor = 2, .revision = 4},
                                       ImageVersion{.major = 1, .minor = 3, .revision = 0},
                                       ImageVersion{.major = 2, .minor = 0, .revision = 0});
    CHECK(downgrade_case(file, "1.2.3")->plan().next == UpdateState::Uploading);
}

TEST_CASE("the build number never makes a downgrade", "[dfu][machine][mode][downgrade]")
{
    // MCUboot ignores it unless built to compare it, which the device does not
    // report; smply ignores it always, so it never refuses what MCUboot takes.
    const std::unique_ptr<Scenario> scenario =
        downgrade_case(ImageVersion{.major = 1, .minor = 2, .revision = 3, .build = 1}, "1.2.3.9");
    CHECK(scenario->plan().next == UpdateState::Uploading);
}

TEST_CASE("the downgrade check can be turned off, and needs the device's flag",
          "[dfu][machine][mode][downgrade]")
{
    const ImageVersion older{.major = 1, .minor = 0, .revision = 0};

    UpdatePlan off;
    off.check_downgrade = false;
    CHECK(downgrade_case(older, "2.0.0", off)->plan().next == UpdateState::Uploading);

    const std::unique_ptr<Scenario> no_flag = downgrade_case(older, "2.0.0");
    no_flag->no_downgrade = false;
    CHECK(no_flag->plan().next == UpdateState::Uploading);
}

TEST_CASE("an unparseable running version is not compared", "[dfu][machine][mode][downgrade]")
{
    // "<???>" is what Zephyr reports when it cannot format the version
    // (docs/protocol-notes.md section 6).
    CHECK(downgrade_case(ImageVersion{.major = 0, .minor = 0, .revision = 1}, R"(<???>)")
              ->plan()
              .next == UpdateState::Uploading);
}

TEST_CASE("the bootloader's flag is remembered from its answer", "[dfu][machine][mode][downgrade]")
{
    // Given once, in the bootloader's answer, and decided on later, at the
    // planning step.
    const std::unique_ptr<Scenario> scenario =
        downgrade_case(ImageVersion{.major = 1, .minor = 0, .revision = 0}, "2.0.0");
    scenario->no_downgrade = false; // the model's own answer would not say it
    scenario->reach(UpdateState::QueryingBootloader);
    static_cast<void>(scenario->feed(bootloader_read(McubootMode::SwapUsingScratch, true)));
    CHECK(scenario->plan().next == UpdateState::Failed);
    CHECK(scenario->report().refusal == Refusal::Downgrade);
}

TEST_CASE("direct-XIP without revert skips the mark and owes a reset", "[dfu][machine][xip]")
{
    // The image is in the free slot after the upload; the device boots it by
    // itself, so the next step is the reset, not set-state (ADR-0025).
    UpdatePlan plan;
    plan.allow_no_revert = true;
    Scenario scenario{plan};
    scenario.mode = McubootMode::DirectXip;
    scenario.reach(UpdateState::VerifyingUpload);
    const ImageState uploaded = state_of({SlotSpec{.slot = 0, .hash = kOther, .active = true},
                                          SlotSpec{.slot = 1, .hash = kTarget, .pending = true}});
    const Step step = scenario.feed(state_read(uploaded));
    CHECK(step.next == UpdateState::Resetting);
    CHECK(step.effect == Effect::Reset);
    CHECK(scenario.report().images[0].upload_slot == 1U);
}

TEST_CASE("an image already in the free slot is not marked under direct-XIP without revert",
          "[dfu][machine][xip]")
{
    UpdatePlan plan;
    plan.allow_no_revert = true;
    const ImageState present = state_of({SlotSpec{.slot = 1, .hash = kOther, .active = true},
                                         SlotSpec{.slot = 0, .hash = kTarget}});
    Scenario scenario{plan};
    const Step step = planned(scenario, McubootMode::DirectXip, present);
    CHECK(step.next == UpdateState::Resetting);
    CHECK(scenario.report().images[0].upload_slot == 0U);
}

TEST_CASE("a direct-XIP image running after the reset is done, never on trial",
          "[dfu][machine][xip]")
{
    // Without set-state nothing is ever reported confirmed, and nothing needs
    // a confirm: the update ends here.
    UpdatePlan plan;
    plan.allow_no_revert = true;
    Scenario scenario{plan};
    scenario.mode = McubootMode::DirectXip;
    scenario.reach(UpdateState::VerifyingBooted);
    const ImageState booted = state_of({SlotSpec{.slot = 1, .hash = kTarget, .active = true},
                                        SlotSpec{.slot = 0, .hash = kOther}});
    const Step step = scenario.feed(state_read(booted));
    CHECK(step.next == UpdateState::Completed);
}

TEST_CASE("the same booted-unconfirmed state with revert opens the confirmation window",
          "[dfu][machine][xip]")
{
    Scenario scenario;
    scenario.mode = McubootMode::DirectXipWithRevert;
    scenario.reach(UpdateState::VerifyingBooted);
    const ImageState booted = state_of({SlotSpec{.slot = 1, .hash = kTarget, .active = true},
                                        SlotSpec{.slot = 0, .hash = kOther, .confirmed = true}});
    const Step step = scenario.feed(state_read(booted));
    CHECK(step.next == UpdateState::AwaitingConfirmation);
}

TEST_CASE("direct-XIP refusals: no revert, and more than one image", "[dfu][machine][xip][refusal]")
{
    Scenario single;
    CHECK(planned(single, McubootMode::DirectXip, running_old_only()).next == UpdateState::Failed);
    CHECK(single.report().refusal == Refusal::RevertUnavailable);

    Scenario with_revert;
    CHECK(planned(with_revert, McubootMode::DirectXipWithRevert, running_old_only()).next ==
          UpdateState::Uploading);

    const McubootMode mode = GENERATE(McubootMode::DirectXip, McubootMode::DirectXipWithRevert);
    UpdatePlan plan;
    plan.allow_no_revert = true;
    Scenario two{two_client_images(), plan};
    two.mode = mode;
    CHECK(two.plan().next == UpdateState::Failed);
    CHECK(two.report().refusal == Refusal::MultiImageUnsupported);
}

TEST_CASE("every refusal has a name", "[dfu][machine][refusal]")
{
    for (const Refusal refusal : {Refusal::RevertUnavailable, Refusal::Downgrade,
                                  Refusal::UnsupportedMode, Refusal::MultiImageUnsupported}) {
        CHECK_FALSE(smply::to_string(refusal).empty());
    }
}

TEST_CASE("a failed bootloader query is fatal and changes nothing", "[dfu][machine][mode]")
{
    Scenario scenario;
    scenario.reach(UpdateState::QueryingBootloader);
    const Step step = scenario.feed(failed(ErrorCode::Timeout));
    CHECK(step.next == UpdateState::Failed);
    CHECK(step.effect == Effect::Finish);
    REQUIRE(scenario.report().cause.has_value());
    CHECK(scenario.report().cause->code() == ErrorCode::Timeout);
    CHECK_FALSE(scenario.report().revert_pending);
}

TEST_CASE("reading the slot table leads to a planning step", "[dfu][machine]")
{
    Scenario scenario;
    scenario.reach(UpdateState::InspectingImages);
    const ImageState state = running_old_holding_new();
    const Step step = scenario.feed(state_read(state));
    CHECK(step.next == UpdateState::Planning);
    CHECK(step.effect == Effect::Continue);
    // ... and the plan is made on the table read: the image is already there.
    CHECK(scenario.next().next == UpdateState::MarkingForTest);
}

TEST_CASE("a failure while inspecting is fatal and changes nothing", "[dfu][machine]")
{
    Scenario scenario;
    scenario.reach(UpdateState::InspectingImages);
    const Step step = scenario.feed(failed(ErrorCode::Timeout));
    CHECK(step.next == UpdateState::Failed);
    CHECK(step.effect == Effect::Finish);
    REQUIRE(scenario.report().cause.has_value());
    CHECK(scenario.report().cause->code() == ErrorCode::Timeout);
    CHECK_FALSE(scenario.report().revert_pending);
}

// --- Planning ---------------------------------------------------------------

namespace {

/// Whether cancelling now reports a revert pending: the observable sign that a
/// swap is scheduled and not yet confirmed.
[[nodiscard]] bool swap_scheduled(Scenario& scenario)
{
    const Step cancelled = scenario.feed(just(Event::Kind::Cancel));
    REQUIRE(cancelled.next == UpdateState::Cancelled);
    return scenario.report().revert_pending;
}

} // namespace

TEST_CASE("an image the device is already running and has confirmed is done",
          "[dfu][machine][planning]")
{
    Scenario scenario;
    scenario.device =
        state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true, .confirmed = true}});
    const Step step = scenario.plan();
    CHECK(step.next == UpdateState::Completed);
    CHECK(step.effect == Effect::Finish);
    CHECK(scenario.report().upload_skipped);
}

TEST_CASE("an image already running unconfirmed lands in the confirmation window",
          "[dfu][machine][planning]")
{
    // A trial boot somebody else started -- an application restarted mid-update
    // arrives here, and must not re-upload or re-reset.
    Scenario asked;
    asked.device = trial_boot();
    const Step step = asked.plan();
    CHECK(step.next == UpdateState::AwaitingConfirmation);
    CHECK(step.effect == Effect::RequestConfirmation);
    CHECK(swap_scheduled(asked));

    UpdatePlan plan;
    plan.mode = UpdateMode::ConfirmImmediately;
    Scenario automatic{plan};
    automatic.device = trial_boot();
    const Step confirming = automatic.plan();
    CHECK(confirming.next == UpdateState::Confirming);
    CHECK(confirming.effect == Effect::Confirm);

    UpdatePlan stop_early;
    stop_early.mode = UpdateMode::UploadOnly;
    Scenario upload_only{stop_early};
    upload_only.device = trial_boot();
    CHECK(upload_only.plan().next == UpdateState::Completed);
}

TEST_CASE("an image already marked for the next boot skips to the reset",
          "[dfu][machine][planning]")
{
    // The mark succeeded even if its response was lost. Re-sending it would be
    // refused with ImageAlreadyPending, so the plan steps over it.
    Scenario scenario;
    scenario.device =
        state_of({SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true},
                  SlotSpec{.slot = 1, .hash = kTarget, .pending = true}});
    const Step step = scenario.plan();
    CHECK(step.next == UpdateState::Resetting);
    CHECK(step.effect == Effect::Reset);
    CHECK(scenario.report().upload_skipped);
    CHECK(swap_scheduled(scenario));
}

TEST_CASE("an image present but unmarked is marked without uploading", "[dfu][machine][planning]")
{
    Scenario scenario;
    scenario.device = running_old_holding_new();
    const Step step = scenario.plan();
    CHECK(step.next == UpdateState::MarkingForTest);
    CHECK(step.effect == Effect::MarkForTest);
    CHECK(scenario.report().upload_skipped);
}

TEST_CASE("the pre-flight skip can be switched off", "[dfu][machine][planning]")
{
    // Turning it off costs a round trip rather than the transfer: the server
    // runs the same check on the first packet (section 6, rule 9a).
    UpdatePlan plan;
    plan.skip_if_already_present = false;
    Scenario scenario{plan};
    scenario.device = running_old_holding_new();
    const Step step = scenario.plan();
    CHECK(step.next == UpdateState::Uploading);
    CHECK(step.effect == Effect::StartUpload);
}

TEST_CASE("an image the device does not hold is uploaded", "[dfu][machine][planning]")
{
    Scenario scenario;
    const Step step = scenario.plan();
    CHECK(step.next == UpdateState::Uploading);
    CHECK(step.effect == Effect::StartUpload);
    CHECK_FALSE(scenario.report().upload_skipped);
}

// --- Uploading --------------------------------------------------------------

TEST_CASE("a finished upload is verified, unless the caller only wanted the upload",
          "[dfu][machine]")
{
    Scenario scenario;
    scenario.reach(UpdateState::Uploading);
    const Step step = scenario.feed(upload_finished(4096));
    CHECK(step.next == UpdateState::VerifyingUpload);
    CHECK(step.effect == Effect::ReadState);
    CHECK(scenario.report().bytes_transferred == 4096);

    UpdatePlan plan;
    plan.mode = UpdateMode::UploadOnly;
    Scenario stop_early{plan};
    stop_early.reach(UpdateState::Uploading);
    const Step stopped = stop_early.feed(upload_finished(4096));
    CHECK(stopped.next == UpdateState::Completed);
    CHECK(stopped.effect == Effect::Finish);
}

TEST_CASE("a dropped link suspends the upload rather than ending it", "[dfu][machine]")
{
    // The device keeps its session and resumes by `sha` (section 6, rule 6).
    Scenario scenario;
    scenario.reach(UpdateState::Uploading);
    const Step step = scenario.feed(failed(ErrorCode::Disconnected));
    CHECK(step.next == UpdateState::AwaitingReconnect);
    CHECK(step.effect == Effect::RequestReconnect);
    CHECK_FALSE(scenario.report().cause.has_value());

    const Step resumed = scenario.feed(just(Event::Kind::Reconnected));
    CHECK(resumed.next == UpdateState::Uploading);
    CHECK(resumed.effect == Effect::ResumeUpload);
}

TEST_CASE("any other upload failure is fatal", "[dfu][machine]")
{
    Scenario scenario;
    scenario.reach(UpdateState::Uploading);
    const Step step = scenario.feed(failed(ErrorCode::ImageMismatch));
    CHECK(step.next == UpdateState::Failed);
    REQUIRE(scenario.report().cause.has_value());
    CHECK(scenario.report().cause->code() == ErrorCode::ImageMismatch);
}

// --- Verifying the upload ---------------------------------------------------

TEST_CASE("an uploaded image must appear in the slot table", "[dfu][machine]")
{
    Scenario present;
    present.reach(UpdateState::VerifyingUpload);
    const ImageState holding = running_old_holding_new();
    const Step step = present.feed(state_read(holding));
    CHECK(step.next == UpdateState::MarkingForTest);
    CHECK(step.effect == Effect::MarkForTest);

    Scenario absent;
    absent.reach(UpdateState::VerifyingUpload);
    const ImageState empty = running_old_only();
    const Step missing = absent.feed(state_read(empty));
    CHECK(missing.next == UpdateState::Failed);
    REQUIRE(absent.report().cause.has_value());
    CHECK(absent.report().cause->code() == ErrorCode::ImageMismatch);
}

TEST_CASE("a failed verification read is fatal", "[dfu][machine]")
{
    Scenario scenario;
    scenario.reach(UpdateState::VerifyingUpload);
    CHECK(scenario.feed(failed(ErrorCode::Timeout)).next == UpdateState::Failed);
}

// --- Marking for test -------------------------------------------------------

TEST_CASE("marking for test schedules a swap and resets", "[dfu][machine]")
{
    Scenario scenario;
    scenario.reach(UpdateState::MarkingForTest);
    const ImageState answer =
        state_of({SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true},
                  SlotSpec{.slot = 1, .hash = kTarget, .pending = true}});
    const Step step = scenario.feed(marked_for_test(answer));
    CHECK(step.next == UpdateState::Resetting);
    CHECK(step.effect == Effect::Reset);
    CHECK(swap_scheduled(scenario));
}

TEST_CASE("ImageAlreadyPending is recoverable exactly once", "[dfu][machine]")
{
    // Re-reading the state and finding our own image marked means the previous
    // attempt worked and its response was lost. A second one is a real refusal.
    Scenario scenario;
    scenario.reach(UpdateState::MarkingForTest);
    const Step retried = scenario.feed(image_failure(ImageError::ImageAlreadyPending));
    CHECK(retried.next == UpdateState::InspectingImages);
    CHECK(retried.effect == Effect::ReadState);
    CHECK_FALSE(scenario.report().cause.has_value());

    // The re-read finds the image unmarked, so it is marked again.
    scenario.reach(UpdateState::MarkingForTest);
    const Step again = scenario.feed(image_failure(ImageError::ImageAlreadyPending));
    CHECK(again.next == UpdateState::Failed);
    REQUIRE(scenario.report().cause.has_value());
}

TEST_CASE("a group-less BadState recovers the mark exactly once", "[dfu][machine]")
{
    // The same rule over SMP v1. A server with
    // CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL translates the image code
    // onto `mcumgr_err_t` and drops the group, so the recovery above never
    // sees `ImageAlreadyPending`. Branching on that code alone, it could not
    // fire at all.
    Scenario scenario;
    scenario.reach(UpdateState::MarkingForTest);
    const Step retried = scenario.feed(flat_failure(SmpError::BadState));
    CHECK(retried.next == UpdateState::InspectingImages);
    CHECK(retried.effect == Effect::ReadState);
    CHECK_FALSE(scenario.report().cause.has_value());

    scenario.reach(UpdateState::MarkingForTest);
    const Step again = scenario.feed(flat_failure(SmpError::BadState));
    CHECK(again.next == UpdateState::Failed);
    REQUIRE(scenario.report().cause.has_value());
}

TEST_CASE("the budget is one recovery, not one of each shape", "[dfu][machine]")
{
    // Spending it on either shape spends it for both: a device that answers
    // v2 once and v1 once is still one lost response, not two. The v1 shape
    // goes first deliberately -- that ordering is the one that fails if the
    // flat arm is ever removed again, and an invariant test that passes either
    // way protects nothing.
    Scenario scenario;
    scenario.reach(UpdateState::MarkingForTest);
    const Step first = scenario.feed(flat_failure(SmpError::BadState));
    CHECK(first.next == UpdateState::InspectingImages);

    scenario.reach(UpdateState::MarkingForTest);
    const Step second = scenario.feed(image_failure(ImageError::ImageAlreadyPending));
    CHECK(second.next == UpdateState::Failed);
}

TEST_CASE("a group-less code that is not BadState is still fatal", "[dfu][machine]")
{
    // `Unknown` is what A24 actually measured on the bench, and it is where
    // the widening deliberately stops: the same translation table gives it to
    // eighteen other image codes, every flash failure among them, so treating
    // it as recoverable would retry genuine refusals.
    Scenario scenario;
    scenario.reach(UpdateState::MarkingForTest);
    const Step step = scenario.feed(flat_failure(SmpError::Unknown));
    CHECK(step.next == UpdateState::Failed);
    REQUIRE(scenario.report().cause.has_value());
    CHECK(smply::smp_error(*scenario.report().cause) == SmpError::Unknown);
}

TEST_CASE("marking the running slot for test is fatal with the device's own code", "[dfu][machine]")
{
    Scenario scenario;
    scenario.reach(UpdateState::MarkingForTest);
    const Step step = scenario.feed(image_failure(ImageError::ImageSettingTestToActiveDenied));
    CHECK(step.next == UpdateState::Failed);
    REQUIRE(scenario.report().cause.has_value());
    CHECK(smply::image_error(*scenario.report().cause) ==
          ImageError::ImageSettingTestToActiveDenied);
    // Nothing was scheduled, so nothing will revert.
    CHECK_FALSE(scenario.report().revert_pending);
}

// --- Resetting --------------------------------------------------------------

TEST_CASE("an accepted reset waits for the link to drop", "[dfu][machine]")
{
    Scenario scenario;
    scenario.reach(UpdateState::Resetting);
    const Step step = scenario.feed(just(Event::Kind::ResetAccepted));
    CHECK(step.next == UpdateState::AwaitingDisconnect);
    CHECK(step.effect == Effect::AwaitDisconnect);
}

TEST_CASE("a busy reset is retried once with force", "[dfu][machine]")
{
    Scenario scenario;
    scenario.reach(UpdateState::Resetting);
    const Error busy{ErrorCode::ProtocolError,
                     MgmtError::smp(static_cast<std::uint16_t>(SmpError::Busy))};

    const Step forced = scenario.feed(failed(busy));
    CHECK(forced.next == UpdateState::Resetting);
    CHECK(forced.effect == Effect::ForceReset);

    const Step again = scenario.feed(failed(busy));
    CHECK(again.next == UpdateState::Failed);
}

TEST_CASE("a lost reset response is treated as the reset happening", "[dfu][machine]")
{
    // The device may reset before the answer goes out (A3). Failing here would
    // abandon a device that is already swapping; the verify after the reboot is
    // the real check.
    for (const ErrorCode code : {ErrorCode::Disconnected, ErrorCode::Timeout}) {
        Scenario scenario;
        scenario.reach(UpdateState::Resetting);
        const Step step = scenario.feed(failed(code));
        CHECK(step.next == UpdateState::AwaitingDisconnect);
        CHECK(step.effect == Effect::AwaitDisconnect);
        CHECK_FALSE(scenario.report().cause.has_value());
    }
}

TEST_CASE("a refused reset is fatal, and the scheduled swap is reported", "[dfu][machine]")
{
    Scenario scenario;
    scenario.reach(UpdateState::Resetting);
    const Step step = scenario.feed(failed(ErrorCode::ProtocolError));
    CHECK(step.next == UpdateState::Failed);
    CHECK(scenario.report().revert_pending);
}

// --- Disconnect and reconnect -----------------------------------------------

TEST_CASE("the reconnect is requested whether or not the link actually dropped", "[dfu][machine]")
{
    // A link still up is not proof the device ignored the reset.
    for (const Event::Kind kind : {Event::Kind::Disconnected, Event::Kind::GraceExpired}) {
        Scenario scenario;
        scenario.reach(UpdateState::AwaitingDisconnect);
        const Step step = scenario.feed(just(kind));
        CHECK(step.next == UpdateState::AwaitingReconnect);
        CHECK(step.effect == Effect::RequestReconnect);
    }
}

TEST_CASE("a reconnect resumes an upload, or verifies the boot", "[dfu][machine]")
{
    Scenario mid_upload;
    mid_upload.reach(UpdateState::Uploading);
    static_cast<void>(mid_upload.feed(failed(ErrorCode::Disconnected)));
    const Step resumed = mid_upload.feed(just(Event::Kind::Reconnected));
    CHECK(resumed.next == UpdateState::Uploading);
    CHECK(resumed.effect == Effect::ResumeUpload);

    Scenario after_reset;
    after_reset.reach(UpdateState::AwaitingReconnect);
    const Step verified = after_reset.feed(just(Event::Kind::Reconnected));
    CHECK(verified.next == UpdateState::VerifyingBooted);
    CHECK(verified.effect == Effect::ReadState);
}

TEST_CASE("a failed reconnect is fatal and says a revert is pending", "[dfu][machine]")
{
    Scenario scenario;
    scenario.reach(UpdateState::AwaitingReconnect);
    Event event = just(Event::Kind::ReconnectFailed);
    event.error = Error{ErrorCode::Disconnected};

    const Step step = scenario.feed(event);
    CHECK(step.next == UpdateState::Failed);
    CHECK(scenario.report().revert_pending);
}

// --- Verifying what booted --------------------------------------------------

TEST_CASE("a trial boot is recognised and leads to the confirmation fork", "[dfu][machine]")
{
    const ImageState trial = trial_boot();

    Scenario asked;
    asked.reach(UpdateState::VerifyingBooted);
    const Step step = asked.feed(state_read(trial));
    CHECK(step.next == UpdateState::AwaitingConfirmation);
    CHECK(step.effect == Effect::RequestConfirmation);
    CHECK(swap_scheduled(asked));

    UpdatePlan plan;
    plan.mode = UpdateMode::ConfirmImmediately;
    Scenario automatic{plan};
    automatic.reach(UpdateState::VerifyingBooted);
    const Step confirming = automatic.feed(state_read(trial));
    CHECK(confirming.next == UpdateState::Confirming);
    CHECK(confirming.effect == Effect::Confirm);
}

TEST_CASE("an image that booted already confirmed needs nothing further", "[dfu][machine]")
{
    // Where a ConfirmImmediately update whose confirm response was lost lands.
    Scenario scenario;
    scenario.reach(UpdateState::VerifyingBooted);
    const ImageState state = booted_confirmed();
    const Step step = scenario.feed(state_read(state));
    CHECK(step.next == UpdateState::Completed);
    CHECK(step.effect == Effect::Finish);
    CHECK_FALSE(scenario.report().revert_pending);
}

TEST_CASE("the old image running with nothing pending is a rollback", "[dfu][machine]")
{
    // The rule the flags exist to protect: decided from them. "Not confirmed"
    // cannot mean "wrong image", because a trial boot reports exactly that.
    Scenario scenario;
    scenario.reach(UpdateState::VerifyingBooted);
    const ImageState reverted = running_old_holding_new();
    const Step step = scenario.feed(state_read(reverted));
    CHECK(step.next == UpdateState::Failed);
    CHECK(scenario.report().rolled_back);
    // A revert already happened, so nothing further is pending.
    CHECK_FALSE(scenario.report().revert_pending);
}

TEST_CASE("the old image running with a swap still pending is not a rollback", "[dfu][machine]")
{
    // The swap has not happened yet -- a different failure, and calling it a
    // rollback would tell the caller the device had rejected the image.
    Scenario scenario;
    scenario.reach(UpdateState::VerifyingBooted);
    const ImageState not_yet =
        state_of({SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true},
                  SlotSpec{.slot = 1, .hash = kTarget, .pending = true}});
    const Step step = scenario.feed(state_read(not_yet));
    CHECK(step.next == UpdateState::Failed);
    CHECK_FALSE(scenario.report().rolled_back);
}

TEST_CASE("a device reporting no active slot after the reboot is a failure", "[dfu][machine]")
{
    Scenario scenario;
    scenario.reach(UpdateState::VerifyingBooted);
    const ImageState nothing = state_of({SlotSpec{.slot = 1, .hash = kTarget}});
    const Step step = scenario.feed(state_read(nothing));
    CHECK(step.next == UpdateState::Failed);
    CHECK_FALSE(scenario.report().rolled_back);
}

TEST_CASE("the image the update inspects is the one it uploads", "[dfu][machine]")
{
    // Each target names its image. Image 0 running the target says nothing
    // about image 1, which has no active slot at all.
    ImageState booted = state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true}});
    Scenario other{one_image(1)};
    other.reach(UpdateState::VerifyingBooted);
    CHECK(other.feed(state_read(booted)).next == UpdateState::Failed);

    for (ImageSlot& slot : booted.slots) {
        slot.image = 1;
    }
    Scenario same{one_image(1)};
    same.reach(UpdateState::VerifyingBooted);
    CHECK(same.feed(state_read(booted)).next != UpdateState::Failed);
}

TEST_CASE("a failed boot verification is fatal", "[dfu][machine]")
{
    Scenario scenario;
    scenario.reach(UpdateState::VerifyingBooted);
    CHECK(scenario.feed(failed(ErrorCode::Timeout)).next == UpdateState::Failed);
}

// --- Confirming (ADR-0014) --------------------------------------------------

TEST_CASE("the application's approval moves the update on", "[dfu][machine][confirm]")
{
    Scenario scenario;
    scenario.reach(UpdateState::AwaitingConfirmation);
    const Step step = scenario.feed(just(Event::Kind::ConfirmApproved));
    CHECK(step.next == UpdateState::Confirming);
    CHECK(step.effect == Effect::Confirm);
    CHECK(step.image == 0);
    CHECK(step.hash == kTarget);
}

TEST_CASE("declining to confirm ends the update with a revert pending", "[dfu][machine][confirm]")
{
    // Not the same as "nothing happened": the device is running the new image
    // and will undo that on its next reset.
    Scenario scenario;
    scenario.reach(UpdateState::AwaitingConfirmation);
    const Step step = scenario.feed(just(Event::Kind::Cancel));
    CHECK(step.next == UpdateState::Cancelled);
    CHECK(step.effect == Effect::Finish);
    CHECK(scenario.report().revert_pending);
}

TEST_CASE("an accepted confirm is verified", "[dfu][machine][confirm]")
{
    Scenario scenario;
    scenario.reach(UpdateState::Confirming);
    const ImageState answer = booted_confirmed();
    const Step step = scenario.feed(confirmed(answer));
    CHECK(step.next == UpdateState::VerifyingConfirmed);
    CHECK(step.effect == Effect::ReadState);
    CHECK_FALSE(swap_scheduled(scenario));
}

TEST_CASE("a refused confirm is fatal and leaves the device about to revert",
          "[dfu][machine][confirm]")
{
    Scenario scenario;
    scenario.reach(UpdateState::Confirming);
    const Step step = scenario.feed(image_failure(ImageError::ImageConfirmationDenied));
    CHECK(step.next == UpdateState::Failed);
    CHECK(scenario.report().revert_pending);
    REQUIRE(scenario.report().cause.has_value());
    CHECK(smply::image_error(*scenario.report().cause) == ImageError::ImageConfirmationDenied);
}

TEST_CASE("the confirmation is checked against the device's own report", "[dfu][machine]")
{
    Scenario good;
    good.reach(UpdateState::VerifyingConfirmed);
    const ImageState read_back =
        state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true, .confirmed = true}});
    const Step done = good.feed(state_read(read_back));
    CHECK(done.next == UpdateState::Completed);
    CHECK(done.effect == Effect::Finish);

    Scenario bad;
    bad.reach(UpdateState::VerifyingConfirmed);
    const ImageState unconfirmed = state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true}});
    const Step refused = bad.feed(state_read(unconfirmed));
    CHECK(refused.next == UpdateState::Failed);
    CHECK(bad.report().revert_pending);
}

TEST_CASE("a lost link or answer around the confirm is re-inspected, not fatal",
          "[dfu][machine][confirm]")
{
    // ADR-0023. Whether the confirm landed is unknown, so the machine assumes
    // nothing: a drop reconnects, a lost answer is read again once, and
    // VerifyingBooted decides.
    const ImageState trial = trial_boot();
    const ImageState landed = booted_confirmed();
    for (const UpdateState state : {UpdateState::Confirming, UpdateState::VerifyingConfirmed}) {
        CAPTURE(state);

        Scenario dropped;
        dropped.reach(state);
        const Step reconnect = dropped.feed(failed(ErrorCode::Disconnected));
        CHECK(reconnect.next == UpdateState::AwaitingReconnect);
        CHECK(reconnect.effect == Effect::RequestReconnect);
        CHECK(swap_scheduled(dropped));

        Scenario lost;
        lost.reach(state);
        const Step reread = lost.feed(failed(ErrorCode::Timeout));
        CHECK(reread.next == UpdateState::VerifyingBooted);
        CHECK(reread.effect == Effect::ReadState);

        // The re-read is spent once per update; a second lost answer is fatal
        // and reports the revert that may still come. The re-read finds the
        // image still on trial, so it is confirmed again first.
        REQUIRE(lost.feed(state_read(trial)).next == UpdateState::Confirming);
        if (state == UpdateState::VerifyingConfirmed) {
            REQUIRE(lost.feed(confirmed(landed)).next == UpdateState::VerifyingConfirmed);
        }
        const Step again = lost.feed(failed(ErrorCode::Timeout));
        CHECK(again.next == UpdateState::Failed);
        CHECK(lost.report().revert_pending);

        // Anything else is fatal, as before.
        Scenario broken;
        broken.reach(state);
        const Step fatal = broken.feed(failed(ErrorCode::MalformedMessage));
        CHECK(fatal.next == UpdateState::Failed);
        CHECK(broken.report().revert_pending);
    }
}

TEST_CASE("after a lost confirm, the re-inspection routes on what the device reports",
          "[dfu][machine][confirm]")
{
    // The confirm landed: done, and nothing is reported as reverting.
    Scenario landed;
    landed.reach(UpdateState::Confirming);
    static_cast<void>(landed.feed(failed(ErrorCode::Disconnected)));
    REQUIRE(landed.feed(just(Event::Kind::Reconnected)).next == UpdateState::VerifyingBooted);
    const ImageState read_back = booted_confirmed();
    const Step done = landed.feed(state_read(read_back));
    CHECK(done.next == UpdateState::Completed);
    CHECK_FALSE(landed.report().revert_pending);

    // It did not land, and the application had already approved: confirm
    // again without asking a second time.
    Scenario approved;
    approved.reach(UpdateState::Confirming);
    static_cast<void>(approved.feed(failed(ErrorCode::Timeout)));
    const ImageState trial = trial_boot();
    const Step retry = approved.feed(state_read(trial));
    CHECK(retry.next == UpdateState::Confirming);
    CHECK(retry.effect == Effect::Confirm);

    // Without an approval on record the application is asked, as on any
    // trial boot.
    Scenario unasked;
    unasked.reach(UpdateState::VerifyingBooted);
    CHECK(unasked.feed(state_read(trial)).next == UpdateState::AwaitingConfirmation);
}

// --- Cancellation and terminal states ---------------------------------------

TEST_CASE("cancellation is legal in every non-terminal state", "[dfu][machine]")
{
    // A revert is reported pending exactly while a swap is scheduled and not
    // yet confirmed: from the first mark to the confirm.
    const std::array<UpdateState, 7> scheduled{
        UpdateState::Resetting,           UpdateState::AwaitingDisconnect,
        UpdateState::AwaitingReconnect,   UpdateState::VerifyingBooted,
        UpdateState::AwaitingDeviceApply, UpdateState::AwaitingConfirmation,
        UpdateState::Confirming,
    };
    for (const UpdateState state : kNonTerminal) {
        CAPTURE(state);
        const std::unique_ptr<Scenario> scenario = through_every_state();
        scenario->reach(state);
        const Step step = scenario->feed(just(Event::Kind::Cancel));
        CHECK(step.next == UpdateState::Cancelled);
        CHECK(step.effect == Effect::Finish);
        CHECK(scenario->report().revert_pending ==
              (std::ranges::find(scheduled, state) != scheduled.end()));
    }
}

TEST_CASE("a terminal state absorbs everything", "[dfu][machine]")
{
    Scenario completed;
    completed.device =
        state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true, .confirmed = true}});
    REQUIRE(completed.plan().next == UpdateState::Completed);

    Scenario failure;
    failure.reach(UpdateState::InspectingImages);
    REQUIRE(failure.feed(failed(ErrorCode::Timeout)).next == UpdateState::Failed);

    Scenario cancelled;
    REQUIRE(cancelled.feed(just(Event::Kind::Cancel)).next == UpdateState::Cancelled);

    for (Scenario* scenario : {&completed, &failure, &cancelled}) {
        const UpdateState state = scenario->state();
        CAPTURE(state);
        const Step again = scenario->feed(just(Event::Kind::Cancel));
        CHECK(again.next == state);
        CHECK(again.effect == Effect::None);

        const Step other = scenario->feed(just(Event::Kind::ResetAccepted));
        CHECK(other.next == state);
        CHECK(other.effect == Effect::None);
    }
}

TEST_CASE("an event with no rule for the state is an internal error", "[dfu][machine]")
{
    // Ignoring it would hide a driver bug; failing makes it visible where it
    // happens. Every state, because "this one silently swallows a stray event"
    // is exactly the kind of hole a spot check leaves.
    const ImageState answer = running_old_holding_new();
    for (const UpdateState state : kNonTerminal) {
        CAPTURE(state);
        const std::unique_ptr<Scenario> scenario = through_every_state();
        scenario->reach(state);
        const Step step = scenario->feed(marked_for_test(answer));
        if (state == UpdateState::MarkingForTest) {
            // The one state it is legal in: on to the second image.
            CHECK(step.next == UpdateState::Planning);
            continue;
        }
        CHECK(step.next == UpdateState::Failed);
        REQUIRE(scenario->report().cause.has_value());
        CHECK(scenario->report().cause->code() == ErrorCode::Internal);
    }
}

TEST_CASE("planning on an empty slot table uploads rather than guessing", "[dfu][machine]")
{
    // Every "does the device already have it?" answer needs the table. With
    // nothing in it there is no evidence, and the safe reading is that it does
    // not.
    Scenario scenario;
    scenario.device = ImageState{};
    const Step step = scenario.plan();
    CHECK(step.next == UpdateState::Uploading);
    CHECK(step.effect == Effect::StartUpload);
}

TEST_CASE("planning with no active slot still finds the image", "[dfu][machine][planning]")
{
    // A device reports only *valid* images, so a freshly erased primary slot is
    // simply missing from the table.
    Scenario scenario;
    scenario.device = state_of({SlotSpec{.slot = 1, .hash = kTarget}});
    CHECK(scenario.plan().next == UpdateState::MarkingForTest);
    CHECK(scenario.report().upload_skipped);
}

TEST_CASE("UploadOnly stops at every point the image is already there", "[dfu][machine][planning]")
{
    UpdatePlan plan;
    plan.mode = UpdateMode::UploadOnly;

    // Already marked for the next boot.
    Scenario marked{plan};
    marked.device =
        state_of({SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true},
                  SlotSpec{.slot = 1, .hash = kTarget, .pending = true}});
    const Step from_marked = marked.plan();
    CHECK(from_marked.next == UpdateState::Completed);
    CHECK(from_marked.effect == Effect::Finish);

    // Present but unmarked: marking it is activation, which UploadOnly does not
    // do.
    Scenario present{plan};
    present.device = running_old_holding_new();
    const Step from_present = present.plan();
    CHECK(from_present.next == UpdateState::Completed);
    CHECK(from_present.effect == Effect::Finish);
}

TEST_CASE("a slot the device reports without a hash cannot be the target", "[dfu][machine]")
{
    // `hash` is optional on the wire. A slot without one is not evidence of
    // anything, and must never be read as a match.
    ImageState nameless = state_of({SlotSpec{.slot = 0, .hash = kOther, .active = true}});
    nameless.slots[0].hash.reset();

    Scenario booted;
    booted.reach(UpdateState::VerifyingBooted);
    CHECK(booted.feed(state_read(nameless)).next == UpdateState::Failed);
    CHECK(booted.report().rolled_back);

    Scenario confirming;
    confirming.reach(UpdateState::VerifyingConfirmed);
    CHECK(confirming.feed(state_read(nameless)).next == UpdateState::Failed);
}

TEST_CASE("a confirmation check with no active slot fails", "[dfu][machine]")
{
    Scenario scenario;
    scenario.reach(UpdateState::VerifyingConfirmed);
    const ImageState nothing = state_of({SlotSpec{.slot = 1, .hash = kTarget}});
    CHECK(scenario.feed(state_read(nothing)).next == UpdateState::Failed);
    CHECK(scenario.report().revert_pending);
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

TEST_CASE("the report holds the end only once the update has ended", "[dfu][machine]")
{
    // The final state, target hash and last slot table are filled on the step
    // that finishes the update, and default before it (stage 1's contract).
    Scenario scenario;
    scenario.reach(UpdateState::VerifyingConfirmed);
    CHECK(scenario.report().final_state == UpdateState::Idle);
    CHECK_FALSE(scenario.report().target_hash.has_value());
    CHECK_FALSE(scenario.report().final_device_state.has_value());

    REQUIRE(scenario.next().next == UpdateState::Completed);
    CHECK(scenario.report().final_state == UpdateState::Completed);
    CHECK(scenario.report().target_hash == kTarget);
    REQUIRE(scenario.report().final_device_state.has_value());
    const ImageSlot* active = scenario.report().final_device_state->active_slot(0);
    REQUIRE(active != nullptr);
    CHECK(active->hash == kTarget);
    CHECK(active->confirmed);
}

// --- Several images: every decision is about the target's image (ADR-0021) --

TEST_CASE("another image's pending swap does not hide a revert of this one",
          "[dfu][machine][multi]")
{
    // Image 1 came back on its old image with nothing of its own pending: a
    // rollback. Image 0 happens to have a swap queued, which says nothing
    // about image 1 -- and before scoping, it turned the verdict into "did not
    // boot the new image" instead.
    Scenario scenario{one_image(1)};
    scenario.reach(UpdateState::VerifyingBooted);
    const ImageState state = state_of(
        {SlotSpec{.image = 0, .slot = 0, .hash = kOther, .active = true, .confirmed = true},
         SlotSpec{.image = 0, .slot = 1, .hash = hash_of(50), .pending = true},
         SlotSpec{.image = 1, .slot = 0, .hash = kOther, .active = true, .confirmed = true},
         SlotSpec{.image = 1, .slot = 1, .hash = kTarget}});
    CHECK(scenario.feed(state_read(state)).next == UpdateState::Failed);
    CHECK(scenario.report().rolled_back);
}

TEST_CASE("the target held by another image is not held by this one", "[dfu][machine][multi]")
{
    // The device finds a hash in any image, but a copy sitting in image 0's
    // secondary is not an image-1 update that is already staged.
    Scenario scenario{one_image(1)};
    scenario.device = state_of(
        {SlotSpec{.image = 0, .slot = 0, .hash = kOther, .active = true, .confirmed = true},
         SlotSpec{.image = 0, .slot = 1, .hash = kTarget, .pending = true},
         SlotSpec{.image = 1, .slot = 0, .hash = hash_of(60), .active = true, .confirmed = true}});
    const Step step = scenario.plan();
    CHECK(step.next == UpdateState::Uploading);
    CHECK(step.effect == Effect::StartUpload);
    CHECK_FALSE(scenario.report().upload_skipped);
}

// --- Several images in one update (ADR-0021) --------------------------------

namespace {

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
    Scenario scenario{app_and_radio()};
    scenario.device = state_of(
        {SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true},
         SlotSpec{.image = 1, .slot = 0, .hash = kRadioOld, .active = true, .confirmed = true},
         SlotSpec{.image = 1, .slot = 1, .hash = kRadio}});

    // Image 0 is not on the device: upload it first.
    const Step first = scenario.plan();
    CHECK(first.next == UpdateState::Uploading);
    CHECK(first.target == 0);
    CHECK(first.image == 0);
    scenario.reach(UpdateState::MarkingForTest);

    // Marked: on to image 1 rather than to the reset.
    const ImageState first_answer = state_of(
        {SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true},
         SlotSpec{.slot = 1, .hash = kTarget, .pending = true},
         SlotSpec{.image = 1, .slot = 0, .hash = kRadioOld, .active = true, .confirmed = true},
         SlotSpec{.image = 1, .slot = 1, .hash = kRadio}});
    const Step marked = scenario.feed(marked_for_test(first_answer));
    CHECK(marked.next == UpdateState::Planning);
    CHECK(marked.effect == Effect::Continue);

    // Image 1 is already on the device, unmarked: only the mark is needed.
    const Step second = scenario.next();
    CHECK(second.next == UpdateState::MarkingForTest);
    CHECK(second.image == 1);
    CHECK(second.hash == kRadio);
    CHECK(scenario.report().images[1].upload_skipped);
    CHECK_FALSE(scenario.report().upload_skipped); // image 0 was not skipped

    const ImageState last_answer = state_of(
        {SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true},
         SlotSpec{.slot = 1, .hash = kTarget, .pending = true},
         SlotSpec{.image = 1, .slot = 0, .hash = kRadioOld, .active = true, .confirmed = true},
         SlotSpec{.image = 1, .slot = 1, .hash = kRadio, .pending = true}});
    const Step last = scenario.feed(marked_for_test(last_answer));
    CHECK(last.next == UpdateState::Resetting);
    CHECK(last.effect == Effect::Reset);
}

TEST_CASE("the mark-for-test recovery is one per image", "[dfu][machine][multi]")
{
    Scenario scenario{app_and_radio()};
    const Event refused = image_failure(ImageError::ImageAlreadyPending);

    // Image 0 spends its recovery, and is then marked.
    scenario.reach(UpdateState::MarkingForTest);
    REQUIRE(scenario.last().image == 0);
    CHECK(scenario.feed(refused).next == UpdateState::InspectingImages);
    scenario.reach(UpdateState::MarkingForTest);
    REQUIRE(scenario.last().image == 0);
    CHECK(scenario.next().next == UpdateState::Planning);

    // Image 1 has a recovery of its own.
    scenario.reach(UpdateState::MarkingForTest);
    REQUIRE(scenario.last().image == 1);
    CHECK(scenario.feed(refused).next == UpdateState::InspectingImages);
}

TEST_CASE("the slot table a mark-for-test returns routes the next image", "[dfu][machine][multi]")
{
    // The set-state answer is the device's whole slot table, and the machine
    // decides the next image on it -- the same contract in a unit test as in
    // `FirmwareUpdater`, which hands the answer over in the event rather than
    // writing it behind the machine's back.
    Scenario scenario{two_client_images()};
    scenario.device = state_of(
        {SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true},
         SlotSpec{.image = 1, .slot = 0, .hash = kRadioOld, .active = true, .confirmed = true}});

    // Neither image is on the device: image 0 is uploaded and verified first.
    scenario.reach(UpdateState::MarkingForTest);
    REQUIRE(scenario.last().image == 0);

    // The answer to the mark shows image 1's file already staged and marked,
    // so image 1 needs no upload: straight on to the one reset.
    const ImageState answer = state_of(
        {SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true},
         SlotSpec{.slot = 1, .hash = kTarget, .pending = true},
         SlotSpec{.image = 1, .slot = 0, .hash = kRadioOld, .active = true, .confirmed = true},
         SlotSpec{.image = 1, .slot = 1, .hash = kRadio, .pending = true}});
    REQUIRE(scenario.feed(marked_for_test(answer)).next == UpdateState::Planning);

    const Step next = scenario.next();
    CHECK(next.next == UpdateState::Resetting);
    CHECK(next.effect == Effect::Reset);
    CHECK(scenario.report().images[1].upload_skipped);
    CHECK(scenario.report().images[1].upload_slot == 1);
}

TEST_CASE("an effect names everything the updater needs to carry it out", "[dfu][machine][multi]")
{
    // The updater carries out an effect from the step alone: which target and
    // build to send, to which image, with which buffer budget, and which hash
    // to mark or confirm. It never reads the machine's working state.
    Scenario scenario{two_client_images()};
    scenario.buf_size = 512;

    const Step upload = scenario.plan();
    REQUIRE(upload.effect == Effect::StartUpload);
    CHECK(upload.target == 0);
    CHECK(upload.build == 0);
    CHECK(upload.image == 0);
    CHECK(upload.buf_size == 512);

    REQUIRE(scenario.next().next == UpdateState::VerifyingUpload);
    const Step mark = scenario.next();
    REQUIRE(mark.effect == Effect::MarkForTest);
    CHECK(mark.image == 0);
    CHECK(mark.hash == kTarget);

    REQUIRE(scenario.next().next == UpdateState::Planning);
    const Step second = scenario.next();
    REQUIRE(second.effect == Effect::StartUpload);
    CHECK(second.target == 1);
    CHECK(second.image == 1);
    CHECK(second.buf_size == 512);
}

TEST_CASE("the upload names the build chosen for the free slot", "[dfu][machine][xip]")
{
    const ImageHash primary_build = hash_of(70);
    const ImageHash secondary_build = hash_of(80);
    Scenario scenario{{Target{.image = 0,
                              .hash = primary_build,
                              .builds = std::array<Target::Build, 2>{
                                  Target::Build{.hash = primary_build, .version = {}},
                                  Target::Build{.hash = secondary_build, .version = {}}}}}};

    // Running from the primary slot, so the secondary's build is the one sent.
    const Step upload = planned(scenario, McubootMode::DirectXipWithRevert, running_old_only());
    REQUIRE(upload.effect == Effect::StartUpload);
    CHECK(upload.target == 0);
    CHECK(upload.build == 1);
    CHECK(upload.buf_size == 0);
    CHECK(scenario.report().images[0].target_hash == secondary_build);
}

TEST_CASE("a device image still applying is waited for", "[dfu][machine][multi]")
{
    Scenario scenario{app_and_radio()};
    scenario.reach(UpdateState::VerifyingBooted);

    const ImageState applying = radio_applying();
    const Step booted = scenario.feed(state_read(applying));
    CHECK(booted.next == UpdateState::AwaitingDeviceApply);
    CHECK(booted.effect == Effect::AwaitApply);

    const Step due = scenario.feed(just(Event::Kind::ApplyPollDue));
    CHECK(due.next == UpdateState::AwaitingDeviceApply);
    CHECK(due.effect == Effect::ReadState);

    const Step again = scenario.feed(state_read(applying));
    CHECK(again.effect == Effect::AwaitApply);

    // Applied: only now does the confirmation window open, for image 0.
    const ImageState applied = radio_applied();
    const Step done = scenario.feed(state_read(applied));
    CHECK(done.next == UpdateState::AwaitingConfirmation);
    CHECK(scenario.report().images[1].applied);
    const Step confirm = scenario.feed(just(Event::Kind::ConfirmApproved));
    CHECK(confirm.effect == Effect::Confirm);
    CHECK(confirm.image == 0);
}

TEST_CASE("a device image on trial in slot 0 is applied, not yet committed",
          "[dfu][machine][multi]")
{
    // ADR-0022: slot 0 reports what the other MCU runs. The new image there,
    // unconfirmed, is running on trial -- time to open the window for image 0.
    Scenario scenario{app_and_radio()};
    scenario.reach(UpdateState::VerifyingBooted);
    const ImageState on_trial =
        after_reset(SlotSpec{.hash = kRadio}, SlotSpec{.hash = kRadioOld, .confirmed = true});
    CHECK(scenario.feed(state_read(on_trial)).next == UpdateState::AwaitingConfirmation);
    CHECK(scenario.report().images[1].applied);
    CHECK_FALSE(scenario.report().images[1].committed);
}

TEST_CASE("a failed device apply confirms nothing", "[dfu][machine][multi]")
{
    Scenario scenario{app_and_radio()};
    scenario.reach(UpdateState::VerifyingBooted);
    const ImageState failed_apply = radio_failed();
    CHECK(scenario.feed(state_read(failed_apply)).next == UpdateState::Failed);
    REQUIRE(scenario.report().cause.has_value());
    CHECK(scenario.report().cause->code() == ErrorCode::UpdateFailed);
    // Image 0 is left on trial, so it reverts on the next reset.
    CHECK(scenario.report().revert_pending);
    CHECK_FALSE(scenario.report().images[1].applied);
    CHECK_FALSE(scenario.report().rolled_back);
}

TEST_CASE("waiting for the device ends on a timeout, and survives a dropped link",
          "[dfu][machine][multi]")
{
    for (const UpdateState wait :
         {UpdateState::AwaitingDeviceApply, UpdateState::AwaitingDeviceCommit}) {
        CAPTURE(wait);
        const std::unique_ptr<Scenario> timed_out = through_every_state();
        timed_out->reach(wait);
        const Step late = timed_out->feed(just(Event::Kind::ApplyTimedOut));
        CHECK(late.next == UpdateState::Failed);
        REQUIRE(timed_out->report().cause.has_value());
        CHECK(timed_out->report().cause->code() == ErrorCode::Timeout);
        // Before the confirm image 0 reverts; after it, nothing does.
        CHECK(timed_out->report().revert_pending == (wait == UpdateState::AwaitingDeviceApply));

        // The link runs through the MCU being updated: a drop asks for a
        // reconnect, and the wait carries on afterwards (ADR-0022).
        const std::unique_ptr<Scenario> dropped = through_every_state();
        dropped->reach(wait);
        const Step lost = dropped->feed(failed(ErrorCode::Disconnected));
        CHECK(lost.next == UpdateState::AwaitingReconnect);
        CHECK(lost.effect == Effect::RequestReconnect);
        CHECK_FALSE(dropped->report().cause.has_value());

        // A lost answer is asked again at the next poll.
        const std::unique_ptr<Scenario> silent = through_every_state();
        silent->reach(wait);
        const Step again = silent->feed(failed(ErrorCode::Timeout));
        CHECK(again.next == wait);
        CHECK(again.effect == Effect::AwaitApply);

        // Anything else is still fatal.
        const std::unique_ptr<Scenario> refused = through_every_state();
        refused->reach(wait);
        CHECK(refused->feed(image_failure(ImageError::Unknown)).next == UpdateState::Failed);
    }
}

namespace {

/// Image 0 confirmed on its new image, image 1 as \p radio describes it.
[[nodiscard]] ImageState after_confirm(SlotSpec radio_primary, SlotSpec radio_secondary)
{
    radio_primary.image = 1;
    radio_primary.slot = 0;
    radio_primary.active = true;
    radio_secondary.image = 1;
    radio_secondary.slot = 1;
    return state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true, .confirmed = true},
                     SlotSpec{.slot = 1, .hash = kOther}, radio_primary, radio_secondary});
}

} // namespace

TEST_CASE("after the confirm, smply waits for the device to commit its image",
          "[dfu][machine][multi]")
{
    const ImageState on_trial =
        after_confirm(SlotSpec{.hash = kRadio}, SlotSpec{.hash = kRadioOld, .confirmed = true});
    const ImageState committed =
        after_confirm(SlotSpec{.hash = kRadio, .confirmed = true}, SlotSpec{.hash = kRadioOld});
    const ImageState reverted =
        after_confirm(SlotSpec{.hash = kRadioOld, .confirmed = true}, SlotSpec{.hash = kRadio});

    Scenario scenario{app_and_radio()};
    scenario.reach(UpdateState::VerifyingConfirmed);
    const Step verified = scenario.feed(state_read(on_trial));
    CHECK(verified.next == UpdateState::AwaitingDeviceCommit);
    CHECK(verified.effect == Effect::AwaitApply);

    REQUIRE(scenario.feed(just(Event::Kind::ApplyPollDue)).effect == Effect::ReadState);
    CHECK(scenario.feed(state_read(on_trial)).next == UpdateState::AwaitingDeviceCommit);

    REQUIRE(scenario.feed(just(Event::Kind::ApplyPollDue)).effect == Effect::ReadState);
    CHECK(scenario.feed(state_read(committed)).next == UpdateState::Completed);
    CHECK(scenario.report().images[1].committed);
    CHECK_FALSE(scenario.report().revert_pending);

    // The other MCU reverted its trial instead: reported, and nothing reverts
    // here -- image 0 is already confirmed.
    Scenario lost{app_and_radio()};
    lost.reach(UpdateState::AwaitingDeviceCommit);
    REQUIRE(lost.feed(just(Event::Kind::ApplyPollDue)).effect == Effect::ReadState);
    CHECK(lost.feed(state_read(reverted)).next == UpdateState::Failed);
    REQUIRE(lost.report().cause.has_value());
    CHECK(lost.report().cause->code() == ErrorCode::UpdateFailed);
    CHECK_FALSE(lost.report().revert_pending);
    CHECK_FALSE(lost.report().images[1].committed);
}

TEST_CASE("with no client image on trial, the commit is waited for at once",
          "[dfu][machine][multi]")
{
    // Only image 1 changed, so no confirm will come and the device commits on
    // its own. The same state is where a reconnect, or a new process, lands
    // after the confirm.
    const ImageState on_trial =
        after_confirm(SlotSpec{.hash = kRadio}, SlotSpec{.hash = kRadioOld, .confirmed = true});

    Scenario booted{app_and_radio()};
    booted.reach(UpdateState::VerifyingBooted);
    CHECK(booted.feed(state_read(on_trial)).next == UpdateState::AwaitingDeviceCommit);

    Scenario resumed{app_and_radio()};
    resumed.device = on_trial;
    CHECK(resumed.plan().next == UpdateState::AwaitingDeviceCommit);
    CHECK(resumed.report().upload_skipped);
}

TEST_CASE("a revert of one client image fails the update and names it", "[dfu][machine][multi]")
{
    Scenario scenario{app_and_radio()};
    scenario.reach(UpdateState::VerifyingBooted);
    const ImageState reverted = state_of(
        {SlotSpec{.slot = 0, .hash = kOther, .active = true, .confirmed = true},
         SlotSpec{.slot = 1, .hash = kTarget},
         SlotSpec{.image = 1, .slot = 0, .hash = kRadio, .active = true, .confirmed = true}});
    CHECK(scenario.feed(state_read(reverted)).next == UpdateState::Failed);
    CHECK(scenario.report().images[0].rolled_back);
    CHECK(scenario.report().rolled_back);
    CHECK_FALSE(scenario.report().revert_pending);
}

TEST_CASE("resuming after the reset goes straight to the wait", "[dfu][machine][multi]")
{
    // A new process, a device part-way through: image 0 on trial means the
    // reset happened, so nothing is staged again -- that would need another
    // reset, which would revert the trial.
    Scenario applying{app_and_radio()};
    applying.device = radio_applying();
    CHECK(applying.plan().next == UpdateState::AwaitingDeviceApply);
    CHECK(applying.report().upload_skipped);

    // ... and after the device applied its image, to the confirmation window.
    Scenario applied{app_and_radio()};
    applied.device = radio_applied();
    CHECK(applied.plan().next == UpdateState::AwaitingConfirmation);

    // ... and a failed apply is reported, not staged again.
    Scenario failed_apply{app_and_radio()};
    failed_apply.device = radio_failed();
    CHECK(failed_apply.plan().next == UpdateState::Failed);
    CHECK(failed_apply.report().revert_pending);
}

TEST_CASE("nothing to stage and nothing on trial completes without a reset",
          "[dfu][machine][multi]")
{
    Scenario scenario{app_and_radio()};
    scenario.device = state_of(
        {SlotSpec{.slot = 0, .hash = kTarget, .active = true, .confirmed = true},
         SlotSpec{.image = 1, .slot = 0, .hash = kRadio, .active = true, .confirmed = true}});
    CHECK(scenario.plan().next == UpdateState::Completed);
    CHECK(scenario.report().upload_skipped);
    CHECK(scenario.report().images[1].applied);
    CHECK_FALSE(scenario.report().revert_pending);
}

TEST_CASE("a link lost after the confirm goes on to wait for the device's commit",
          "[dfu][machine][multi]")
{
    // The product's case: the device takes the link down to commit the other
    // MCU just after smply's confirm (ADR-0023).
    Scenario scenario{app_and_radio()};
    scenario.reach(UpdateState::VerifyingConfirmed);
    CHECK(scenario.feed(failed(ErrorCode::Disconnected)).next == UpdateState::AwaitingReconnect);
    CHECK(scenario.feed(just(Event::Kind::Reconnected)).next == UpdateState::VerifyingBooted);

    const ImageState on_trial =
        after_confirm(SlotSpec{.hash = kRadio}, SlotSpec{.hash = kRadioOld, .confirmed = true});
    const Step wait = scenario.feed(state_read(on_trial));
    CHECK(wait.next == UpdateState::AwaitingDeviceCommit);
    CHECK(wait.effect == Effect::AwaitApply);
    // Image 0 is confirmed, so a failure now leaves nothing to revert.
    CHECK(scenario.feed(just(Event::Kind::ApplyTimedOut)).next == UpdateState::Failed);
    CHECK_FALSE(scenario.report().revert_pending);
}

TEST_CASE("each client image is confirmed in turn, by its own hash", "[dfu][machine][multi]")
{
    UpdatePlan plan;
    plan.mode = UpdateMode::ConfirmImmediately;
    Scenario scenario{two_client_images(), plan};
    scenario.reach(UpdateState::VerifyingBooted);

    const ImageState trial =
        state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true},
                  SlotSpec{.image = 1, .slot = 0, .hash = kRadio, .active = true}});
    const Step first = scenario.feed(state_read(trial));
    CHECK(first.next == UpdateState::Confirming);
    CHECK(first.image == 0);
    CHECK(first.hash == kTarget);

    const ImageState first_confirmed =
        state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true, .confirmed = true},
                  SlotSpec{.image = 1, .slot = 0, .hash = kRadio, .active = true}});
    const Step second = scenario.feed(confirmed(first_confirmed));
    CHECK(second.next == UpdateState::Confirming);
    CHECK(second.effect == Effect::Confirm);
    CHECK(second.image == 1);
    CHECK(second.hash == kRadio);

    const ImageState both_confirmed = state_of(
        {SlotSpec{.slot = 0, .hash = kTarget, .active = true, .confirmed = true},
         SlotSpec{.image = 1, .slot = 0, .hash = kRadio, .active = true, .confirmed = true}});
    CHECK(scenario.feed(confirmed(both_confirmed)).next == UpdateState::VerifyingConfirmed);

    // Every client image must read back confirmed, not just the last one.
    const ImageState half =
        state_of({SlotSpec{.slot = 0, .hash = kTarget, .active = true, .confirmed = true},
                  SlotSpec{.image = 1, .slot = 0, .hash = kRadio, .active = true}});
    CHECK(scenario.feed(state_read(half)).next == UpdateState::Failed);
}

TEST_CASE("an image still on trial after a confirm keeps the swap scheduled",
          "[dfu][machine][multi]")
{
    // Between the two confirms image 1 is still on trial: an update that ends
    // there leaves it to revert.
    UpdatePlan plan;
    plan.mode = UpdateMode::ConfirmImmediately;
    Scenario scenario{two_client_images(), plan};
    scenario.reach(UpdateState::Confirming);
    REQUIRE(scenario.next().image == 1);
    CHECK(swap_scheduled(scenario));
}

TEST_CASE("UploadOnly uploads every image and stops", "[dfu][machine][multi]")
{
    UpdatePlan plan;
    plan.mode = UpdateMode::UploadOnly;
    Scenario scenario{app_and_radio(), plan};
    scenario.reach(UpdateState::Uploading);

    const Step next = scenario.feed(upload_finished(100));
    CHECK(next.next == UpdateState::Planning);
    const Step second = scenario.next();
    CHECK(second.next == UpdateState::Uploading);
    CHECK(second.target == 1);

    const Step done = scenario.feed(upload_finished(50));
    CHECK(done.next == UpdateState::Completed);
    CHECK(scenario.report().bytes_transferred == 150);
    CHECK(scenario.report().images[0].bytes_transferred == 100);
    CHECK(scenario.report().images[1].bytes_transferred == 50);
}

TEST_CASE("a machine with no image to work on is an internal error", "[dfu][machine][multi]")
{
    // The updater never builds one, and a machine that indexes its targets
    // refuses rather than read past them.
    Machine empty{{}, UpdatePlan{}};
    const Step none = empty.apply(just(Event::Kind::Start));
    CHECK(none.next == UpdateState::Failed);
    CHECK(none.effect == Effect::Finish);
    REQUIRE(empty.report().cause.has_value());
    CHECK(empty.report().cause->code() == ErrorCode::Internal);
    CHECK(empty.report().final_state == UpdateState::Failed);
}
