// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_EXAMPLE_FILE_IMAGE_SOURCE_HPP
#define SMPLY_EXAMPLE_FILE_IMAGE_SOURCE_HPP

/// \file
/// An `ImageSource` over a file on disk.
///
/// smply ships only `MemoryImageSource`, deliberately: `ImageSource` is two
/// virtual functions, and putting a file-backed one in the library would drag
/// file I/O, path handling and error mapping across three platforms into a core
/// that otherwise does no I/O at all (open question O4, resolved in P9). This is
/// the dozen lines that resolution said an application would write, written out
/// so nobody has to take it on faith.
///
/// Reads are seek-and-read rather than streaming, because the library needs
/// both directions: `sha256()` walks forwards, and the TLV scan jumps to the end
/// and works back.

#include "smply/bytes.hpp"
#include "smply/image_source.hpp"
#include "smply/result.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>

namespace smply::example {

/// A firmware file, opened once and read at arbitrary offsets.
class FileImageSource final : public ImageSource
{
public:
    /// Opens \p path for reading.
    ///
    /// \return `ErrorCode::InvalidArgument` if the file cannot be opened, or is
    ///         empty -- an empty file is not a firmware image, and failing here
    ///         gives a better message than failing later in the header parser.
    [[nodiscard]] static Result<FileImageSource> open(const std::string& path);

    FileImageSource(const FileImageSource&) = delete;
    FileImageSource(FileImageSource&&) = default;
    FileImageSource& operator=(const FileImageSource&) = delete;
    FileImageSource& operator=(FileImageSource&&) = delete;
    ~FileImageSource() override = default;

    [[nodiscard]] std::uint64_t size() const noexcept override;

    /// Reads at \p offset. A read starting at or past the end yields zero bytes,
    /// which is end-of-file and not a failure; a short read anywhere else is a
    /// broken source and reported as one.
    [[nodiscard]] Result<std::size_t> read(std::uint64_t offset, MutBytes out) override;

private:
    FileImageSource(std::unique_ptr<std::ifstream> stream, std::uint64_t size) noexcept;

    /// Held by pointer for one specific reason, and it is a constraint every
    /// consumer of this library meets sooner or later: `Result<T>` requires `T`
    /// to be nothrow-move-constructible (ADR-0002), and `std::ifstream`'s move
    /// constructor is not `noexcept`. Holding the stream indirectly makes this
    /// class nothrow-movable, so `open()` can return a `Result<FileImageSource>`
    /// rather than something clumsier.
    std::unique_ptr<std::ifstream> stream_;
    std::uint64_t size_ = 0;
};

} // namespace smply::example

#endif // SMPLY_EXAMPLE_FILE_IMAGE_SOURCE_HPP
