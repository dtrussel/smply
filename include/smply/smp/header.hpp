// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SMP_HEADER_HPP
#define SMPLY_SMP_HEADER_HPP

/// \file
/// The 8-byte SMP header: types and codec.
///
/// This is the only place in smply that manipulates header bytes
/// (docs/architecture.md section 3). Layout is fixed by
/// docs/protocol-notes.md section 2:
///
/// \verbatim
///  byte 0   byte 1   byte 2   byte 3   byte 4   byte 5   byte 6   byte 7
/// +--------+--------+--------+--------+--------+--------+--------+--------+
/// |RRRVVOOO| flags  |     data length |      group id   |  seq   |  cmd   |
/// +--------+--------+--------+--------+--------+--------+--------+--------+
/// \endverbatim
///
/// Byte 0, MSB to LSB: three reserved bits that must be zero, two version bits,
/// three operation bits. Every multi-byte field is big-endian.
///
/// Zephyr's MCUmgr is hardcoded to treat the wire as big-endian despite the
/// original specification allowing either, so smply encodes and decodes
/// big-endian only and never attempts to detect the byte order.

#include "smply/bytes.hpp"
#include "smply/group.hpp"
#include "smply/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace smply {

/// The SMP operation, occupying the low three bits of byte 0.
enum class Operation : std::uint8_t
{
    Read = 0,          ///< Request that reads state.
    ReadResponse = 1,  ///< Response to a Read.
    Write = 2,         ///< Request that changes state.
    WriteResponse = 3, ///< Response to a Write.
};

/// The SMP protocol version, occupying bits 4..3 of byte 0.
///
/// The versions differ only in how errors are reported: V1 returns a flat `rc`,
/// V2 returns a group-scoped `err` map (docs/protocol-notes.md section 3).
/// There is no negotiation mechanism, so smply sends V1 by default and decodes
/// both shapes regardless (ADR-0010). Values 0b10 and 0b11 are reserved and are
/// rejected on decode.
enum class Version : std::uint8_t
{
    V1 = 0, ///< Legacy: flat `rc` errors.
    V2 = 1, ///< Group-scoped `err` errors.
};

/// The fixed size of an SMP header, in bytes.
inline constexpr std::size_t kHeaderSize = 8;

/// Largest payload the 16-bit length field can describe.
inline constexpr std::uint32_t kMaxEncodableLength = 0xFFFFU;

/// A decoded SMP header.
///
/// `length` is the payload size *excluding* this header, so a complete message
/// occupies `total_size()` bytes. That relationship is what lets a byte stream
/// be split back into messages without any transport framing, which is why
/// reassembly belongs to the core (ADR-0006).
struct Header
{
    Operation op{};
    Version version{Version::V1};
    /// No flags are defined by the specification. Unknown bits are preserved
    /// rather than rejected: a future device that sets one must not be made
    /// unreachable by this client.
    std::uint8_t flags{};
    std::uint16_t length{};
    Group group{};
    /// Wraps at 8 bits. A response echoes the request's value.
    std::uint8_t seq{};
    std::uint8_t command{};

    /// Bytes occupied by the whole message: this header plus its payload.
    [[nodiscard]] constexpr std::size_t total_size() const noexcept
    {
        return kHeaderSize + static_cast<std::size_t>(length);
    }

    [[nodiscard]] friend constexpr bool operator==(const Header&, const Header&) noexcept = default;
};

/// Serialises a header. Total function: every Header has a valid encoding.
///
/// The reserved bits are always written as zero, whatever the caller did.
[[nodiscard]] std::array<std::byte, kHeaderSize> encode(const Header& header) noexcept;

/// Parses a header from exactly \p bytes.
///
/// Rejects a header whose reserved bits are set (`MalformedMessage`), whose
/// version is one of the two reserved encodings (`UnsupportedSmpVersion`), or
/// whose operation is outside 0..3 (`MalformedMessage`).
///
/// It deliberately does **not** validate `length`. Bounds belong to the
/// reassembler, which is the only component that knows the configured limit
/// (docs/design.md section 2).
[[nodiscard]] Result<Header> decode_header(std::span<const std::byte, kHeaderSize> bytes) noexcept;

/// \overload Accepts a dynamic span, rejecting one that is too short.
///
/// Convenience for callers holding a buffer whose size is only known at run
/// time; a span shorter than kHeaderSize yields `MalformedMessage`.
[[nodiscard]] Result<Header> decode_header(ConstBytes bytes) noexcept;

/// True when \p op is a response rather than a request.
[[nodiscard]] constexpr bool is_response(Operation op) noexcept
{
    return op == Operation::ReadResponse || op == Operation::WriteResponse;
}

/// The response operation a device must use when answering \p request.
///
/// Correlation needs this: a response is only the answer to a request if its
/// operation is the matching response form, and accepting any operation would
/// let a `Read` be answered by a `WriteResponse`
/// (ADR-0010). Passing an operation that is already a response returns it
/// unchanged, since there is nothing to map.
[[nodiscard]] constexpr Operation response_to(Operation request) noexcept
{
    switch (request) {
    case Operation::Read:
        return Operation::ReadResponse;
    case Operation::Write:
        return Operation::WriteResponse;
    case Operation::ReadResponse:
    case Operation::WriteResponse:
        return request;
    }
    return request;
}

/// A short, stable name for an operation, for logs and test failure messages.
[[nodiscard]] const char* operation_name(Operation op) noexcept;

} // namespace smply

#endif // SMPLY_SMP_HEADER_HPP
