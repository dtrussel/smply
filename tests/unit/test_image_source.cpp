// SPDX-License-Identifier: Apache-2.0
//
// MemoryImageSource is small, but it defines what every other ImageSource is
// expected to do: end of image is not a failure, a short read happens only
// there, and reads may arrive in any offset order because the header parse, the
// hash and the TLV scan all walk the file differently.

#include "smply/image_source.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using smply::ConstBytes;
using smply::MemoryImageSource;
using smply::MutBytes;

namespace {

std::vector<std::byte> counted(std::size_t count)
{
    std::vector<std::byte> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(static_cast<std::byte>(i & 0xFFU));
    }
    return out;
}

} // namespace

TEST_CASE("a memory source reports the span's length", "[image_source]")
{
    const auto image = counted(100);
    const MemoryImageSource source{ConstBytes{image}};
    REQUIRE(source.size() == 100);
}

TEST_CASE("an empty memory source is legal", "[image_source]")
{
    MemoryImageSource source{ConstBytes{}};
    REQUIRE(source.size() == 0);

    std::array<std::byte, 4> out{};
    const auto read = source.read(0, MutBytes{out});
    REQUIRE(read.has_value());
    REQUIRE(*read == 0);
}

TEST_CASE("a read in the middle delivers exactly what was asked for", "[image_source]")
{
    const auto image = counted(100);
    MemoryImageSource source{ConstBytes{image}};

    std::array<std::byte, 8> out{};
    const auto read = source.read(10, MutBytes{out});
    REQUIRE(read.has_value());
    REQUIRE(*read == 8);
    REQUIRE(out[0] == std::byte{10});
    REQUIRE(out[7] == std::byte{17});
}

TEST_CASE("a read crossing the end is clamped, not refused", "[image_source]")
{
    // Short reads are legal at the end of the image. Callers that need every
    // byte check the count themselves.
    const auto image = counted(10);
    MemoryImageSource source{ConstBytes{image}};

    std::array<std::byte, 8> out{};
    const auto read = source.read(6, MutBytes{out});
    REQUIRE(read.has_value());
    REQUIRE(*read == 4);
    REQUIRE(out[0] == std::byte{6});
    REQUIRE(out[3] == std::byte{9});
}

TEST_CASE("a read at or past the end yields nothing", "[image_source]")
{
    // End of image is not an error: only the caller knows whether it expected
    // more.
    const auto image = counted(10);
    MemoryImageSource source{ConstBytes{image}};

    std::array<std::byte, 4> out{};
    for (const std::uint64_t offset :
         {std::uint64_t{10}, std::uint64_t{11}, std::uint64_t{1} << 40U}) {
        INFO("offset " << offset);
        const auto read = source.read(offset, MutBytes{out});
        REQUIRE(read.has_value());
        REQUIRE(*read == 0);
    }
}

TEST_CASE("an empty read is legal anywhere", "[image_source]")
{
    const auto image = counted(10);
    MemoryImageSource source{ConstBytes{image}};

    const auto read = source.read(5, MutBytes{});
    REQUIRE(read.has_value());
    REQUIRE(*read == 0);
}

TEST_CASE("reads may go in any offset order", "[image_source]")
{
    // The header parse reads the front, the TLV scan seeks near the back and
    // then forwards again; a source that assumed sequential access would break
    // on the second one.
    const auto image = counted(100);
    MemoryImageSource source{ConstBytes{image}};

    std::array<std::byte, 1> out{};
    for (const std::uint64_t offset : {std::uint64_t{99}, std::uint64_t{0}, std::uint64_t{50}}) {
        const auto read = source.read(offset, MutBytes{out});
        REQUIRE(read.has_value());
        REQUIRE(*read == 1);
        REQUIRE(out[0] == static_cast<std::byte>(offset & 0xFFU));
    }
}
