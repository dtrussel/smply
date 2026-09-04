// SPDX-License-Identifier: Apache-2.0

#include "smply/smp/header.hpp"

#include "smply/error.hpp"
#include "smply/result.hpp"

#include <cstddef>
#include <cstdint>

namespace smply {
namespace {

// Byte 0 is packed MSB to LSB as: reserved(3) | version(2) | op(3).
constexpr std::uint8_t kReservedMask = 0xE0;
constexpr std::uint8_t kVersionMask = 0x03;
constexpr unsigned kVersionShift = 3;
constexpr std::uint8_t kOperationMask = 0x07;

constexpr std::uint8_t kMaxOperation = 3; // WriteResponse
constexpr std::uint8_t kMaxVersion = 1;   // V2

/// Big-endian 16-bit read. Explicit shifts rather than a cast over the buffer:
/// the wire order is fixed, so it must not depend on the host's.
[[nodiscard]] constexpr std::uint16_t read_be16(std::byte high, std::byte low) noexcept
{
    return static_cast<std::uint16_t>(
        (static_cast<unsigned>(std::to_integer<std::uint8_t>(high)) << 8U) |
        static_cast<unsigned>(std::to_integer<std::uint8_t>(low)));
}

[[nodiscard]] constexpr std::byte high_byte(std::uint16_t value) noexcept
{
    return static_cast<std::byte>((value >> 8U) & 0xFFU);
}

[[nodiscard]] constexpr std::byte low_byte(std::uint16_t value) noexcept
{
    return static_cast<std::byte>(value & 0xFFU);
}

} // namespace

std::array<std::byte, kHeaderSize> encode(const Header& header) noexcept
{
    // Reserved bits are written as zero regardless of anything the caller did:
    // there is no way to express them in Header, and the specification requires
    // them clear.
    const auto version_bits =
        static_cast<unsigned>(static_cast<std::uint8_t>(header.version) & kVersionMask);
    const auto op_bits =
        static_cast<unsigned>(static_cast<std::uint8_t>(header.op) & kOperationMask);
    const auto byte0 = static_cast<std::byte>((version_bits << kVersionShift) | op_bits);

    const std::uint16_t group = to_underlying(header.group);

    return {
        byte0,
        static_cast<std::byte>(header.flags),
        high_byte(header.length),
        low_byte(header.length),
        high_byte(group),
        low_byte(group),
        static_cast<std::byte>(header.seq),
        static_cast<std::byte>(header.command),
    };
}

Result<Header> decode_header(std::span<const std::byte, kHeaderSize> bytes) noexcept
{
    const auto byte0 = std::to_integer<std::uint8_t>(bytes[0]);

    if ((byte0 & kReservedMask) != 0) {
        return fail(ErrorCode::MalformedMessage, "smp header: reserved bits set");
    }

    const auto version_bits = static_cast<std::uint8_t>((byte0 >> kVersionShift) & kVersionMask);
    if (version_bits > kMaxVersion) {
        // 0b10 and 0b11 are reserved for future use. Refusing them is the
        // honest response: we cannot know how such a device frames its errors.
        return fail(ErrorCode::UnsupportedSmpVersion, "smp header: reserved version");
    }

    const auto op_bits = static_cast<std::uint8_t>(byte0 & kOperationMask);
    if (op_bits > kMaxOperation) {
        return fail(ErrorCode::MalformedMessage, "smp header: unknown operation");
    }

    return Header{
        .op = static_cast<Operation>(op_bits),
        .version = static_cast<Version>(version_bits),
        // Unknown flag bits are carried through, not rejected.
        .flags = std::to_integer<std::uint8_t>(bytes[1]),
        .length = read_be16(bytes[2], bytes[3]),
        // Group is open: any 16-bit value round-trips unchanged.
        .group = static_cast<Group>(read_be16(bytes[4], bytes[5])),
        .seq = std::to_integer<std::uint8_t>(bytes[6]),
        .command = std::to_integer<std::uint8_t>(bytes[7]),
    };
}

Result<Header> decode_header(ConstBytes bytes) noexcept
{
    if (bytes.size() < kHeaderSize) {
        return fail(ErrorCode::MalformedMessage, "smp header: truncated");
    }
    return decode_header(bytes.first<kHeaderSize>());
}

const char* operation_name(Operation op) noexcept
{
    switch (op) {
    case Operation::Read:
        return "read";
    case Operation::ReadResponse:
        return "read-response";
    case Operation::Write:
        return "write";
    case Operation::WriteResponse:
        return "write-response";
    }
    // Unreachable for a decoded header: decode_header rejects anything above 3.
    return "unknown";
}

} // namespace smply
