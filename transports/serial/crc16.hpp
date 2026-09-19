// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TRANSPORTS_SERIAL_CRC16_HPP
#define SMPLY_TRANSPORTS_SERIAL_CRC16_HPP

/// \file
/// The CRC MCUmgr's serial framing puts on every packet.
///
/// Zephyr computes it with `crc16_itu_t(0x0000, data, len)`. That name is
/// doing less work than it looks: `crc.h` (S21) documents the function as
/// polynomial `0x1021`, **MSB-first, with no input or output reflection**, and
/// the seed is a parameter rather than part of the definition. With seed `0`
/// and no final XOR that is the variant catalogued as **CRC-16/XMODEM**, whose
/// published check value over `"123456789"` is `0x31C3` -- which is what the
/// unit suite asserts, because a CRC checked only against this file's own
/// output would agree with itself no matter which variant it had implemented.
///
/// Two properties the framing depends on, both from `serial_util.c` (S19):
///
/// * the sender computes it over the **SMP packet alone** -- not over the
///   two-byte length prefix that precedes the packet in the encoded body;
/// * the receiver does not recompute-and-compare. It runs the CRC over the
///   accumulated bytes *including* the two CRC bytes at the end and requires
///   **zero**. For a non-reflected CRC with no final XOR, appending the value
///   big-endian is exactly what makes that identity hold, so the two
///   formulations are the same statement and `crc16_xmodem(0, packet_and_crc)`
///   is a legitimate way to verify.
///
/// Bitwise rather than table-driven on purpose: a 512-byte table would be the
/// largest object in a header-only transport module, and a serial link moves
/// at most a few KiB per second. If that ever stops being true, the table
/// belongs behind the same signature.

#include "smply/bytes.hpp"

#include <cstdint>

namespace smply::transport {

/// CRC-16/XMODEM over \p data, continuing from \p seed.
///
/// Pass `0` for a whole buffer. A non-zero seed continues a computation across
/// non-contiguous blocks, which is the same rule Zephyr's own header states.
[[nodiscard]] constexpr std::uint16_t crc16_xmodem(std::uint16_t seed, ConstBytes data) noexcept
{
    std::uint16_t crc = seed;
    for (const std::byte value : data) {
        crc ^= static_cast<std::uint16_t>(static_cast<std::uint16_t>(value) << 8U);
        for (int bit = 0; bit < 8; ++bit) {
            const bool carry = (crc & 0x8000U) != 0U;
            crc = static_cast<std::uint16_t>(crc << 1U);
            if (carry) {
                crc ^= 0x1021U;
            }
        }
    }
    return crc;
}

} // namespace smply::transport

#endif // SMPLY_TRANSPORTS_SERIAL_CRC16_HPP
