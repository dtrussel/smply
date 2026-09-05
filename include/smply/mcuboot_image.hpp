// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_MCUBOOT_IMAGE_HPP
#define SMPLY_MCUBOOT_IMAGE_HPP

/// \file
/// The little smply knows about the MCUboot image format
/// (docs/protocol-notes.md section 7).
///
/// The image is essentially opaque, with three narrow exceptions
/// ([ADR-0009](docs/decisions/ADR-0009-mcuboot-boundary.md)): parse the 32-byte
/// header, compute the file's SHA-256, and optionally find the image-hash TLV.
///
/// smply does **not** verify signatures, decrypt, evaluate dependency TLVs,
/// reimplement swap logic, or modify the image. Verification belongs where the
/// key is, which is the device. **A successful smply update is not an
/// authenticity statement** (docs/security.md section 1).
///
/// Everything here is a pure function over an `ImageSource`. Nothing allocates
/// on a number read out of the file, and every offset is bounded against the
/// source's length before it is used.

#include "smply/bytes.hpp"
#include "smply/groups/image.hpp"
#include "smply/image_source.hpp"
#include "smply/result.hpp"

#include <cstdint>
#include <optional>

namespace smply {

/// The MCUboot magic, `ih_magic`, at offset 0 of a signed image.
inline constexpr std::uint32_t kMcubootImageMagic = 0x96F3B83DU;

/// The magic of MCUboot's first image format, which smply cannot use.
///
/// Recognised only so that an image from a much older toolchain can be reported
/// as out of date rather than as "not an MCUboot image at all".
inline constexpr std::uint32_t kMcubootImageMagicV1 = 0x96F3B83CU;

/// Size of the MCUboot image header, and the smallest legal `ih_hdr_size`.
inline constexpr std::size_t kMcubootHeaderSize = 32;

/// What the 32-byte header says.
///
/// Only the fields smply has a use for. Unknown `flags` bits are carried
/// through rather than rejected: the format grows, and a bit smply does not
/// name is not an error.
struct McubootImageInfo
{
    /// `ih_hdr_size`. The image body starts here; at least `kMcubootHeaderSize`.
    std::uint32_t header_size = 0;
    /// `ih_img_size`, the body length. **Excludes** the header.
    std::uint32_t image_size = 0;
    /// `ih_protect_tlv_size`. Zero when there is no protected TLV area, and
    /// otherwise **includes** that area's own four-byte info header
    /// (docs/protocol-notes.md section 7).
    std::uint32_t protected_tlv_size = 0;
    /// `ih_flags`, verbatim.
    std::uint32_t flags = 0;
    /// `ih_ver`. What the device will report for this image once it is flashed.
    ImageVersion version;
    /// True for `IMAGE_F_ENCRYPTED_AES128` or `IMAGE_F_ENCRYPTED_AES256`.
    ///
    /// Upload works; hash correlation does not, because the bytes on the device
    /// are not the bytes in the file (docs/protocol-notes.md section 9, A13).
    bool encrypted = false;

    [[nodiscard]] friend constexpr bool operator==(const McubootImageInfo&,
                                                   const McubootImageInfo&) noexcept = default;
};

/// Parses the 32-byte MCUboot header.
///
/// Field by field out of the span -- never a struct cast and never a
/// `reinterpret_cast`, because the layout is little-endian on the wire whatever
/// the host is, and a packed struct read is a portability and alignment trap.
///
/// This is the cheap pre-flight check ADR-0009 exists for: the server performs
/// exactly this validation on the first upload chunk
/// (docs/protocol-notes.md section 6, rule 3), so failing here turns "the
/// upload dies 200 KB in with `InvalidImageHeaderMagic`" into "this is not a
/// signed image" before a byte goes out. The commonest cause is picking
/// `zephyr.bin` instead of `zephyr.signed.bin`.
///
/// \param first_32_bytes At least `kMcubootHeaderSize` bytes from offset 0.
///                       Anything beyond that is ignored.
/// \return `ErrorCode::InvalidArgument` for a short span, a wrong magic, an
///         `ih_hdr_size` smaller than the header itself, or a header plus body
///         larger than `limits::kMaxImageSize`.
[[nodiscard]] Result<McubootImageInfo> parse_mcuboot_header(ConstBytes first_32_bytes);

/// SHA-256 of the **whole file**: the MCUmgr upload `sha` field.
///
/// This is *not* the image-state `hash` -- see `find_image_tlv_hash()` and
/// docs/protocol-notes.md section 7. It is computed over every byte the client
/// uploads, header and TLVs included, and it is what lets the device resume an
/// interrupted upload and verify what it flashed.
///
/// Streams the source a few kilobytes at a time and buffers nothing whole, so
/// the cost is bounded regardless of image size.
[[nodiscard]] Result<Hash> sha256(ImageSource& source);

/// Finds the image-hash TLV, for correlating this file with a device slot.
///
/// This is the **other** hash: MCUboot's `IMAGE_TLV_SHA256`, `SHA384` or
/// `SHA512` over the header, body and protected TLVs, which is what image-state
/// reports as `ImageSlot::hash`. The result compares directly against that,
/// so a caller can tell whether the device is already holding *this* file
/// without taking its word for it.
///
/// \return `std::nullopt` when there is no hash TLV -- which is a normal
///         answer, not a failure. An **encrypted** image also yields
///         `std::nullopt` without scanning: the flashed bytes differ from the
///         file's, so correlation is meaningless (ADR-0009, A13).
///
/// Fails with `ErrorCode::MalformedMessage` on a structurally broken TLV area:
/// a length that overruns the area, a protected size that disagrees with the
/// protected area's own header, an area extending past the end of the file,
/// more entries than `limits::kMaxImageTlvs`, or two hash TLVs in one image
/// (which is what the device reports as `ImageError::TlvMultipleHashesFound`).
[[nodiscard]] Result<std::optional<ImageHash>> find_image_tlv_hash(ImageSource& source,
                                                                   const McubootImageInfo& info);

} // namespace smply

#endif // SMPLY_MCUBOOT_IMAGE_HPP
