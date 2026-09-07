// SPDX-License-Identifier: Apache-2.0

#include "demo_image.hpp"

#include "smply/mcuboot_image.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace smply::example {
namespace {

/// `image_tlv_info.magic` for the unprotected area (protocol-notes section 7).
constexpr std::uint16_t kTlvInfoMagic = 0x6907;
/// `IMAGE_TLV_SHA256`.
constexpr std::uint16_t kTlvSha256 = 0x10;
constexpr std::size_t kSha256Length = 32;
/// Both `image_tlv_info` and `image_tlv` are four bytes.
constexpr std::size_t kTlvHeaderSize = 4;

void put8(std::vector<std::byte>& out, std::uint8_t value)
{
    out.push_back(static_cast<std::byte>(value));
}

void put16(std::vector<std::byte>& out, std::uint16_t value)
{
    put8(out, static_cast<std::uint8_t>(value & 0xFFU));
    put8(out, static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void put32(std::vector<std::byte>& out, std::uint32_t value)
{
    put16(out, static_cast<std::uint16_t>(value & 0xFFFFU));
    put16(out, static_cast<std::uint16_t>((value >> 16U) & 0xFFFFU));
}

} // namespace

std::vector<std::byte> build_demo_image(DemoVersion version, std::size_t body_size)
{
    std::vector<std::byte> out;

    // --- struct image_header, 32 bytes, little-endian ----------------------
    put32(out, kMcubootImageMagic);
    put32(out, 0);                                              // ih_load_addr
    put16(out, static_cast<std::uint16_t>(kMcubootHeaderSize)); // ih_hdr_size
    put16(out, 0);                                     // ih_protect_tlv_size: no protected area
    put32(out, static_cast<std::uint32_t>(body_size)); // ih_img_size, excludes the header
    put32(out, 0);                                     // ih_flags
    put8(out, version.major);
    put8(out, version.minor);
    put16(out, version.revision);
    put32(out, version.build);
    put32(out, 0); // _pad1

    // --- body ---------------------------------------------------------------
    // A recognisable pattern rather than zeroes, so a truncated or misaligned
    // transfer shows up as garbage rather than as a plausible run of nulls.
    for (std::size_t i = 0; i < body_size; ++i) {
        put8(out, static_cast<std::uint8_t>((i * std::size_t{31} + version.major) & 0xFFU));
    }

    // --- unprotected TLV area ----------------------------------------------
    // `total` includes this four-byte header of its own; getting that wrong is
    // the mistake protocol-notes section 7 calls out.
    put16(out, kTlvInfoMagic);
    put16(out, static_cast<std::uint16_t>(kTlvHeaderSize + kTlvHeaderSize + kSha256Length));
    put16(out, kTlvSha256);
    put16(out, static_cast<std::uint16_t>(kSha256Length));
    // Everything in std::uint32_t, and said so: mixing widths here is how a
    // pattern generator ends up depending on the platform's int size.
    const auto seed = static_cast<std::uint32_t>(version.major) * 97U +
                      static_cast<std::uint32_t>(version.minor) + version.build * 13U;
    for (std::uint32_t i = 0; i < kSha256Length; ++i) {
        put8(out, static_cast<std::uint8_t>((i * 7U) ^ seed));
    }

    return out;
}

std::string demo_version_string(DemoVersion version)
{
    std::string text = std::to_string(version.major) + '.' + std::to_string(version.minor) + '.' +
                       std::to_string(version.revision);
    if (version.build != 0) {
        text += '.' + std::to_string(version.build);
    }
    return text;
}

} // namespace smply::example
