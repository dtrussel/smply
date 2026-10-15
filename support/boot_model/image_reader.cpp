// SPDX-License-Identifier: Apache-2.0

#include "boot_model/image_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace smply::boot_model {

namespace {

// --- MCUboot image layout ---------------------------------------------------
//
// Read here from the layout table in docs/protocol-notes.md section 7 rather
// than through src/image/, for the same reason the CBOR codec is independent:
// a component test that parsed slot content with the parser under test could
// not tell a correct image from one both halves agree to misread.

constexpr std::uint16_t kTlvInfoMagic = 0x6907;
constexpr std::uint16_t kTlvProtInfoMagic = 0x6908;
constexpr std::uint16_t kTlvSha256 = 0x10;
constexpr std::uint32_t kImageNonBootable = 0x00000010U;
constexpr std::size_t kAreaHeaderSize = 4;

[[nodiscard]] std::uint16_t read16(ConstBytes bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes[offset]) |
                                      (std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8U));
}

/// Whether `base + add` neither overflows nor passes \p limit
/// (docs/design.md section 11: `off + len < off` and `off + len > size`).
[[nodiscard]] bool fits(std::uint64_t base, std::uint64_t add, std::uint64_t limit)
{
    const std::uint64_t end = base + add;
    return end >= base && end <= limit;
}

} // namespace

std::uint32_t read32(ConstBytes bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= std::to_integer<std::uint32_t>(bytes[offset + i])
                 << (8U * static_cast<unsigned>(i));
    }
    return value;
}

std::optional<SlotImage> describe(ConstBytes content)
{
    if (content.size() < kImageHeaderSize || read32(content, 0) != kImageMagic) {
        return std::nullopt;
    }

    const std::uint16_t header_size = read16(content, 8);
    const std::uint32_t image_size = read32(content, 12);
    const std::uint32_t flags = read32(content, 16);

    SlotImage out;
    out.bootable = (flags & kImageNonBootable) == 0;

    // img_mgmt_ver_str(): "major.minor.revision", plus ".build" when non-zero.
    const auto major = std::to_integer<unsigned>(content[20]);
    const auto minor = std::to_integer<unsigned>(content[21]);
    const std::uint16_t revision = read16(content, 22);
    const std::uint32_t build = read32(content, 24);
    out.version =
        std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(revision);
    if (build != 0) {
        out.version += "." + std::to_string(build);
    }

    // The hash TLV lives in the unprotected area, which follows the protected
    // one when there is a protected one. Both areas are walked as one run.
    // Every offset is std::uint64_t and every addition is checked for both
    // overflow and the end of the content (docs/design.md section 11).
    const std::uint64_t size = content.size();
    if (!fits(header_size, image_size, std::numeric_limits<std::uint64_t>::max())) {
        return out;
    }
    std::uint64_t offset = std::uint64_t{header_size} + image_size;
    for (int area = 0; area < 2; ++area) {
        if (!fits(offset, kAreaHeaderSize, size)) {
            break;
        }
        const auto at = static_cast<std::size_t>(offset);
        const std::uint16_t magic = read16(content, at);
        const std::uint16_t total = read16(content, at + 2);
        if (magic != kTlvInfoMagic && magic != kTlvProtInfoMagic) {
            break;
        }
        if (total < kAreaHeaderSize || !fits(offset, total, size)) {
            break;
        }

        const std::uint64_t area_end = offset + total;
        std::uint64_t entry = offset + kAreaHeaderSize;
        while (fits(entry, kAreaHeaderSize, area_end)) {
            const auto entry_at = static_cast<std::size_t>(entry);
            const std::uint16_t type = read16(content, entry_at);
            const std::uint16_t length = read16(content, entry_at + 2);
            const std::uint64_t value = entry + kAreaHeaderSize;
            if (!fits(value, length, area_end)) {
                break;
            }
            if (type == kTlvSha256 && magic == kTlvInfoMagic) {
                const ConstBytes bytes = content.subspan(static_cast<std::size_t>(value), length);
                out.hash.assign(bytes.begin(), bytes.end());
            }
            entry = value + length;
        }

        offset = area_end;
        if (magic == kTlvInfoMagic) {
            break;
        }
    }

    return out;
}

std::optional<HeaderVersion> header_version(ConstBytes content)
{
    if (content.size() < kImageHeaderSize) {
        return std::nullopt;
    }
    // major, minor and revision, as boot_compare_version() reads them (S41).
    return HeaderVersion{std::to_integer<unsigned>(content[20]),
                         std::to_integer<unsigned>(content[21]), read16(content, 22)};
}

} // namespace smply::boot_model
