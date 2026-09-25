// SPDX-License-Identifier: Apache-2.0
//
// The two values a device reports about an image and MCUboot defines:
// ImageHash (the image-hash TLV) and ImageVersion (ih_ver). They live with the
// image layer, below the image group, so that parsing a file does not depend
// on the SMP client (docs/architecture.md section 3).

#include "smply/error.hpp"
#include "smply/limits.hpp"
#include "smply/mcuboot_image.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace smply {

Result<ImageHash> ImageHash::from(ConstBytes bytes) noexcept
{
    if (bytes.empty()) {
        // An absent hash decodes to std::nullopt; a present but empty one is a
        // device saying something that cannot be true.
        return fail(ErrorCode::CborDecode, "image: empty hash");
    }
    if (bytes.size() > limits::kMaxImageHashLength) {
        return fail(ErrorCode::CborDecode, "image: hash too long");
    }
    ImageHash hash;
    std::copy(bytes.begin(), bytes.end(), hash.data_.begin());
    hash.size_ = bytes.size();
    return hash;
}

ImageHash ImageHash::from(const Hash& hash) noexcept
{
    ImageHash value;
    std::copy(hash.begin(), hash.end(), value.data_.begin());
    value.size_ = hash.size();
    return value;
}

Result<ImageVersion> ImageVersion::parse(std::string_view text)
{
    if (text.empty() || text.size() > limits::kMaxVersionStringLength) {
        return fail(ErrorCode::InvalidArgument, "image: version string not parseable");
    }

    // Hand-written rather than delegating to a stream or strtoul: both accept
    // leading signs, whitespace and other spellings this grammar does not have,
    // and neither reports trailing junk without extra care.
    std::size_t at = 0;
    const auto number = [&text, &at](std::uint64_t limit) -> Result<std::uint64_t> {
        const std::size_t start = at;
        std::uint64_t value = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
            value = (value * 10) + static_cast<std::uint64_t>(text[at] - '0');
            if (value > limit) {
                return fail(ErrorCode::InvalidArgument, "image: version component too large");
            }
            ++at;
        }
        if (at == start) {
            return fail(ErrorCode::InvalidArgument, "image: version component missing");
        }
        return value;
    };
    const auto separator = [&text, &at](char expected) {
        if (at < text.size() && text[at] == expected) {
            ++at;
            return true;
        }
        return false;
    };

    ImageVersion version;
    const auto major = number(std::numeric_limits<std::uint8_t>::max());
    if (!major.has_value()) {
        return fail(major.error());
    }
    if (!separator('.')) {
        return fail(ErrorCode::InvalidArgument, "image: version needs major.minor.revision");
    }
    const auto minor = number(std::numeric_limits<std::uint8_t>::max());
    if (!minor.has_value()) {
        return fail(minor.error());
    }
    if (!separator('.')) {
        return fail(ErrorCode::InvalidArgument, "image: version needs major.minor.revision");
    }
    const auto revision = number(std::numeric_limits<std::uint16_t>::max());
    if (!revision.has_value()) {
        return fail(revision.error());
    }

    version.major = static_cast<std::uint8_t>(*major);
    version.minor = static_cast<std::uint8_t>(*minor);
    version.revision = static_cast<std::uint16_t>(*revision);

    // The device writes ".build" and imgtool takes "+build"; both are accepted
    // so a string can round-trip either way (docs/protocol-notes.md section 6).
    if (separator('.') || separator('+')) {
        const auto build = number(std::numeric_limits<std::uint32_t>::max());
        if (!build.has_value()) {
            return fail(build.error());
        }
        version.build = static_cast<std::uint32_t>(*build);
    }

    if (at != text.size()) {
        return fail(ErrorCode::InvalidArgument, "image: trailing text after version");
    }
    return version;
}

std::string ImageVersion::to_string() const
{
    std::string text =
        std::to_string(major) + '.' + std::to_string(minor) + '.' + std::to_string(revision);
    if (build != 0) {
        text += '.' + std::to_string(build);
    }
    return text;
}

} // namespace smply
