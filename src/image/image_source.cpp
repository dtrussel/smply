// SPDX-License-Identifier: Apache-2.0

#include "smply/image_source.hpp"

#include "image/sha256.hpp"
#include "image/source_reader.hpp"
#include "smply/error.hpp"
#include "smply/mcuboot_image.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace smply {
namespace {

/// How much of the image is held at once while hashing.
///
/// Nothing larger is ever buffered: the cost of hashing is this, whatever the
/// image size (docs/design.md section 7).
constexpr std::size_t kStreamChunk = 4096;

} // namespace

MemoryImageSource::MemoryImageSource(ConstBytes bytes) noexcept : bytes_{bytes} {}

std::uint64_t MemoryImageSource::size() const noexcept
{
    return bytes_.size();
}

Result<std::size_t> MemoryImageSource::read(std::uint64_t offset, MutBytes out)
{
    if (offset >= bytes_.size()) {
        // Reading at or past the end yields nothing. End of image is not a
        // failure -- the caller decides whether it expected more.
        return std::size_t{0};
    }
    const auto available = image::narrow<std::size_t>(bytes_.size() - offset);
    const std::size_t wanted = std::min(available, out.size());
    const ConstBytes from = bytes_.subspan(image::narrow<std::size_t>(offset), wanted);
    std::copy(from.begin(), from.end(), out.begin());
    return wanted;
}

Result<Hash> sha256(ImageSource& source)
{
    image::Sha256 hasher;
    std::array<std::byte, kStreamChunk> buffer{};

    const std::uint64_t total = source.size();
    for (std::uint64_t offset = 0; offset < total;) {
        const auto remaining = total - offset;
        const std::size_t wanted =
            remaining < buffer.size() ? image::narrow<std::size_t>(remaining) : buffer.size();

        const auto read = source.read(offset, MutBytes{buffer.data(), wanted});
        if (!read.has_value()) {
            return fail(read.error());
        }
        if (*read != wanted) {
            // Short of the end, so the source has broken its contract. Bailing
            // out rather than looping keeps a source that returns one byte at a
            // time from turning this into millions of calls.
            return fail(Error{ErrorCode::InvalidArgument, "image: source returned a short read"});
        }

        hasher.update(ConstBytes{buffer.data(), wanted});
        offset += wanted;
    }

    return hasher.finish();
}

} // namespace smply
