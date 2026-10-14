// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SUPPORT_BOOT_MODEL_IMAGE_READER_HPP
#define SMPLY_SUPPORT_BOOT_MODEL_IMAGE_READER_HPP

/// \file
/// The boot model's own reader of MCUboot images in flash.
///
/// Read here from the layout table in docs/protocol-notes.md section 7 rather
/// than through `src/image/` or smply's public image parser, for the same
/// reason the CBOR codec in `support/minicbor/` is independent: a test that
/// parsed slot content with the parser under test could not tell a correct
/// image from one both halves agree to misread. **Nothing in this directory
/// may include a private smply header, or `smply/mcuboot_image.hpp`.**

#include "smply/bytes.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace smply::boot_model {

/// What the device can say about a slot: everything image-state reports.
struct SlotImage
{
    /// As `img_mgmt_ver_str()` formats it: "major.minor.revision", plus
    /// ".build" when the build number is not zero (S14).
    std::string version;
    /// The `IMAGE_TLV_SHA256` value from the unprotected TLV area, or empty.
    std::vector<std::byte> hash;
    /// False when the header carries `IMAGE_F_NON_BOOTABLE`.
    bool bootable = true;
};

/// Reads a slot's content the way `img_mgmt_read_info()` does.
///
/// Returns nullopt when the content is not a valid image, which is the
/// mechanism behind "a response only contains valid images, invalid ones are
/// simply skipped" (docs/protocol-notes.md section 6, S14).
[[nodiscard]] std::optional<SlotImage> describe(ConstBytes content);

/// The three fields `boot_compare_version()` compares (S41): major, minor and
/// revision, without the build number.
struct HeaderVersion
{
    unsigned major = 0;
    unsigned minor = 0;
    unsigned revision = 0;

    [[nodiscard]] friend auto operator<=>(const HeaderVersion&, const HeaderVersion&) = default;
};

/// The version in \p content's header, read whenever a whole header is there,
/// magic or not, as MCUboot reads a slot it has already chosen to compare.
[[nodiscard]] std::optional<HeaderVersion> header_version(ConstBytes content);

} // namespace smply::boot_model

#endif // SMPLY_SUPPORT_BOOT_MODEL_IMAGE_READER_HPP
