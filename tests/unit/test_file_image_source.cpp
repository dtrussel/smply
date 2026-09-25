// SPDX-License-Identifier: Apache-2.0
//
// support/dfu_app/file_image_source.cpp: the ImageSource every example reads
// firmware through. It is application code, not library code, but it is the
// file a user copies, so its contract is pinned the same way MemoryImageSource's
// is: end of file is zero bytes and not a failure, a read is clamped to the
// file, and a file that changes under it is a broken source rather than a short
// image.

#include "dfu_app/file_image_source.hpp"
#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/result.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <random>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using smply::ErrorCode;
using smply::MutBytes;
using smply::Result;
using smply::dfu_app::FileImageSource;

namespace {

/// A file in the temporary directory with a name no other test run shares, so
/// parallel ctest runs cannot race on it. Removed when the guard goes.
class TempFile
{
public:
    TempFile() : path_{unique_path()} {}

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    TempFile(TempFile&&) = delete;
    TempFile& operator=(TempFile&&) = delete;

    ~TempFile()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] const std::string& path() const noexcept
    {
        return path_;
    }

    /// Writes `count` bytes, each its own index modulo 128.
    void write(std::size_t count) const
    {
        std::vector<char> bytes(count);
        for (std::size_t i = 0; i < count; ++i) {
            bytes[i] = static_cast<char>(i & 0x7FU);
        }
        std::ofstream out{path_, std::ios::binary | std::ios::trunc};
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(out.good());
    }

private:
    [[nodiscard]] static std::string unique_path()
    {
        std::random_device entropy;
        const std::string name = "smply_file_image_source_" + std::to_string(entropy()) + "_" +
                                 std::to_string(entropy()) + ".bin";
        return (std::filesystem::temp_directory_path() / name).string();
    }

    std::string path_;
};

[[nodiscard]] FileImageSource open_or_fail(const std::string& path)
{
    Result<FileImageSource> source = FileImageSource::open(path);
    REQUIRE(source.has_value());
    return std::move(*source);
}

} // namespace

TEST_CASE("a file source refuses a missing file and an empty one", "[file_image_source]")
{
    const TempFile file;
    const auto missing = FileImageSource::open(file.path());
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code() == ErrorCode::InvalidArgument);

    file.write(0);
    const auto empty = FileImageSource::open(file.path());
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("a file source reads at any offset and clamps at the end", "[file_image_source]")
{
    const TempFile file;
    file.write(100);
    FileImageSource source = open_or_fail(file.path());
    CHECK(source.size() == 100);

    std::array<std::byte, 10> out{};

    // The middle first, then the start: reads arrive in any order, because the
    // TLV scan works backwards from the end.
    auto read = source.read(40, MutBytes{out});
    REQUIRE(read.has_value());
    CHECK(*read == 10);
    CHECK(out[0] == std::byte{40});
    CHECK(out[9] == std::byte{49});

    read = source.read(0, MutBytes{out});
    REQUIRE(read.has_value());
    CHECK(*read == 10);
    CHECK(out[0] == std::byte{0});

    // Straddling the end: clamped to what is left.
    read = source.read(95, MutBytes{out});
    REQUIRE(read.has_value());
    CHECK(*read == 5);
    CHECK(out[4] == std::byte{99});
}

TEST_CASE("a file source reports end of file as zero bytes, and keeps reading",
          "[file_image_source]")
{
    const TempFile file;
    file.write(16);
    FileImageSource source = open_or_fail(file.path());
    std::array<std::byte, 8> out{};

    for (const std::uint64_t offset : {std::uint64_t{16}, std::uint64_t{1000}}) {
        const auto at_end = source.read(offset, MutBytes{out});
        REQUIRE(at_end.has_value());
        CHECK(*at_end == 0);
    }

    // A read that reached end of file sets the stream's eofbit. The next read
    // must still work, which is what the stream's clear() is for.
    REQUIRE(source.read(12, MutBytes{out}).has_value());
    const auto again = source.read(0, MutBytes{out});
    REQUIRE(again.has_value());
    CHECK(*again == 8);
    CHECK(out[7] == std::byte{7});
}

TEST_CASE("a file that shrinks after open is a broken source", "[file_image_source]")
{
    const TempFile file;
    file.write(64);
    FileImageSource source = open_or_fail(file.path());

    // The size was taken at open(), so the source still believes in 64 bytes.
    // A platform may refuse to truncate a file another handle has open; then
    // there is nothing to test.
    std::error_code truncated;
    std::filesystem::resize_file(file.path(), 8, truncated);
    if (truncated) {
        SKIP("cannot truncate an open file here: " << truncated.message());
    }

    std::array<std::byte, 16> out{};
    const auto read = source.read(32, MutBytes{out});
    REQUIRE_FALSE(read.has_value());
    CHECK(read.error().code() == ErrorCode::InvalidArgument);
}
