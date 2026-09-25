// SPDX-License-Identifier: Apache-2.0
//
// src/image/source_reader.hpp: the bounded read and the little-endian loads the
// header parser and the TLV scanner share. Both parse file content an attacker
// may have written, so the properties are pinned here directly rather than only
// through the two parsers: a read delivers every byte or fails, a read past the
// end is refused before the source is asked, and a field is assembled from bytes
// in little-endian order whatever the host is.

#include "fake_image_source.hpp"
#include "image/source_reader.hpp"
#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/image_source.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

using smply::ConstBytes;
using smply::ErrorCode;
using smply::MemoryImageSource;
using smply::MutBytes;
using smply::image::load_le16;
using smply::image::load_le32;
using smply::image::read_exact;

namespace {

/// Bytes with the high bit set in every position, so a load that sign-extends
/// a byte before shifting it shows up as the wrong value.
constexpr std::array<std::byte, 6> kHighBits{std::byte{0x81}, std::byte{0xF2}, std::byte{0xE3},
                                             std::byte{0xD4}, std::byte{0xC5}, std::byte{0xB6}};

} // namespace

TEST_CASE("load_le16 and load_le32 read little-endian at any offset", "[source_reader]")
{
    const ConstBytes bytes{kHighBits};
    CHECK(load_le16(bytes, 0) == 0xF281U);
    CHECK(load_le16(bytes, bytes.size() - 2) == 0xB6C5U);
    CHECK(load_le32(bytes, 0) == 0xD4E3F281U);
    CHECK(load_le32(bytes, bytes.size() - 4) == 0xB6C5D4E3U);
}

TEST_CASE("read_exact delivers every byte asked for", "[source_reader]")
{
    MemoryImageSource source{ConstBytes{kHighBits}};
    std::array<std::byte, 3> out{};
    REQUIRE(read_exact(source, 3, MutBytes{out}).has_value());
    CHECK(out[0] == std::byte{0xD4});
    CHECK(out[2] == std::byte{0xB6});

    // Ending exactly at the end of the image is still inside it.
    std::array<std::byte, 6> whole{};
    CHECK(read_exact(source, 0, MutBytes{whole}).has_value());
}

TEST_CASE("read_exact refuses a read past the end before asking the source", "[source_reader]")
{
    // A failing source proves the refusal happens first: asking it would
    // report its own error, not MalformedMessage.
    smply::test::FailingImageSource source{6};
    std::array<std::byte, 4> out{};

    const auto straddling = read_exact(source, 3, MutBytes{out});
    REQUIRE_FALSE(straddling.has_value());
    CHECK(straddling.error().code() == ErrorCode::MalformedMessage);

    // An offset beyond the end, where `size - offset` would wrap if it were
    // computed first.
    const auto beyond = read_exact(source, 7, MutBytes{out});
    REQUIRE_FALSE(beyond.has_value());
    CHECK(beyond.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("read_exact passes on a source's failure and refuses a short read", "[source_reader]")
{
    std::array<std::byte, 4> out{};

    smply::test::FailingImageSource failing{16, ErrorCode::TransportError};
    const auto failed = read_exact(failing, 0, MutBytes{out});
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code() == ErrorCode::TransportError);

    smply::test::ShortReadingImageSource short_reading{16};
    const auto short_read = read_exact(short_reading, 0, MutBytes{out});
    REQUIRE_FALSE(short_read.has_value());
    CHECK(short_read.error().code() == ErrorCode::InvalidArgument);
}
