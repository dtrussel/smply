// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SRC_IMAGE_SOURCE_READER_HPP
#define SMPLY_SRC_IMAGE_SOURCE_READER_HPP

/// \file
/// Bounded reads and little-endian field loads, shared by the header parser and
/// the TLV scanner.
///
/// Both parse attacker-supplied file content, so both need the same two
/// properties: a read either delivers every byte asked for or fails, and a
/// multi-byte field is assembled from bytes rather than cast over them. Sharing
/// them here keeps one definition of "a short read is a broken source".

#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/image_source.hpp"
#include "smply/result.hpp"

#include <cstddef>
#include <cstdint>

namespace smply::image {

/// Narrows a value already known to fit into the destination type.
///
/// Written as a template on purpose. On a 64-bit host `std::uint64_t` and
/// `std::size_t` are the same type, and GCC's `-Wuseless-cast` rejects a direct
/// `static_cast` between them; on a 32-bit host the narrowing is real and must
/// not be left implicit. A dependent conversion satisfies both, and says at the
/// call site that the value was checked first.
template<class To, class From>
[[nodiscard]] constexpr To narrow(From value) noexcept
{
    return static_cast<To>(value);
}

/// Reads exactly `out.size()` bytes, or fails.
///
/// A source that returns fewer bytes anywhere but at the end of the image has
/// broken the `ImageSource` contract; that is the application's bug, so it is
/// reported as `InvalidArgument` rather than retried. Retrying would let a
/// pathological source turn one read into arbitrarily many.
[[nodiscard]] inline Result<void> read_exact(ImageSource& source, std::uint64_t offset,
                                             MutBytes out)
{
    // Checked before the call rather than trusting the source to notice: the
    // end of the image is not an error to a source, so only the caller can tell
    // "past the end" from "short read".
    if (offset > source.size() || out.size() > source.size() - offset) {
        return fail(Error{ErrorCode::MalformedMessage, "image: read past the end of the image"});
    }

    const auto read = source.read(offset, out);
    if (!read.has_value()) {
        return fail(read.error());
    }
    if (*read != out.size()) {
        return fail(Error{ErrorCode::InvalidArgument, "image: source returned a short read"});
    }
    return {};
}

/// Loads a little-endian 16-bit field. MCUboot's on-disk fields are
/// little-endian whatever the host is (docs/protocol-notes.md section 7).
[[nodiscard]] inline std::uint16_t load_le16(ConstBytes from, std::size_t at) noexcept
{
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(from[at]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(from[at + 1]) << 8U));
}

/// \overload For a 32-bit field.
[[nodiscard]] inline std::uint32_t load_le32(ConstBytes from, std::size_t at) noexcept
{
    return static_cast<std::uint32_t>(from[at]) | (static_cast<std::uint32_t>(from[at + 1]) << 8U) |
           (static_cast<std::uint32_t>(from[at + 2]) << 16U) |
           (static_cast<std::uint32_t>(from[at + 3]) << 24U);
}

} // namespace smply::image

#endif // SMPLY_SRC_IMAGE_SOURCE_READER_HPP
