// SPDX-License-Identifier: Apache-2.0
//
// The header parse is the cheap pre-flight check ADR-0009 exists for, so the
// cases that matter most are the rejections: the wrong file, the too-old file,
// and the file whose own fields are absurd.
//
// The golden vector below is written out byte by byte from the layout table in
// docs/protocol-notes.md section 7 and asserted against ImageBuilder, which is
// what keeps the builder the rest of this suite relies on honest.

#include "smply/mcuboot_image.hpp"

#include "smply/limits.hpp"

#include "image_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

using smply::ConstBytes;
using smply::ErrorCode;
using smply::ImageVersion;
using smply::parse_mcuboot_header;
using smply::test::ImageBuilder;

namespace Catch {
template<>
struct StringMaker<smply::ErrorCode>
{
    static std::string convert(smply::ErrorCode code)
    {
        return std::string{smply::to_string(code)};
    }
};
} // namespace Catch

namespace {

std::vector<std::byte> bytes_of(std::initializer_list<std::uint8_t> values)
{
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const std::uint8_t value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

/// The first 32 bytes of an image with header_size 32, body 64, version 1.2.3.4
/// and no flags -- little-endian throughout.
std::vector<std::byte> golden_header()
{
    return bytes_of({
        0x3D, 0xB8, 0xF3, 0x96, // ih_magic     = 0x96F3B83D
        0x00, 0x00, 0x00, 0x00, // ih_load_addr = 0
        0x20, 0x00,             // ih_hdr_size  = 32
        0x00, 0x00,             // ih_protect_tlv_size = 0
        0x40, 0x00, 0x00, 0x00, // ih_img_size  = 64
        0x00, 0x00, 0x00, 0x00, // ih_flags     = 0
        0x01,                   // iv_major     = 1
        0x02,                   // iv_minor     = 2
        0x03, 0x00,             // iv_revision  = 3
        0x04, 0x00, 0x00, 0x00, // iv_build_num = 4
        0x00, 0x00, 0x00, 0x00, // _pad1
    });
}

} // namespace

TEST_CASE("the builder agrees with a hand-written header", "[mcuboot][header]")
{
    // Proves the builder used by every other case in this file and by
    // test_tlv.cpp, against bytes derived from the layout table rather than
    // from either implementation.
    const auto built = ImageBuilder{}.build();
    const auto golden = golden_header();
    REQUIRE(built.size() > golden.size());
    REQUIRE(std::vector<std::byte>(built.begin(), built.begin() + 32) == golden);
}

TEST_CASE("a golden header parses field by field", "[mcuboot][header]")
{
    const auto golden = golden_header();
    const auto info = parse_mcuboot_header(ConstBytes{golden});

    REQUIRE(info.has_value());
    REQUIRE(info->header_size == 32);
    REQUIRE(info->image_size == 64);
    REQUIRE(info->protected_tlv_size == 0);
    REQUIRE(info->flags == 0);
    REQUIRE(info->version == ImageVersion{1, 2, 3, 4});
    REQUIRE_FALSE(info->encrypted);
}

TEST_CASE("every header field is read from its documented offset", "[mcuboot][header]")
{
    // Distinct values in every field, so a field read from the wrong offset
    // cannot coincidentally look right.
    // The body is patched in rather than built, so the test does not allocate
    // eleven megabytes to check four bytes of header.
    auto image = ImageBuilder{}
                     .header_size(0x0100)
                     .body(0)
                     .flags(0x00000100) // IMAGE_F_ROM_FIXED
                     .version(9, 8, 0x1234, 0x89ABCDEF)
                     .build();
    image[12] = std::byte{0xEF};
    image[13] = std::byte{0xCD};
    image[14] = std::byte{0xAB};
    image[15] = std::byte{0x00};
    const auto info = parse_mcuboot_header(ConstBytes{image});

    REQUIRE(info.has_value());
    REQUIRE(info->header_size == 0x0100);
    REQUIRE(info->image_size == 0x00ABCDEF);
    REQUIRE(info->flags == 0x00000100);
    REQUIRE(info->version == ImageVersion{9, 8, 0x1234, 0x89ABCDEF});
}

TEST_CASE("the version is the one the device will report", "[mcuboot][header]")
{
    // The header carries the same four numbers image-state renders as a string,
    // so the two are directly comparable for pre-flight (protocol-notes §6).
    const auto image = ImageBuilder{}.version(1, 2, 3, 0).build();
    const auto info = parse_mcuboot_header(ConstBytes{image});
    REQUIRE(info.has_value());
    REQUIRE(info->version.to_string() == "1.2.3");
    REQUIRE(ImageVersion::parse(info->version.to_string()) == info->version);
}

TEST_CASE("an unsigned binary is rejected by its magic", "[mcuboot][header]")
{
    // Picking zephyr.bin instead of zephyr.signed.bin is the commonest user
    // error this check exists to catch before a byte is transferred.
    const auto image = ImageBuilder{}.magic(0x00000000).build();
    const auto info = parse_mcuboot_header(ConstBytes{image});

    REQUIRE_FALSE(info.has_value());
    REQUIRE(info.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("an MCUboot v1 image is rejected as too old, not as foreign", "[mcuboot][header]")
{
    // 0x96f3b83c is a real MCUboot image from an older toolchain; saying so is
    // a different diagnosis from "this is not an image at all".
    const auto image = ImageBuilder{}.magic(0x96F3B83CU).build();
    const auto info = parse_mcuboot_header(ConstBytes{image});

    REQUIRE_FALSE(info.has_value());
    REQUIRE(info.error().code() == ErrorCode::InvalidArgument);
    REQUIRE(std::string{info.error().where()}.find("too old") != std::string::npos);
}

TEST_CASE("every truncation of a header is refused", "[mcuboot][header][hostile]")
{
    const auto golden = golden_header();
    for (std::size_t length = 0; length < smply::kMcubootHeaderSize; ++length) {
        INFO("length " << length);
        const auto info = parse_mcuboot_header(ConstBytes{golden.data(), length});
        REQUIRE_FALSE(info.has_value());
        REQUIRE(info.error().code() == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("a header claiming to be smaller than itself is refused", "[mcuboot][header][hostile]")
{
    // ih_hdr_size is where the body starts; below 32 it would overlap the
    // header it is part of.
    for (const std::uint16_t size : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{31}}) {
        const auto image = ImageBuilder{}.header_size(size).build();
        INFO("ih_hdr_size " << size);
        const auto info = parse_mcuboot_header(ConstBytes{image});
        REQUIRE_FALSE(info.has_value());
        REQUIRE(info.error().code() == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("a header size of exactly 32 is accepted", "[mcuboot][header]")
{
    const auto image = ImageBuilder{}.header_size(32).build();
    REQUIRE(parse_mcuboot_header(ConstBytes{image}).has_value());
}

TEST_CASE("an absurd image size is refused", "[mcuboot][header][hostile]")
{
    // 0xFFFFFFFF bytes is not a firmware image. Bounded against
    // limits::kMaxImageSize before anything sizes a buffer from it.
    const auto image = ImageBuilder{}.body(0).build();
    auto raw = image;
    raw[12] = std::byte{0xFF};
    raw[13] = std::byte{0xFF};
    raw[14] = std::byte{0xFF};
    raw[15] = std::byte{0xFF};

    const auto info = parse_mcuboot_header(ConstBytes{raw});
    REQUIRE_FALSE(info.has_value());
    REQUIRE(info.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("an image at exactly the size bound is accepted", "[mcuboot][header]")
{
    // The header is not required to be present in full -- only its first 32
    // bytes are parsed -- so the bound is on what the header claims.
    const auto image = ImageBuilder{}.header_size(32).body(0).build();
    auto raw = image;
    const std::uint32_t body = static_cast<std::uint32_t>(smply::limits::kMaxImageSize) - 32;
    raw[12] = static_cast<std::byte>(body & 0xFFU);
    raw[13] = static_cast<std::byte>((body >> 8U) & 0xFFU);
    raw[14] = static_cast<std::byte>((body >> 16U) & 0xFFU);
    raw[15] = static_cast<std::byte>((body >> 24U) & 0xFFU);

    const auto info = parse_mcuboot_header(ConstBytes{raw});
    REQUIRE(info.has_value());
    REQUIRE(info->image_size == body);
}

TEST_CASE("both encryption flags set the encrypted flag", "[mcuboot][header]")
{
    // IMAGE_F_ENCRYPTED_AES128 and _AES256. An encrypted image uploads fine;
    // what it cannot do is have its hash correlated (A13).
    for (const std::uint32_t flag : {std::uint32_t{0x04}, std::uint32_t{0x08}}) {
        const auto image = ImageBuilder{}.flags(flag).build();
        INFO("flag " << flag);
        const auto info = parse_mcuboot_header(ConstBytes{image});
        REQUIRE(info.has_value());
        REQUIRE(info->encrypted);
        REQUIRE(info->flags == flag);
    }
}

TEST_CASE("an unknown flag is carried through, not rejected", "[mcuboot][header]")
{
    // The flag set grows -- compression flags were added after encryption.
    // A bit smply does not name is not an error.
    const auto image = ImageBuilder{}.flags(0x80000000U).build();
    const auto info = parse_mcuboot_header(ConstBytes{image});

    REQUIRE(info.has_value());
    REQUIRE(info->flags == 0x80000000U);
    REQUIRE_FALSE(info->encrypted);
}

TEST_CASE("bytes beyond the header are ignored", "[mcuboot][header]")
{
    // The parser is handed a span that usually runs to the end of the file.
    const auto image = ImageBuilder{}.body(4096).tlv(0x01, {}).build();
    const auto info = parse_mcuboot_header(ConstBytes{image});
    REQUIRE(info.has_value());
    REQUIRE(info->image_size == 4096);
}
