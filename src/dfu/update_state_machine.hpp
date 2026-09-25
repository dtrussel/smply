// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SRC_DFU_UPDATE_STATE_MACHINE_HPP
#define SMPLY_SRC_DFU_UPDATE_STATE_MACHINE_HPP

/// \file
/// The update decision logic, as one pure function.
///
/// No client, no transport, no clock, no `ImageSource`: `advance()` says what
/// should happen next and `FirmwareUpdater` does it. That is what makes every
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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace smply::dfu {

/// What `FirmwareUpdater` must do to carry out a step.
enum class Effect : std::uint8_t
{
    /// Nothing to do; the machine waits for an external event.
    None,
    /// Feed the machine a `Continue` event immediately. Used where a decision
    /// needs no I/O, so that the state it is decided in is still a real state
    /// with a real transition rather than something invisible.
    Continue,
    QueryParameters,
    ReadState,
    StartUpload,
    ResumeUpload,
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
    Confirm,
    /// Terminal: the report is complete, emit `Finished`.
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
    /// `StateRead`. Borrowed for the duration of the call.
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
    /// `Client` only: booted and not yet confirmed, so a confirm is owed.
    bool in_trial = false;
};

/// Everything the decisions need, carried between them.
///
/// Deliberately small: anything the machine does not branch on belongs in
/// `FirmwareUpdater`, not here.
struct Context
{
    /// The images, in the order they are staged. Never empty; build it with
    /// `make_context()`, which sizes `report.images` to match.
    std::vector<Target> targets;
    /// The target being uploaded, marked or confirmed. Equal to
    /// `targets.size()` once every image is staged.
    std::size_t current = 0;
    /// The most recent slot table.
    std::optional<ImageState> device;
    /// From the device, or zero when it does not implement the command.
    std::uint32_t buf_size = 0;

    /// What the update will report. The machine writes the outcome fields
    /// here as it decides them -- per image in `images`, and `revert_pending`
    /// and `cause` for the whole -- and derives the summary fields from the
    /// images after every step, so the updater hands the report out rather
    /// than copying it field by field. `FirmwareUpdater` adds the final
    /// state, the target hash and the last slot table when it finishes.
    UpdateReport report;

    /// An upload was started and has not finished, so a reconnect resumes it
    /// rather than moving on.
    bool upload_in_progress = false;

    /// A swap is scheduled and not yet confirmed, so an update that ends now
    /// leaves the device about to revert.
    bool swap_scheduled = false;
    /// The one mark-for-test recovery has been spent for the current target --
    /// for either shape of refusal, `ImageAlreadyPending` or a group-less
    /// `BadState`.
    bool mark_retried = false;
    /// The one `Busy` reset retry has been spent.
    bool reset_forced = false;
};

/// The next state, and what to do to get there.
struct Step
{
    UpdateState next{};
    Effect effect = Effect::None;
};

/// A context for \p targets, with one report entry per target.
[[nodiscard]] Context make_context(std::vector<Target> targets);

/// Applies one event.
///
/// \param state The current state.
/// \param event What happened.
/// \param plan  The caller's plan; read, never modified.
/// \param context Mutated to record what the outcome must report.
[[nodiscard]] Step advance(UpdateState state, const Event& event, const UpdatePlan& plan,
                           Context& context);

} // namespace smply::dfu

#endif // SMPLY_SRC_DFU_UPDATE_STATE_MACHINE_HPP
