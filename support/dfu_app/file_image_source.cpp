// SPDX-License-Identifier: Apache-2.0

#include "dfu_app/file_image_source.hpp"

#include "smply/error.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <utility>

namespace smply::dfu_app {

FileImageSource::FileImageSource(std::unique_ptr<std::ifstream> stream, std::uint64_t size) noexcept
    : stream_{std::move(stream)}, size_{size}
{}

Result<FileImageSource> FileImageSource::open(const std::string& path)
{
    auto stream = std::make_unique<std::ifstream>(path, std::ios::binary | std::ios::ate);
    if (!*stream) {
        return fail(ErrorCode::InvalidArgument, "cannot open the image file");
    }

    const std::streamoff end = stream->tellg();
    if (end <= 0) {
        return fail(ErrorCode::InvalidArgument, "the image file is empty");
    }

    return FileImageSource{std::move(stream), static_cast<std::uint64_t>(end)};
}

std::uint64_t FileImageSource::size() const noexcept
{
    return size_;
}

Result<std::size_t> FileImageSource::read(std::uint64_t offset, MutBytes out)
{
    if (offset >= size_) {
        return std::size_t{0}; // end of file, not a failure
    }

    const std::uint64_t remaining = size_ - offset;
    const std::size_t wanted =
        out.size() < remaining ? out.size() : static_cast<std::size_t>(remaining);

    stream_->clear(); // a previous read may have set eofbit
    stream_->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!*stream_) {
        return fail(ErrorCode::InvalidArgument, "seek failed on the image file");
    }

    // istream speaks char, so this is the one conversion in the file. It is not
    // a cast over device data into a structure, which design.md section 11
    // forbids -- these are bytes the user named.
    //
    // The marker has to be the *last* comment line before the code: on a line
    // followed by more comment, NOLINTNEXTLINE silences the comment.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    stream_->read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(wanted));
    const auto got = static_cast<std::size_t>(stream_->gcount());
    if (got != wanted) {
        // The file shrank under us, or the read failed. Either way the source is
        // broken rather than exhausted -- `wanted` was already clamped to what
        // the file said it had.
        return fail(ErrorCode::InvalidArgument, "short read from the image file");
    }
    return got;
}

} // namespace smply::dfu_app
