// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_GROUPS_OS_HPP
#define SMPLY_GROUPS_OS_HPP

/// \file
/// The OS management group, group 0 (docs/protocol-notes.md section 5).
///
/// Five commands, and nothing else: reset, MCUmgr parameters, echo, and the
/// bootloader's name and mode from bootloader information. This is a
/// thin encoder/decoder over `SmpClient` -- it allocates no sequence numbers,
/// sets no deadlines and interprets no `rc`, because the client below it
/// already does all three. If something here looks like it needs to know about
/// correlation, the seam is in the wrong place.
///
/// **Threading and lifetime.** As everywhere: calls and callbacks happen on the
/// client context, a callback never runs inside the call that started the
/// operation, and whatever a callback captures must outlive the `SmpClient`
/// (see `smply/smp_client.hpp`). An `OsManagement` is a handle onto a client,
/// so it must not outlive it either.

#include "smply/clock.hpp"
#include "smply/limits.hpp"
#include "smply/result.hpp"
#include "smply/smp_client.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace smply {

/// The server's SMP buffer budget, from the MCUmgr parameters command.
struct McumgrParameters
{
    /// Size of one SMP buffer, **including the 8-byte header**, not just the
    /// CBOR payload. This is the authoritative input to upload chunk sizing
    /// (docs/protocol-notes.md section 8); conflating it with the transport's
    /// MTU is the classic mistake.
    std::uint32_t buf_size = 0;
    /// How many such buffers the server has.
    std::uint32_t buf_count = 0;

    [[nodiscard]] friend constexpr bool operator==(const McumgrParameters&,
                                                   const McumgrParameters&) noexcept = default;
};

/// MCUboot's mode of operation, numbered as MCUboot's `enum mcuboot_mode`
/// numbers it (docs/protocol-notes.md section 7, S56).
///
/// The number is what the device sends. Zephyr's specification lists only 0 to
/// 6, and calls 3 by an older name; the enum is the authority (A37).
enum class McubootMode : std::int8_t
{
    /// The device did not say, said `-1`, or sent a number outside MCUboot's
    /// enum. Zephyr reports `-1` for its RAM-load builds too (A38).
    Unknown = -1,
    SingleSlot = 0,
    SwapUsingScratch = 1,
    /// Overwrite: an update is copied into place permanently, with no trial.
    UpgradeOnly = 2,
    SwapUsingMove = 3,
    /// Runs from either slot; no set-state, and no revert.
    DirectXip = 4,
    DirectXipWithRevert = 5,
    RamLoad = 6,
    FirmwareLoader = 7,
    SingleSlotRamLoad = 8,
    SwapUsingOffset = 9,
};

/// A short, stable name for a mode, such as `"swap-using-move"`. Never
/// allocates.
[[nodiscard]] std::string_view to_string(McubootMode mode) noexcept;

/// The answer to the bootloader-information `mode` query.
struct BootloaderMode
{
    /// `Unknown` for a number outside MCUboot's enum.
    McubootMode mode = McubootMode::Unknown;
    /// The number exactly as the device sent it, so that a mode newer than
    /// smply is still visible (A2).
    std::int64_t raw_mode = -1;
    /// MCUboot refuses an image with a lower version than the running one
    /// (`CONFIG_MCUBOOT_BOOTLOADER_NO_DOWNGRADE`, protocol-notes section 7).
    /// The device sends the flag only when it is true.
    bool no_downgrade = false;

    [[nodiscard]] friend constexpr bool operator==(const BootloaderMode&,
                                                   const BootloaderMode&) noexcept = default;
};

/// What to ask for when resetting.
struct ResetOptions
{
    /// Ask the device to reset even if a registered reset hook would otherwise
    /// refuse. The documented use is retrying after `SmpError::Busy`.
    ///
    /// Sent as a CBOR boolean, and omitted entirely when false. The
    /// specification says this field is an integer; the server decodes a
    /// boolean and *silently ignores* anything else, so a boolean is what a
    /// device actually acts on (docs/protocol-notes.md section 9, A15).
    bool force = false;

    /// Overrides the client's default deadline for this request.
    std::optional<Duration> timeout;
};

/// Reset, MCUmgr parameters, echo and bootloader information.
///
/// Holds a reference to the client and no state of its own, so several may
/// exist over one client and any may be destroyed at any time. Destroying it
/// does not cancel requests it issued -- the returned `RequestHandle` does
/// that.
class OsManagement
{
public:
    explicit OsManagement(SmpClient& client) noexcept;

    /// Asks the device to restart.
    ///
    /// **The response means the request was accepted, not that the device has
    /// restarted.** Zephyr answers first and reboots afterwards, by design, so
    /// that the client learns the command was taken; the delay between the two
    /// is implementation-defined (docs/protocol-notes.md section 5). Treat the
    /// callback as the start of the reboot, and wait for a transport disconnect
    /// to learn it happened. Losing the response entirely is also normal --
    /// the device may reset before it goes out.
    ///
    /// A reset hook on the device may refuse, which arrives as
    /// `ErrorCode::ProtocolError`. `SmpError::Busy` specifically invites a
    /// retry with `force` set.
    RequestHandle reset(const ResetOptions& options, Callback<void> on_done);

    /// \overload Resets with default options.
    RequestHandle reset(Callback<void> on_done);

    /// Reads the server's buffer budget.
    ///
    /// **Optional command.** A minimal server answers `SmpError::NotSupported`,
    /// which is an ordinary `ProtocolError` here and not a reason to give up:
    /// the caller falls back to `limits::kDefaultSmpMessageBudget`. Use
    /// `smp_error()` to recognise it.
    RequestHandle mcumgr_parameters(Callback<McumgrParameters> on_done);

    /// Sends a string and expects it back verbatim.
    ///
    /// The cheapest end-to-end check that a link, the framing and the server
    /// all work. \p text longer than `limits::kMaxEchoLength` is rejected with
    /// `ErrorCode::InvalidArgument` rather than truncated, and a reply longer
    /// than that is rejected as `ErrorCode::CborDecode` -- a device echoing
    /// back more than it was sent is not answering the question that was asked.
    ///
    /// \p text is borrowed for the duration of this call only.
    RequestHandle echo(std::string_view text, Callback<std::string> on_done);

    /// Reads the bootloader's name: bootloader information with no query.
    /// MCUboot answers `"MCUboot"`.
    ///
    /// **Optional command** (`CONFIG_MCUMGR_GRP_OS_BOOTLOADER_INFO`): a device
    /// without it answers `SmpError::NotSupported`, as for
    /// `mcumgr_parameters()`. A name longer than
    /// `limits::kMaxBootloaderNameLength` is rejected as
    /// `ErrorCode::CborDecode` before it is copied.
    RequestHandle bootloader_name(Callback<std::string> on_done);

    /// Reads MCUboot's mode and downgrade prevention: bootloader information
    /// with the query `"mode"` (docs/protocol-notes.md section 5).
    ///
    /// The same optional command as `bootloader_name()`. A bootloader that is
    /// not MCUboot has no answer, which arrives as an OS-group error
    /// (`QUERY_YIELDS_NO_ANSWER`, 3) over SMP v2. Every device error is an
    /// ordinary `ErrorCode::ProtocolError`; what to do without an answer is
    /// the caller's decision.
    RequestHandle bootloader_mode(Callback<BootloaderMode> on_done);

private:
    SmpClient* client_;
};

} // namespace smply

#endif // SMPLY_GROUPS_OS_HPP
