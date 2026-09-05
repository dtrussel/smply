// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SRC_CBOR_MGMT_ERROR_HPP
#define SMPLY_SRC_CBOR_MGMT_ERROR_HPP

/// \file
/// Extracts the device's error report from a response payload.
///
/// Applied to every response before any group-specific parsing, because the
/// two SMP versions report failures in incompatible shapes and a client must
/// understand both (docs/protocol-notes.md section 3).

#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/result.hpp"

#include <optional>
#include <string>

namespace smply::cbor {

/// What a response says about whether it succeeded.
struct MgmtOutcome
{
    /// Absent when the command succeeded.
    std::optional<MgmtError> error;
    /// The device's optional `rsn` text. Attacker-controlled and length-capped
    /// (docs/security.md, T12).
    std::string reason;

    [[nodiscard]] bool failed() const noexcept
    {
        return error.has_value();
    }
};

/// Reads the error report out of a response payload.
///
/// Handles all four shapes a device may send:
///
/// * `{}` or a map with neither key -- success, which is how MCUmgr reports it;
/// * `{"rc": 0}` -- also success, since `rc` is only meaningful when non-zero;
/// * `{"rc": N}` -- an SMP-level or SMP v1 error, drawn from `mcumgr_err_t`;
/// * `{"err": {"group": G, "rc": R}}` -- an SMP v2 group-scoped error.
///
/// The fourth shape can arrive even from a client that asked for v1, and the
/// third can arrive from a v2 device: the specification requires v2 clients to
/// handle SMP-level errors reported as a flat `rc`. So both are always decoded,
/// whatever version was requested.
///
/// \return The outcome, or a decode error if the payload is not a CBOR map.
///         A well-formed map that simply reports no error yields a successful
///         `MgmtOutcome` with no `error`.
[[nodiscard]] Result<MgmtOutcome> extract_mgmt_error(ConstBytes payload);

} // namespace smply::cbor

#endif // SMPLY_SRC_CBOR_MGMT_ERROR_HPP
