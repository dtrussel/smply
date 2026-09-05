// SPDX-License-Identifier: Apache-2.0

#include "smply/smp/header.hpp"

#include "message_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using smply::decode_header;
using smply::encode;
using smply::ErrorCode;
using smply::Group;
using smply::Header;
using smply::kHeaderSize;
using smply::Operation;
using smply::Version;
using smply::test::bytes_of;

namespace {

/// Renders bytes as hex, so a golden-vector failure says which byte differs.
std::string hex(smply::ConstBytes bytes)
{
    static constexpr std::array<char, 16> kDigits{'0', '1', '2', '3', '4', '5', '6', '7',
                                                  '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    std::string out;
    for (const std::byte byte : bytes) {
        const auto value = std::to_integer<std::uint8_t>(byte);
        out.push_back(kDigits.at(value >> 4U));
        out.push_back(kDigits.at(value & 0x0FU));
        out.push_back(' ');
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Golden vectors, computed by hand from the layout table in
// docs/protocol-notes.md section 2. These are the tests that would catch a
// wholesale misreading of the specification; the round-trip tests below would
// not, since they would agree with themselves.
// ---------------------------------------------------------------------------

TEST_CASE("golden vector: read request, v1, OS group, everything zero", "[smp][codec][golden]")
{
    // byte0 = res(000) ver(00) op(000) = 0x00
    const Header header{.op = Operation::Read,
                        .version = Version::V1,
                        .flags = 0,
                        .length = 0,
                        .group = Group::Os,
                        .seq = 0,
                        .command = 0};

    const auto encoded = encode(header);
    const auto expected = bytes_of({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});

    INFO("encoded: " << hex(encoded));
    REQUIRE(std::vector<std::byte>(encoded.begin(), encoded.end()) == expected);
}

TEST_CASE("golden vector: write-response, v2, image group", "[smp][codec][golden]")
{
    // byte0 = res(000) ver(01) op(011) = 0x08 | 0x03 = 0x0B
    // length 0x0102 and group 0x0001 are stored big-endian.
    const Header header{.op = Operation::WriteResponse,
                        .version = Version::V2,
                        .flags = 0,
                        .length = 0x0102,
                        .group = Group::Image,
                        .seq = 1,
                        .command = 1};

    const auto encoded = encode(header);
    const auto expected = bytes_of({0x0B, 0x00, 0x01, 0x02, 0x00, 0x01, 0x01, 0x01});

    INFO("encoded: " << hex(encoded));
    REQUIRE(std::vector<std::byte>(encoded.begin(), encoded.end()) == expected);
}

TEST_CASE("golden vector: every field at its maximum", "[smp][codec][golden]")
{
    const Header header{.op = Operation::WriteResponse,
                        .version = Version::V2,
                        .flags = 0xFF,
                        .length = 0xFFFF,
                        .group = static_cast<Group>(0xFFFF),
                        .seq = 0xFF,
                        .command = 0xFF};

    const auto encoded = encode(header);
    // byte0 stays 0x0B: the reserved bits are never set, whatever else is.
    const auto expected = bytes_of({0x0B, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});

    INFO("encoded: " << hex(encoded));
    REQUIRE(std::vector<std::byte>(encoded.begin(), encoded.end()) == expected);
}

TEST_CASE("length and group are big-endian, not host-endian", "[smp][codec][golden]")
{
    // Asymmetric values: on a little-endian host a byte-order mistake swaps
    // these and the test fails loudly, which a symmetric value would hide.
    const Header header{.op = Operation::Read,
                        .version = Version::V1,
                        .flags = 0,
                        .length = 0x1234,
                        .group = static_cast<Group>(0x5678),
                        .seq = 0,
                        .command = 0};

    const auto encoded = encode(header);
    INFO("encoded: " << hex(encoded));

    REQUIRE(std::to_integer<std::uint8_t>(encoded[2]) == 0x12); // length, high byte first
    REQUIRE(std::to_integer<std::uint8_t>(encoded[3]) == 0x34);
    REQUIRE(std::to_integer<std::uint8_t>(encoded[4]) == 0x56); // group, high byte first
    REQUIRE(std::to_integer<std::uint8_t>(encoded[5]) == 0x78);

    const auto decoded = decode_header(std::span<const std::byte, kHeaderSize>{encoded});
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->length == 0x1234);
    REQUIRE(smply::to_underlying(decoded->group) == 0x5678);
}

TEST_CASE("a known-good decode matches the layout table field by field", "[smp][codec][golden]")
{
    const auto raw = bytes_of({0x0B, 0x00, 0x01, 0x02, 0x00, 0x01, 0x2A, 0x05});

    const auto decoded = decode_header(smply::ConstBytes{raw});
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->op == Operation::WriteResponse);
    REQUIRE(decoded->version == Version::V2);
    REQUIRE(decoded->flags == 0);
    REQUIRE(decoded->length == 258);
    REQUIRE(decoded->group == Group::Image);
    REQUIRE(decoded->seq == 42);
    REQUIRE(decoded->command == 5);
}

// ---------------------------------------------------------------------------
// Round-trip
// ---------------------------------------------------------------------------

TEST_CASE("encode and decode round-trip over the field space", "[smp][codec]")
{
    const auto op = GENERATE(Operation::Read, Operation::ReadResponse, Operation::Write,
                             Operation::WriteResponse);
    const auto version = GENERATE(Version::V1, Version::V2);
    const std::uint16_t length = GENERATE(std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{255},
                                          std::uint16_t{256}, std::uint16_t{0xFFFF});
    const std::uint16_t group = GENERATE(std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{63},
                                         std::uint16_t{64}, std::uint16_t{0xFFFF});
    const std::uint8_t seq = GENERATE(std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{255});

    const Header original{.op = op,
                          .version = version,
                          .flags = 0,
                          .length = length,
                          .group = static_cast<Group>(group),
                          .seq = seq,
                          .command = 0x7F};

    const auto encoded = encode(original);
    const auto decoded = decode_header(std::span<const std::byte, kHeaderSize>{encoded});

    INFO("encoded: " << hex(encoded));
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == original);
}

TEST_CASE("unknown groups round-trip unchanged", "[smp][codec]")
{
    // Group is open: a vendor or future group must survive the trip intact
    // rather than being normalised or rejected.
    const std::uint16_t raw_group =
        GENERATE(std::uint16_t{12}, std::uint16_t{64}, std::uint16_t{200}, std::uint16_t{9000},
                 std::uint16_t{0xFFFF});

    const Header original{.op = Operation::Read,
                          .version = Version::V1,
                          .flags = 0,
                          .length = 0,
                          .group = static_cast<Group>(raw_group),
                          .seq = 0,
                          .command = 0};

    const auto encoded = encode(original);
    const auto decoded = decode_header(std::span<const std::byte, kHeaderSize>{encoded});

    REQUIRE(decoded.has_value());
    REQUIRE(smply::to_underlying(decoded->group) == raw_group);
}

TEST_CASE("unknown flag bits are preserved, not rejected", "[smp][codec]")
{
    // No flags are defined today. A device that sets one must not become
    // unreachable, so decoding carries the byte through untouched.
    const std::uint8_t flags = GENERATE(std::uint8_t{0x01}, std::uint8_t{0x80}, std::uint8_t{0xFF});

    const auto raw = bytes_of({0x00, flags, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    const auto decoded = decode_header(smply::ConstBytes{raw});

    REQUIRE(decoded.has_value());
    REQUIRE(decoded->flags == flags);
}

// ---------------------------------------------------------------------------
// Rejection
// ---------------------------------------------------------------------------

TEST_CASE("reserved bits set are rejected", "[smp][codec]")
{
    // Any of the top three bits.
    const std::uint8_t byte0 =
        GENERATE(std::uint8_t{0x20}, std::uint8_t{0x40}, std::uint8_t{0x80}, std::uint8_t{0xE0});

    const auto raw = bytes_of({byte0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    const auto decoded = decode_header(smply::ConstBytes{raw});

    REQUIRE_FALSE(decoded.has_value());
    REQUIRE(decoded.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("reserved protocol versions are rejected as unsupported", "[smp][codec]")
{
    // 0b10 and 0b11 in bits 4..3. These are reserved for future use: we cannot
    // know how such a device reports errors, so refusing is the honest answer,
    // and it is a distinct code from a malformed frame.
    const std::uint8_t byte0 = GENERATE(std::uint8_t{0x10}, std::uint8_t{0x18});

    const auto raw = bytes_of({byte0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    const auto decoded = decode_header(smply::ConstBytes{raw});

    REQUIRE_FALSE(decoded.has_value());
    REQUIRE(decoded.error().code() == ErrorCode::UnsupportedSmpVersion);
}

TEST_CASE("operations outside 0..3 are rejected", "[smp][codec]")
{
    // The field is three bits wide, but only four values are defined.
    const std::uint8_t op_bits =
        GENERATE(std::uint8_t{4}, std::uint8_t{5}, std::uint8_t{6}, std::uint8_t{7});

    const auto raw = bytes_of({op_bits, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    const auto decoded = decode_header(smply::ConstBytes{raw});

    REQUIRE_FALSE(decoded.has_value());
    REQUIRE(decoded.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("a buffer shorter than the header is rejected", "[smp][codec]")
{
    const std::size_t size = GENERATE(std::size_t{0}, std::size_t{1}, std::size_t{7});

    const std::vector<std::byte> raw(size, std::byte{0});
    const auto decoded = decode_header(smply::ConstBytes{raw});

    REQUIRE_FALSE(decoded.has_value());
    REQUIRE(decoded.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("a buffer longer than the header decodes from its first eight bytes", "[smp][codec]")
{
    // The dynamic-span overload is what the reassembler will call: trailing
    // payload bytes are not its concern.
    auto raw = bytes_of({0x0B, 0x00, 0x00, 0x03, 0x00, 0x01, 0x07, 0x02});
    raw.push_back(std::byte{0xAA});
    raw.push_back(std::byte{0xBB});
    raw.push_back(std::byte{0xCC});

    const auto decoded = decode_header(smply::ConstBytes{raw});

    REQUIRE(decoded.has_value());
    REQUIRE(decoded->length == 3);
    REQUIRE(decoded->seq == 7);
}

// ---------------------------------------------------------------------------
// Derived values and helpers
// ---------------------------------------------------------------------------

TEST_CASE("total_size covers the header plus the declared payload", "[smp][codec]")
{
    REQUIRE(Header{}.total_size() == kHeaderSize);

    Header header{};
    header.length = 100;
    REQUIRE(header.total_size() == kHeaderSize + 100);

    header.length = 0xFFFF;
    REQUIRE(header.total_size() == kHeaderSize + 0xFFFF);
}

TEST_CASE("headers compare field by field", "[smp][codec]")
{
    const Header base{.op = Operation::Read,
                      .version = Version::V1,
                      .flags = 0,
                      .length = 8,
                      .group = Group::Image,
                      .seq = 3,
                      .command = 1};

    REQUIRE(base == base);

    Header other = base;
    other.seq = 4;
    REQUIRE_FALSE(base == other);

    other = base;
    other.version = Version::V2;
    REQUIRE_FALSE(base == other);
}

TEST_CASE("operation_name covers every operation distinctly", "[smp][codec]")
{
    const std::vector<std::string_view> names{
        smply::operation_name(Operation::Read),
        smply::operation_name(Operation::ReadResponse),
        smply::operation_name(Operation::Write),
        smply::operation_name(Operation::WriteResponse),
    };

    for (const auto& name : names) {
        REQUIRE_FALSE(name.empty());
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        for (std::size_t j = i + 1; j < names.size(); ++j) {
            REQUIRE(names[i] != names[j]);
        }
    }
}

// ---------------------------------------------------------------------------
// Test support
// ---------------------------------------------------------------------------

TEST_CASE("make_message keeps the length field consistent with the payload", "[smp][support]")
{
    const auto payload = smply::test::filler(5);
    const Header header{.op = Operation::Write,
                        .version = Version::V1,
                        .flags = 0,
                        .length = 999, // deliberately wrong; make_message corrects it
                        .group = Group::Image,
                        .seq = 1,
                        .command = 1};

    const auto message = smply::test::make_message(header, payload);

    REQUIRE(message.size() == kHeaderSize + payload.size());
    const auto decoded = decode_header(smply::ConstBytes{message});
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->length == payload.size());
}

TEST_CASE("make_raw_message leaves the declared length alone", "[smp][support]")
{
    // This is how hostile input is expressed: a device claiming far more than
    // it sends. The bounds that reject it arrive with the reassembler in P3.
    const Header header{.op = Operation::WriteResponse,
                        .version = Version::V1,
                        .flags = 0,
                        .length = 60000,
                        .group = Group::Image,
                        .seq = 1,
                        .command = 1};

    const auto message = smply::test::make_raw_message(header, smply::test::filler(4));

    REQUIRE(message.size() == kHeaderSize + 4);
    const auto decoded = decode_header(smply::ConstBytes{message});
    REQUIRE(decoded.has_value());
    // The codec reports what the wire says and does not second-guess it.
    REQUIRE(decoded->length == 60000);
}
