// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_GROUP_HPP
#define SMPLY_GROUP_HPP

/// \file
/// MCUmgr management group identifiers.
///
/// This is core vocabulary rather than an SMP-header detail: the error model
/// needs it (an SMP v2 error code is only meaningful against the group it came
/// from, docs/protocol-notes.md section 3), and so do the group clients and the
/// request router. `smply/smp/header.hpp` includes this header.

#include <cstdint>

namespace smply {

/// A management group identifier, as carried in the SMP header's 16-bit
/// big-endian group field.
///
/// The enumeration is deliberately open: values other than those named below
/// are legal on the wire and round-trip unchanged, so an unknown group from a
/// newer or vendor-extended device is never rejected or lost. smply itself
/// implements only Os and Image (docs/architecture.md section 11).
enum class Group : std::uint16_t
{
    Os = 0,           ///< OS management: reset, echo, MCUmgr parameters.
    Image = 1,        ///< Image management: state, upload, erase, slot info.
    Stat = 2,         ///< Statistics.
    Settings = 3,     ///< Settings / configuration.
    Log = 4,          ///< Log management (unused by Zephyr).
    Crash = 5,        ///< Crash (unused by Zephyr).
    Split = 6,        ///< Split image management (unused by Zephyr).
    Run = 7,          ///< Run (unused by Zephyr).
    Fs = 8,           ///< File system.
    Shell = 9,        ///< Shell command execution.
    Enumeration = 10, ///< Enumeration of supported groups.
    Transport = 11,   ///< Transport bridging.
    ZephyrBasic = 63, ///< Zephyr-specific basic group.
    PerUser = 64,     ///< Base of the user-defined range; payloads may not be CBOR.
};

/// The numeric value of a group, for encoding and for diagnostics.
[[nodiscard]] constexpr std::uint16_t to_underlying(Group group) noexcept
{
    return static_cast<std::uint16_t>(group);
}

/// True when `group` is in the user-defined range (64 and above), whose payload
/// encoding is not required to be CBOR (docs/protocol-notes.md section 2).
[[nodiscard]] constexpr bool is_user_defined(Group group) noexcept
{
    return to_underlying(group) >= to_underlying(Group::PerUser);
}

/// A short, stable name for a group, for logs and test failure messages.
///
/// Returns "unknown" for values smply does not name; the numeric value is
/// preserved separately and is what should be reported to a user.
[[nodiscard]] const char* group_name(Group group) noexcept;

} // namespace smply

#endif // SMPLY_GROUP_HPP
