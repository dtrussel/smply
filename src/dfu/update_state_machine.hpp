// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SRC_DFU_UPDATE_STATE_MACHINE_HPP
#define SMPLY_SRC_DFU_UPDATE_STATE_MACHINE_HPP

/// \file
/// The update decision logic: `Machine`, events in, steps out.
///
/// No client, no transport, no clock, no `ImageSource`: `Machine::apply()`
/// says what should happen next and `FirmwareUpdater` does it. That is what makes every
/// row of the failure/recovery table in docs/design.md section 8 a
/// value-in, value-out unit test rather than a scenario needing a device --
/// the same split ADR-0008 established for the upload, and for the same reason.
///
/// Three rules live here and nowhere else, each read out of the server's
/// source (docs/protocol-notes.md section 7):
///
/// * **A rollback is recognised from the flags, not from a hash alone.** After
///   a trial boot the running image reports `active` with *no* `confirmed`, so
///   "not confirmed" cannot mean "wrong image". A revert is: the active slot
///   does not carry the target hash **and** nothing is pending.
/// * **A refused mark-for-test is recoverable once.** Re-reading the state and
///   finding our own image already marked means the previous attempt succeeded
///   and its response was lost. The refusal arrives in two shapes and both are
///   accepted: `ImageError::ImageAlreadyPending` over SMP v2, and a group-less
///   `SmpError::BadState` from a v1 server that translated the group code away
///   (docs/protocol-notes.md section 9, A16 and A24).
/// * **A refused confirm is fatal *and* leaves the device about to revert.**
///   The report has to say so; "failed" alone would let a caller believe
///   nothing had changed.
///
/// And one from ADR-0021, for an update of several images:
///
/// * **`Client` images are confirmed only after every `Device` image is
///   reported applied.** A failed or timed-out apply leaves them unconfirmed,
///   so the device reverts them, rather than keeping half a package.

#include "smply/dfu/firmware_updater.hpp"
#include "smply/error.hpp"
#include "smply/groups/image.hpp"
#include "smply/mcuboot_image.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace smply::dfu {

/// What `FirmwareUpdater` must do to carry out a step. The parameters an
/// effect needs travel in the `Step` beside it, so the updater needs nothing
/// else from the machine to carry one out.
enum class Effect : std::uint8_t
{
    /// Nothing to do; the machine waits for an external event.
    None,
    /// Feed the machine a `Continue` event immediately. Used where a decision
    /// needs no I/O, so that the state it is decided in is still a real state
    /// with a real transition rather than something invisible.
    Continue,
    QueryParameters,
    QueryBootloader,
    ReadState,
    /// Upload `Step::build` of `Step::target` to `Step::image`, with
    /// `Step::buf_size` as the device's budget when it reported one.
    StartUpload,
    ResumeUpload,
    /// Mark `Step::hash` of `Step::image` for test.
    MarkForTest,
    Reset,
    /// Retry the reset with `force`, after the device answered `Busy`.
    ForceReset,
    /// Emit `DisconnectExpected` and arm the grace timer.
    AwaitDisconnect,
    /// Emit `ReconnectRequired`.
    RequestReconnect,
    /// Arm the apply-poll timer, and the apply timeout if it is not armed yet.
    AwaitApply,
    /// Emit `ConfirmationRequired` and wait for the application (ADR-0014).
    RequestConfirmation,
    /// Confirm `Step::hash` of `Step::image`.
    Confirm,
    /// Terminal: the report is complete, the final state, target hash and last
    /// slot table included; emit `Finished`.
    Finish,
};

/// Something that happened.
struct Event
{
    enum class Kind : std::uint8_t
    {
        Start,
        /// Immediate follow-up to `Effect::Continue`.
        Continue,
        ParametersRead,
        /// The optional parameters command failed. Not fatal (A8).
        ParametersUnavailable,
        /// The device answered the bootloader-information `mode` query.
        BootloaderRead,
        /// It gave no answer: a device error, not a lost link (ADR-0025).
        BootloaderUnavailable,
        StateRead,
        UploadFinished,
        MarkedForTest,
        ResetAccepted,
        Disconnected,
        /// The grace period expired with the link still up.
        GraceExpired,
        Reconnected,
        ReconnectFailed,
        /// The apply-poll timer fired: time to read the state again.
        ApplyPollDue,
        /// The apply-poll timer fired after `UpdatePlan::apply_timeout`.
        ApplyTimedOut,
        /// The application called `confirm()`.
        ConfirmApproved,
        /// The device accepted the confirm.
        Confirmed,
        /// Any command failed. `error` says how.
        Failed,
        Cancel,
    };

