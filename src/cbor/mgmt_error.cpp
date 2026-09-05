// SPDX-License-Identifier: Apache-2.0

#include "cbor/mgmt_error.hpp"

#include "cbor/cbor.hpp"
#include "smply/group.hpp"
#include "smply/limits.hpp"

#include <cstdint>

namespace smply::cbor {
namespace {

/// Reads `err: {group, rc}` if present. Returns nullopt when there is no `err`
/// map, which is the ordinary case for an SMP v1 device.
[[nodiscard]] std::optional<MgmtError> read_group_scoped(Reader& reader)
{
    if (!reader.enter_map("err").has_value()) {
        return std::nullopt;
    }

    const auto group = reader.uint("group");
    const auto rc = reader.uint("rc");
    static_cast<void>(reader.leave_map());

    if (!rc.has_value()) {
        // An `err` map without an `rc` says nothing; treat it as no error
        // rather than inventing a code.
        return std::nullopt;
    }
    if (*rc == 0) {
        return std::nullopt; // zero means success in either shape
    }

    // Values wider than the field are truncated rather than rejected: the
    // enumerations grow with each Zephyr release, and a code we cannot name is
    // still worth reporting numerically (protocol-notes section 9, A2).
    const auto group_id = static_cast<Group>(static_cast<std::uint16_t>(group.value_or(0)));
    return MgmtError::scoped(group_id, static_cast<std::uint16_t>(*rc));
}

} // namespace

Result<MgmtOutcome> extract_mgmt_error(ConstBytes payload)
{
    Reader reader{payload};
    if (auto entered = reader.enter_map(); !entered.has_value()) {
        return fail(entered.error());
    }

    MgmtOutcome outcome;

    // The group-scoped shape is more specific, so it wins if both appear.
    // In practice they do not co-occur, but preferring the one that names its
    // group loses less information than the reverse.
    outcome.error = read_group_scoped(reader);

    if (!outcome.error.has_value()) {
        // `rc` is signed in the v1 shape. Zephyr sends non-negative values, but
        // decoding it as signed avoids rejecting a device that does otherwise.
        if (const auto rc = reader.integer("rc"); rc.has_value() && *rc != 0) {
            outcome.error = MgmtError::smp(static_cast<std::uint16_t>(*rc));
        }
    }

    if (const auto reason = reader.text("rsn"); reason.has_value()) {
        // Capped: this is device-supplied text and must not be able to grow the
        // host's memory or a log line without bound.
        const auto capped = reason->substr(0, limits::kMaxReasonLength);
        outcome.reason.assign(capped);
    }

    if (auto status = reader.status(); !status.has_value()) {
        return fail(status.error());
    }
    return outcome;
}

} // namespace smply::cbor
