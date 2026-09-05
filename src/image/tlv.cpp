// SPDX-License-Identifier: Apache-2.0

#include "image/source_reader.hpp"
#include "smply/error.hpp"
#include "smply/limits.hpp"
#include "smply/mcuboot_image.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace smply {
namespace {

/// `IMAGE_TLV_INFO_MAGIC` -- the unprotected TLV area.
constexpr std::uint16_t kTlvInfoMagic = 0x6907;
/// `IMAGE_TLV_PROT_INFO_MAGIC` -- the protected TLV area, when there is one.
constexpr std::uint16_t kTlvProtInfoMagic = 0x6908;

/// `struct image_tlv_info` and `struct image_tlv` are both four bytes.
constexpr std::uint64_t kTlvHeaderSize = 4;

/// The image-hash TLVs, and the digest length each one must carry.
///
/// MCUboot allows all three in the unprotected area, and which one appears
/// depends on how the bootloader was built -- the same choice that makes the
/// device report a 32- or 64-byte `hash` (docs/protocol-notes.md section 6).
struct HashTlv
{
    std::uint16_t type;
    std::uint16_t length;
};

constexpr std::array<HashTlv, 3> kHashTlvs{HashTlv{0x10, 32},  // IMAGE_TLV_SHA256
                                           HashTlv{0x11, 48},  // IMAGE_TLV_SHA384
                                           HashTlv{0x12, 64}}; // IMAGE_TLV_SHA512

[[nodiscard]] const HashTlv* hash_tlv_for(std::uint16_t type) noexcept
{
    for (const HashTlv& candidate : kHashTlvs) {
        if (candidate.type == type) {
            return &candidate;
        }
    }
    return nullptr;
}

[[nodiscard]] unexpected<Error> malformed(const char* what) noexcept
{
    return fail(Error{ErrorCode::MalformedMessage, what});
}

/// One `image_tlv_info`: a magic and the total size of the area it introduces.
struct TlvAreaHeader
{
    std::uint16_t magic;
    std::uint16_t total;
};

[[nodiscard]] Result<TlvAreaHeader> read_area_header(ImageSource& source, std::uint64_t at)
{
    std::array<std::byte, kTlvHeaderSize> raw{};
    if (const auto read = image::read_exact(source, at, MutBytes{raw}); !read.has_value()) {
        return fail(read.error());
    }
    return TlvAreaHeader{image::load_le16(ConstBytes{raw}, 0),
                         image::load_le16(ConstBytes{raw}, 2)};
}

/// Where the TLV areas begin and end.
struct TlvLayout
{
    std::uint64_t first;         ///< Offset of the first TLV entry.
    std::uint64_t protected_end; ///< End of the protected area; equals `base` when there is none.
    std::uint64_t end;           ///< End of the whole TLV region.
};

/// Resolves the area layout exactly as MCUboot's `bootutil_tlv_iter_begin()`
/// does (docs/protocol-notes.md section 7).
///
/// The two areas are contiguous and are walked as one run: `ih_protect_tlv_size`
/// **includes** the protected area's own four-byte header, and the two must
/// agree exactly or MCUboot refuses the image.
[[nodiscard]] Result<TlvLayout> resolve_layout(ImageSource& source, const McubootImageInfo& info)
{
    const std::uint64_t base =
        static_cast<std::uint64_t>(info.header_size) + static_cast<std::uint64_t>(info.image_size);

    auto first = read_area_header(source, base);
    if (!first.has_value()) {
        return fail(first.error());
    }

    std::uint16_t unprotected_total = 0;
    if (first->magic == kTlvProtInfoMagic) {
        if (info.protected_tlv_size != first->total) {
            return malformed("image: protected TLV size disagrees with the header");
        }
        if (first->total < kTlvHeaderSize) {
            return malformed("image: protected TLV area smaller than its own header");
        }
        auto second = read_area_header(source, base + first->total);
        if (!second.has_value()) {
            return fail(second.error());
        }
        if (second->magic != kTlvInfoMagic) {
            return malformed("image: no unprotected TLV area after the protected one");
        }
        unprotected_total = second->total;
    } else {
        if (info.protected_tlv_size != 0) {
            // The header claims a protected area that is not there.
            return malformed("image: protected TLV size set but no protected area");
        }
        if (first->magic != kTlvInfoMagic) {
            return malformed("image: no TLV area after the image body");
        }
        unprotected_total = first->total;
    }

    if (unprotected_total < kTlvHeaderSize) {
        return malformed("image: TLV area smaller than its own header");
    }

    const std::uint64_t protected_end = base + info.protected_tlv_size;
    const std::uint64_t end = protected_end + unprotected_total;
    if (end > source.size()) {
        // Bounded against the file, not trusted: it_tlv_tot is the file's own
        // claim about how far the area reaches.
        return malformed("image: TLV area extends past the end of the image");
    }

    return TlvLayout{base + kTlvHeaderSize, protected_end, end};
}

} // namespace

