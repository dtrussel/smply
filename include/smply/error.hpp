// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_ERROR_HPP
#define SMPLY_ERROR_HPP

/// \file
/// smply's structured error type (ADR-0002).
///
/// Errors are values, not strings and not bare integers. `ErrorCode` is the
/// machine-readable category callers switch on; `MgmtError` preserves the
/// device's own numbers together with the group they must be interpreted
/// against; `reason` and `where` exist only for diagnostics.

#include "smply/group.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace smply {

/// The machine-readable error category. This is what control flow switches on.
///
/// The underlying type is fixed at 16 bits for headroom and to match the other
/// protocol enumerations; the values themselves are smply's own and never go on
/// the wire.
enum class ErrorCode : std::uint16_t
{
    Ok = 0,                ///< No error. The default-constructed state.
    InvalidArgument,       ///< Caller misuse: bad size, null source, bad argument.
    InvalidState,          ///< The operation is not legal in the current state.
    MalformedMessage,      ///< SMP header or framing violated.
    UnsupportedSmpVersion, ///< SMP version bits 0b10 or 0b11 (reserved).
    MessageTooLarge,       ///< Exceeds a configured limit (smply/limits.hpp).
    CborEncode,            ///< Failed to encode a request payload.
    CborDecode,            ///< Malformed or unexpected CBOR in a response.
    ProtocolError,         ///< The device reported an error; see Error::mgmt().
    UnexpectedResponse,    ///< Sequence, group or command did not match.
    Timeout,               ///< No response within the deadline.
    Cancelled,             ///< Cancelled by the caller, or by destruction.
    TransportError,        ///< The transport failed, but the link is still up.
    TransportBusy,         ///< Retry once the transport drains.
    Disconnected,          ///< The link is gone.
    ImageMismatch,         ///< Device content does not match what was uploaded.
    UpdateFailed,          ///< Terminal failure of the DFU state machine.
    Internal,              ///< A bug in smply.
};

