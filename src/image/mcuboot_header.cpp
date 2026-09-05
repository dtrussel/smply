// SPDX-License-Identifier: Apache-2.0

#include "image/source_reader.hpp"
#include "smply/error.hpp"
#include "smply/limits.hpp"
#include "smply/mcuboot_image.hpp"

#include <cstdint>

namespace smply {
namespace {

/// Field offsets within the 32-byte header (docs/protocol-notes.md section 7).
///
/// Named rather than open-coded so the parse reads as the table does, and so a
/// wrong offset is a one-line diff against the specification.
constexpr std::size_t kOffsetMagic = 0;
constexpr std::size_t kOffsetHeaderSize = 8;
constexpr std::size_t kOffsetProtectTlvSize = 10;
constexpr std::size_t kOffsetImageSize = 12;
constexpr std::size_t kOffsetFlags = 16;
constexpr std::size_t kOffsetVersionMajor = 20;
constexpr std::size_t kOffsetVersionMinor = 21;
constexpr std::size_t kOffsetVersionRevision = 22;
constexpr std::size_t kOffsetVersionBuild = 24;

/// `IMAGE_F_ENCRYPTED_AES128 | IMAGE_F_ENCRYPTED_AES256`.
constexpr std::uint32_t kFlagEncrypted = 0x00000004U | 0x00000008U;

} // namespace

Result<McubootImageInfo> parse_mcuboot_header(ConstBytes first_32_bytes)
{
    if (first_32_bytes.size() < kMcubootHeaderSize) {
        return fail(Error{ErrorCode::InvalidArgument, "image: header shorter than 32 bytes"});
    }

    const std::uint32_t magic = image::load_le32(first_32_bytes, kOffsetMagic);
    if (magic != kMcubootImageMagic) {
        if (magic == kMcubootImageMagicV1) {
            // A real MCUboot image, just from a toolchain older than anything
            // this protocol supports. Worth saying so: it is a different
            // problem from having picked the wrong file.
            return fail(
                Error{ErrorCode::InvalidArgument, "image: MCUboot v1 image format, too old"});
        }
        // Overwhelmingly the commonest user error: the unsigned build output
        // rather than the signed one. The device makes exactly this check on
        // the first chunk (docs/protocol-notes.md section 6, rule 3).
        return fail(Error{ErrorCode::InvalidArgument,
                          "image: not an MCUboot image (wrong magic; unsigned binary?)"});
    }

    McubootImageInfo info;
    info.header_size = image::load_le16(first_32_bytes, kOffsetHeaderSize);
    info.protected_tlv_size = image::load_le16(first_32_bytes, kOffsetProtectTlvSize);
    info.image_size = image::load_le32(first_32_bytes, kOffsetImageSize);
    info.flags = image::load_le32(first_32_bytes, kOffsetFlags);
    info.version.major = static_cast<std::uint8_t>(first_32_bytes[kOffsetVersionMajor]);
    info.version.minor = static_cast<std::uint8_t>(first_32_bytes[kOffsetVersionMinor]);
    info.version.revision = image::load_le16(first_32_bytes, kOffsetVersionRevision);
    info.version.build = image::load_le32(first_32_bytes, kOffsetVersionBuild);
    // Unknown flag bits are carried through in `flags` rather than rejected:
    // the format grows, and a bit smply does not name is not an error.
    info.encrypted = (info.flags & kFlagEncrypted) != 0;

    if (info.header_size < kMcubootHeaderSize) {
        // The header cannot be smaller than itself, and the body offset is
        // computed from this value.
        return fail(Error{ErrorCode::InvalidArgument, "image: header size below 32 bytes"});
    }
    // Both are 32-bit fields, so the sum is computed in 64 bits and cannot
    // wrap before it is compared.
    const std::uint64_t through_body =
        static_cast<std::uint64_t>(info.header_size) + static_cast<std::uint64_t>(info.image_size);
    if (through_body > limits::kMaxImageSize) {
        return fail(Error{ErrorCode::InvalidArgument, "image: declared size implausibly large"});
    }

    return info;
}

} // namespace smply