Result<std::optional<ImageHash>> find_image_tlv_hash(ImageSource& source,
                                                     const McubootImageInfo& info)
{
    if (info.encrypted) {
        // The bytes on the device are not the bytes in the file, so a hash of
        // the file could never match what image-state reports. Skipped by
        // design rather than attempted and failed (ADR-0009, A13).
        return std::optional<ImageHash>{};
    }

    const auto layout = resolve_layout(source, info);
    if (!layout.has_value()) {
        return fail(layout.error());
    }

    std::optional<ImageHash> found;
    std::uint64_t offset = layout->first;
    std::size_t visited = 0;

    while (offset < layout->end) {
        if (visited == limits::kMaxImageTlvs) {
            // The scan terminates on its own -- every step advances by at least
            // the four-byte entry header -- so this bounds the work a hostile
            // file can demand, not the loop.
            return malformed("image: too many TLV entries");
        }
        ++visited;

        if (info.protected_tlv_size != 0 && offset == layout->protected_end) {
            // The unprotected area's own header sits between the two runs of
            // entries; step over it and carry on.
            offset += kTlvHeaderSize;
            continue;
        }
        if (kTlvHeaderSize > layout->end - offset) {
            return malformed("image: TLV entry header overruns the TLV area");
        }

        std::array<std::byte, kTlvHeaderSize> raw{};
        if (const auto read = image::read_exact(source, offset, MutBytes{raw}); !read.has_value()) {
            return fail(read.error());
        }
        const std::uint16_t type = image::load_le16(ConstBytes{raw}, 0);
        const std::uint16_t length = image::load_le16(ConstBytes{raw}, 2);

        // `length` comes straight out of the file. Checked against the area end
        // before it is used for anything, including the advance below.
        if (length > layout->end - offset - kTlvHeaderSize) {
            return malformed("image: TLV entry overruns the TLV area");
        }

        if (const HashTlv* hash_tlv = hash_tlv_for(type); hash_tlv != nullptr) {
            if (length != hash_tlv->length) {
                return malformed("image: hash TLV has the wrong length for its type");
            }
            if (found.has_value()) {
                // What the device reports as TlvMultipleHashesFound. Refusing
                // to choose is the only safe answer.
                return malformed("image: more than one hash TLV");
            }

            std::array<std::byte, limits::kMaxImageHashLength> digest{};
            const MutBytes into{digest.data(), length};
            if (const auto read = image::read_exact(source, offset + kTlvHeaderSize, into);
                !read.has_value()) {
                return fail(read.error());
            }
            auto hash = ImageHash::from(ConstBytes{into});
            if (!hash.has_value()) {
                return fail(hash.error());
            }
            found = *hash;
        }

        // At least kTlvHeaderSize, so the offset strictly increases and the
        // scan cannot spin on a zero-length entry.
        offset += kTlvHeaderSize + length;
    }

    return found;
}

} // namespace smply
