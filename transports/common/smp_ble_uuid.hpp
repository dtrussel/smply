// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TRANSPORTS_COMMON_SMP_BLE_UUID_HPP
#define SMPLY_TRANSPORTS_COMMON_SMP_BLE_UUID_HPP

/// \file
/// The two UUIDs that identify SMP over Bluetooth LE, as bytes.
///
/// **This header exists so that a mistyped UUID is a test failure rather than a
/// device that never answers.** The WinRT adapter (P15b) cannot be compiled, let
/// alone run, on the machine most of this library is developed on, so anything
/// it can be made to rest on that *is* portable is worth putting here: a 128-bit
/// UUID is sixteen bytes, and sixteen bytes are the same on every platform.
///
/// From docs/protocol-notes.md section 8, quoting the MCUmgr transport
/// specification:
///
/// * **Service** `8D53DC1D-1DB7-4CD3-868B-8A527460AA84`
/// * **Characteristic** `DA2E7828-FBCE-4E01-AE9E-261174997C48`
///
/// Both are stored in the canonical RFC 4122 order -- the order the digits are
/// written in, most significant byte first -- because that is the form a reader
/// can check against the specification by eye. Platform GUID structures are not
/// in that order; see `uuid_fields()` below, which is the one place that
/// difference is dealt with.

#include <array>
#include <cstdint>
#include <string_view>

namespace smply::transport {

/// A 128-bit UUID in canonical byte order (RFC 4122 network order).
using Uuid128 = std::array<std::uint8_t, 16>;

/// The SMP service (protocol-notes section 8).
inline constexpr Uuid128 kSmpServiceUuid = {0x8D, 0x53, 0xDC, 0x1D, 0x1D, 0xB7, 0x4C, 0xD3,
                                            0x86, 0x8B, 0x8A, 0x52, 0x74, 0x60, 0xAA, 0x84};

/// The SMP characteristic: write-without-response out, notification in.
inline constexpr Uuid128 kSmpCharacteristicUuid = {0xDA, 0x2E, 0x78, 0x28, 0xFB, 0xCE, 0x4E, 0x01,
                                                   0xAE, 0x9E, 0x26, 0x11, 0x74, 0x99, 0x7C, 0x48};

/// The same two values as text, exactly as the specification writes them.
///
/// Not decoration: the test suite parses these and asserts the result equals the
/// byte arrays above, so the two spellings check each other and neither can be
/// mistyped without something failing.
/// @{
inline constexpr std::string_view kSmpServiceUuidString = "8D53DC1D-1DB7-4CD3-868B-8A527460AA84";
inline constexpr std::string_view kSmpCharacteristicUuidString =
    "DA2E7828-FBCE-4E01-AE9E-261174997C48";

/// @}

/// A UUID split the way platform GUID structures hold one.
///
/// `GUID`, `winrt::guid`, `uuid_t` and friends all store the first three fields
/// as **integers**, which on a little-endian host means their bytes are stored
/// in the opposite order to the way the UUID is written. The last eight bytes
/// are a plain array and are *not* reordered. That asymmetry is the classic
/// UUID bug, and it is why this conversion exists once, here, rather than at
/// each call site.
struct Uuid128Fields
{
    std::uint32_t data1;               ///< Bytes 0-3, as an integer.
    std::uint16_t data2;               ///< Bytes 4-5, as an integer.
    std::uint16_t data3;               ///< Bytes 6-7, as an integer.
    std::array<std::uint8_t, 8> data4; ///< Bytes 8-15, in order, unchanged.
};

/// Splits a canonical UUID into the fields a platform GUID structure wants.
///
/// The shifts are explicit, so the result does not depend on the host's byte
/// order -- the same discipline `src/smp/codec.cpp` uses for the SMP header.
[[nodiscard]] constexpr Uuid128Fields uuid_fields(const Uuid128& uuid) noexcept
{
    return Uuid128Fields{
        // No outer cast on this one: the operands are already `std::uint32_t`,
        // so adding one is what GCC's -Wuseless-cast rejects. The two below do
        // need theirs -- a `std::uint16_t` promotes to `int` before the shift.
        .data1 = static_cast<std::uint32_t>(uuid[0]) << 24U |
                 static_cast<std::uint32_t>(uuid[1]) << 16U |
                 static_cast<std::uint32_t>(uuid[2]) << 8U | static_cast<std::uint32_t>(uuid[3]),
        .data2 = static_cast<std::uint16_t>(static_cast<std::uint16_t>(uuid[4]) << 8U |
                                            static_cast<std::uint16_t>(uuid[5])),
        .data3 = static_cast<std::uint16_t>(static_cast<std::uint16_t>(uuid[6]) << 8U |
                                            static_cast<std::uint16_t>(uuid[7])),
        .data4 = {uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]},
    };
}

} // namespace smply::transport

#endif // SMPLY_TRANSPORTS_COMMON_SMP_BLE_UUID_HPP