/// A short, stable name for an error code. Never allocates.
///
/// This is the zero-allocation path for logging; `to_string(const Error&)` adds
/// the device detail and may allocate.
[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;

/// An error reported by the device, in either SMP version's shape.
///
/// SMP v1 returns a flat `rc` drawn from `mcumgr_err_t`. SMP v2 returns
/// `err: {group, rc}`, where `rc` is drawn from *that group's* own enumeration
/// -- so an image-group `rc` of 30 means something entirely different from an
/// OS-group `rc` of 30, and neither is comparable with a v1 `rc` of 30. Keeping
/// the group and the flag alongside the number is what makes the value
/// interpretable at all (docs/protocol-notes.md section 3).
///
/// Unknown `rc` values are preserved numerically rather than rejected: these
/// enumerations grow with each Zephyr release (protocol-notes section 9, A2).
struct MgmtError
{
    /// The group the code belongs to. Only meaningful when `group_scoped`.
    Group group{Group::Os};
    /// The raw code: group-scoped when `group_scoped`, else `mcumgr_err_t`.
    std::uint16_t rc{};
    /// True when `rc` must be read against `group` (SMP v2 `err` map).
    bool group_scoped{};

    /// A flat SMP v1 `rc`, drawn from `mcumgr_err_t`.
    [[nodiscard]] static constexpr MgmtError smp(std::uint16_t rc) noexcept
    {
        return MgmtError{Group::Os, rc, false};
    }

    /// A group-scoped SMP v2 error, drawn from `group`'s own enumeration.
    [[nodiscard]] static constexpr MgmtError scoped(Group group, std::uint16_t rc) noexcept
    {
        return MgmtError{group, rc, true};
    }

    [[nodiscard]] friend constexpr bool operator==(const MgmtError&,
                                                   const MgmtError&) noexcept = default;
};

/// The SMP-level error codes, `mcumgr_err_t` (docs/protocol-notes.md section 3,
/// S5).
///
/// These are the codes carried by a flat `rc` -- SMP v1 always, and SMP v2 for
/// failures raised below the group handler. They are **not** comparable with a
/// group-scoped `rc`, which is why `MgmtError::group_scoped` must be false
/// before a value is read as one of these. `smp_error()` does that check.
///
/// Values a release adds later are preserved numerically rather than rejected
/// (section 9, A2), so a code outside this list is normal and must not be
/// treated as malformed.
enum class SmpError : std::uint16_t
{
    Ok = 0,
    Unknown = 1,
    NoMemory = 2,        ///< Typically no room for the CBOR response.
    InvalidArgument = 3, ///< `MGMT_ERR_EINVAL`.
    TimedOut = 4,
    NoEntry = 5,
    BadState = 6, ///< The current state disallows the command.
    ResponseTooLarge = 7,
    NotSupported = 8, ///< The command is not built into this firmware.
    Corrupt = 9,
    Busy = 10, ///< Blocked by another command; a reset may be retried with `force`.
    AccessDenied = 11,
    VersionTooOld = 12,
    VersionTooNew = 13,
    BridgeUnavailable = 14,
    PerUser = 256, ///< First code in the user-defined range.
};

/// A structured error: a category, optionally the device's own report, and
/// diagnostic context.
///
/// Comparison covers `code`, `mgmt` and `reason`. It deliberately excludes
/// `where`, which is a logging aid rather than part of the error's identity --
/// two failures of the same kind raised at different call sites are equal.
class Error
{
public:
    /// A default-constructed Error is `ErrorCode::Ok` with no detail.
    Error() noexcept = default;

    /// \param code  The error category.
    /// \param where A static, literal call-site tag for logs, or nullptr. Must
    ///              have static storage duration; it is stored by pointer.
    explicit Error(ErrorCode code, const char* where = nullptr) noexcept
        : code_{code}, where_{where}
    {}

    /// An error carrying the device's own report. The code is normally
    /// `ErrorCode::ProtocolError`.
    Error(ErrorCode code, MgmtError mgmt, const char* where = nullptr) noexcept
        : code_{code}, mgmt_{mgmt}, where_{where}
    {}

    [[nodiscard]] ErrorCode code() const noexcept
    {
        return code_;
    }

    /// The device's own error report, when there was one.
    [[nodiscard]] const std::optional<MgmtError>& mgmt() const noexcept
    {
        return mgmt_;
    }

    /// The device-supplied `rsn` string. Empty when absent.
    ///
    /// This is attacker-controlled text: it must be length-capped and escaped
    /// before being logged or displayed (docs/security.md, T12).
    [[nodiscard]] std::string_view reason() const noexcept
    {
        return reason_;
    }

    /// The static call-site tag, or nullptr. For logs only; never compared.
    [[nodiscard]] const char* where() const noexcept
    {
        return where_;
    }

    /// Attaches the device's `rsn` text. Chainable.
    Error& with_reason(std::string reason) &
    {
        reason_ = std::move(reason);
        return *this;
    }

    /// \overload
    Error&& with_reason(std::string reason) &&
    {
        reason_ = std::move(reason);
        return std::move(*this);
    }

    /// True when this represents an actual failure.
    [[nodiscard]] bool failed() const noexcept
    {
        return code_ != ErrorCode::Ok;
    }

    [[nodiscard]] friend bool operator==(const Error& lhs, const Error& rhs) noexcept
    {
        // `where_` is excluded deliberately: see the class comment.
        return lhs.code_ == rhs.code_ && lhs.mgmt_ == rhs.mgmt_ && lhs.reason_ == rhs.reason_;
    }

private:
    ErrorCode code_{ErrorCode::Ok};
    std::optional<MgmtError> mgmt_;
    std::string reason_;
    const char* where_{nullptr};
};

/// The SMP-level code an error carries, if it carries one.
///
/// Returns `std::nullopt` unless the device reported a **flat** `rc`: a
/// group-scoped code means something entirely different and must not be read as
/// an `SmpError`. This is the safe way to ask "was that ENOTSUP?" without
/// hand-checking `group_scoped` at every call site.
///
/// \code
///     if (smp_error(error) == SmpError::NotSupported) {
///         // Optional command; fall back rather than fail.
///     }
/// \endcode
[[nodiscard]] inline std::optional<SmpError> smp_error(const Error& error) noexcept
{
    // Bound once rather than calling mgmt() three times: the engaged state is
    // then visible -- to a reader and to static analysis -- where rc is read.
    const std::optional<MgmtError>& mgmt = error.mgmt();
    if (!mgmt.has_value() || mgmt->group_scoped) {
        return std::nullopt;
    }
    return static_cast<SmpError>(mgmt->rc);
}

/// A human-readable rendering, for diagnostics only.
///
/// Never parse this, and never branch on it: `code()` and `mgmt()` are the
/// machine-readable surface. Prefer `to_string(ErrorCode)` when only the
/// category is needed -- that overload does not allocate.
[[nodiscard]] std::string to_string(const Error& error);

} // namespace smply

#endif // SMPLY_ERROR_HPP