    Kind kind{};
    /// `ParametersRead`.
    std::uint32_t buf_size = 0;
    /// `BootloaderRead`.
    BootloaderMode bootloader{};
    /// `StateRead`, and the set-state answers `MarkedForTest` and `Confirmed`:
    /// the device's slot table, which the machine records and decides on.
    /// Borrowed for the duration of the call; never null for those kinds.
    const ImageState* state = nullptr;
    /// `UploadFinished`.
    std::uint64_t transferred = 0;
    /// `UploadFinished`: the server's own already-present check completed the
    /// upload on the first packet (rule 9a), so nothing was really transferred.
    bool already_present = false;
    /// `Failed` and `ReconnectFailed`.
    Error error;
};

/// One image of the update, as the decisions see it.
struct Target
{
    std::uint32_t image = 0;
    CommitBy commit = CommitBy::Client;
    /// The MCUboot hash TLV of the file being installed.
    ImageHash hash;
    /// The file's header version, `ih_ver`.
    ImageVersion version{};

    /// One build of the image, linked for one of its two slots.
    struct Build
    {
        ImageHash hash;
        ImageVersion version;
    };

    /// Direct-XIP's one build per slot, primary then secondary
    /// (`ImageTarget::secondary_source`). The machine takes `hash` and
    /// `version` from the chosen one once the slot table shows which slot is
    /// free.
    std::optional<std::array<Build, 2>> builds = std::nullopt;
};

/// The next state, what to do to get there, and what that needs.
///
/// The parameters are set only for the effects that use them, and default
/// otherwise.
struct Step
{
    UpdateState next{};
    Effect effect = Effect::None;

    /// `StartUpload`: the index of the target in the update, as given to the
    /// `Machine` -- and so the index of its source.
    std::size_t target = 0;
    /// `StartUpload`: which of the target's builds to send, 0 for the primary
    /// slot's and 1 for the secondary's. Always 0 for a target without builds.
    std::size_t build = 0;
    /// `StartUpload`, `MarkForTest` and `Confirm`: the image number.
    std::uint32_t image = 0;
    /// `StartUpload`: the device's buffer size, or zero when it did not say.
    std::uint32_t buf_size = 0;
    /// `MarkForTest` and `Confirm`: the image-state hash to name.
    ///
    /// The `{}` is for GCC: without it, every `Step{next, effect}` trips
    /// `-Wmissing-field-initializers`. clang-tidy calls it redundant.
    // NOLINTNEXTLINE(readability-redundant-member-init)
    ImageHash hash{};
};

/// The machine's working state between events: the targets, the current one,
/// the last slot table, the recovery budgets and the report. Defined with the
/// decisions in the .cpp, so nothing outside the machine can read or set it.
struct Context;

/// The update decisions, with the state they carry between events.
///
/// Constructed from the update's targets and plan; `apply()` takes one event
/// and returns the step to carry out. The current state and the one report
/// are read-only queries. Nothing else is visible: a state is reached only by
/// the events that reach it.
class Machine
{
public:
    /// An update of \p targets, in the order they are staged, under \p plan.
    /// The report has one entry per target from the start. With no target,
    /// every event fails the update as an internal error.
    Machine(const std::vector<Target>& targets, const UpdatePlan& plan);
    ~Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;
    Machine(Machine&&) = delete;
    Machine& operator=(Machine&&) = delete;

    /// Applies \p event in the current state, moves to the step's state and
    /// returns the step. A step whose effect is `Continue` expects a
    /// `Continue` event next.
    [[nodiscard]] Step apply(const Event& event);

    /// `Idle` until the first event.
    [[nodiscard]] UpdateState state() const noexcept;

    /// The report as the machine has written it so far: the outcomes decided,
    /// and, once the state is terminal, the final state, the target hash and
    /// the last slot table.
    [[nodiscard]] const UpdateReport& report() const noexcept;

private:
    UpdatePlan plan_;
    UpdateState state_ = UpdateState::Idle;
    std::unique_ptr<Context> context_;
};

} // namespace smply::dfu

#endif // SMPLY_SRC_DFU_UPDATE_STATE_MACHINE_HPP
