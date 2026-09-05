// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_IMAGE_SOURCE_HPP
#define SMPLY_IMAGE_SOURCE_HPP

/// \file
/// Where the firmware image comes from.
///
/// smply never opens a file. The application supplies the bytes through this
/// interface, which keeps the core free of file I/O, of any notion of a path,
/// and of a platform (docs/architecture.md section 2) -- an image may equally
/// live in flash, in a resource section, behind a download, or in memory.
///
/// The one implementation smply provides is `MemoryImageSource`, over a span
/// the caller already holds. Anything else -- a file, a stream, a decompressor
/// -- is the application's, and needs only these two functions.
///
/// **Lifetime.** A source is borrowed for the duration of the operation that
/// takes it, and must outlive it. Nothing here takes ownership.

#include "smply/bytes.hpp"
#include "smply/result.hpp"

#include <cstddef>
#include <cstdint>

namespace smply {

/// A readable firmware image of known length.
///
/// Implementations must be safe to call repeatedly, in any offset order:
/// `sha256()` walks forwards, while the TLV scan seeks near the end and back.
class ImageSource
{
public:
    ImageSource() = default;
    ImageSource(const ImageSource&) = default;
    ImageSource(ImageSource&&) = default;
    ImageSource& operator=(const ImageSource&) = default;
    ImageSource& operator=(ImageSource&&) = default;
    virtual ~ImageSource() = default;

    /// Total length in bytes. Must not change while an operation is in
    /// progress.
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;

    /// Reads into \p out, starting at \p offset.
    ///
    /// \return The number of bytes read, which **must** be `out.size()` unless
    ///         the read reached the end of the image; a read starting at or
    ///         past `size()` returns 0.
    ///
    /// A short read anywhere but at the end is a broken source, and smply
    /// reports it as `ErrorCode::InvalidArgument` rather than looping: the
    /// application supplied the object, and a source that returns one byte at a
    /// time would otherwise turn hashing a 16 MiB image into sixteen million
    /// virtual calls.
    [[nodiscard]] virtual Result<std::size_t> read(std::uint64_t offset, MutBytes out) = 0;
};

/// An image already in memory.
///
/// Borrows the span: it must outlive this object, which in turn must outlive
/// the operation using it. Copying an image that the caller already holds would
/// double the peak memory for no benefit.
class MemoryImageSource final : public ImageSource
{
public:
    explicit MemoryImageSource(ConstBytes bytes) noexcept;

    [[nodiscard]] std::uint64_t size() const noexcept override;

    /// Never fails. Reads past the end are clamped, and a read starting beyond
    /// the end yields zero bytes rather than an error -- end of file is not a
    /// failure.
    [[nodiscard]] Result<std::size_t> read(std::uint64_t offset, MutBytes out) override;

private:
    ConstBytes bytes_;
};

} // namespace smply

#endif // SMPLY_IMAGE_SOURCE_HPP
