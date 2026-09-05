// SPDX-License-Identifier: Apache-2.0
//
// SHA-256 is written for this project rather than taken from a library
// (ADR-0009), so its correctness rests entirely on these vectors. They are the
// published FIPS 180-4 / NIST examples, not values captured from this
// implementation -- an implementation checked against its own output proves
// only that it is deterministic.
//
// The second half is about the streaming path: hashing an image arrives in
// whatever chunks the source hands over, so the incremental result must equal
// the one-shot result at every possible split, especially around the 64-byte
// block and the 4 KiB read boundaries.

#include "image/sha256.hpp"

#include "fake_image_source.hpp"
#include "smply/image_source.hpp"
#include "smply/mcuboot_image.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using smply::ConstBytes;
using smply::ErrorCode;
using smply::Hash;
using smply::MemoryImageSource;
using smply::image::Sha256;
using smply::test::FailingImageSource;
using smply::test::ShortReadingImageSource;

namespace {

/// Lowercase hex, so a failure prints something comparable with the published
/// vector rather than a span of bytes.
template<class Bytes>
std::string hex(const Bytes& bytes)
{
    static constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::byte byte : bytes) {
        const auto value = static_cast<unsigned>(byte);
        out.push_back(kDigits[(value >> 4U) & 0xFU]);
        out.push_back(kDigits[value & 0xFU]);
    }
    return out;
}

std::vector<std::byte> bytes_of_text(std::string_view text)
{
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (const char ch : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    return out;
}

/// Filler with position-dependent content, so a misordered chunk shows up.
std::vector<std::byte> filler(std::size_t count)
{
    std::vector<std::byte> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(static_cast<std::byte>((i * 31U + 7U) & 0xFFU));
    }
    return out;
}

std::string hash_of(ConstBytes data)
{
    Sha256 hasher;
    hasher.update(data);
    return hex(hasher.finish());
}

} // namespace

// ---------------------------------------------------------------------------
// NIST vectors
// ---------------------------------------------------------------------------

TEST_CASE("SHA-256 of the empty message", "[sha256][vector]")
{
    REQUIRE(hash_of(ConstBytes{}) ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("SHA-256 of \"abc\"", "[sha256][vector]")
{
    // FIPS 180-4 appendix B.1: the one-block example.
    const auto message = bytes_of_text("abc");
    REQUIRE(hash_of(ConstBytes{message}) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("SHA-256 of the 56-byte example", "[sha256][vector]")
{
    // FIPS 180-4 appendix B.2: two blocks, and 56 bytes is exactly the length
    // at which the padding no longer fits in the first block.
    const auto message = bytes_of_text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    REQUIRE(message.size() == 56);
    REQUIRE(hash_of(ConstBytes{message}) ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("SHA-256 of the 112-byte example", "[sha256][vector]")
{
    const auto message =
        bytes_of_text("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                      "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu");
    REQUIRE(message.size() == 112);
    REQUIRE(hash_of(ConstBytes{message}) ==
            "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST_CASE("SHA-256 of a million 'a'", "[sha256][vector]")
{
    // The long NIST example. Also the only case where the 64-bit length field
    // exceeds what a single block could describe.
    const std::vector<std::byte> message(1000000, static_cast<std::byte>('a'));
    REQUIRE(hash_of(ConstBytes{message}) ==
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// ---------------------------------------------------------------------------
// The incremental path
// ---------------------------------------------------------------------------

TEST_CASE("splitting the message anywhere gives the same digest", "[sha256]")
{
    // The property the streaming path depends on. 200 bytes spans three
    // 64-byte blocks, so every kind of split is covered: inside a block, on a
    // boundary, and across several.
    const auto message = filler(200);
    const std::string expected = hash_of(ConstBytes{message});

    for (std::size_t split = 0; split <= message.size(); ++split) {
        Sha256 hasher;
        hasher.update(ConstBytes{message.data(), split});
        hasher.update(ConstBytes{message.data() + split, message.size() - split});
        INFO("split at " << split);
        REQUIRE(hex(hasher.finish()) == expected);
    }
}

TEST_CASE("a byte at a time gives the same digest", "[sha256]")
{
    const auto message = filler(130);
    const std::string expected = hash_of(ConstBytes{message});

    Sha256 hasher;
    for (const std::byte byte : message) {
        hasher.update(ConstBytes{&byte, 1});
    }
    REQUIRE(hex(hasher.finish()) == expected);
}

TEST_CASE("empty updates do not disturb the digest", "[sha256]")
{
    const auto message = bytes_of_text("abc");

    Sha256 hasher;
    hasher.update(ConstBytes{});
    hasher.update(ConstBytes{message});
    hasher.update(ConstBytes{});
    REQUIRE(hex(hasher.finish()) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("padding is exercised at every length across a block", "[sha256]")
{
    // Lengths 55/56/57 and 63/64/65 are where the padding either just fits, or
    // spills into an extra block. Sweeping the whole range covers them without
    // singling them out.
    for (std::size_t length = 0; length <= 130; ++length) {
        const auto message = filler(length);
        Sha256 whole;
        whole.update(ConstBytes{message});
        const std::string expected = hex(whole.finish());

        Sha256 halves;
        halves.update(ConstBytes{message.data(), length / 2});
        halves.update(ConstBytes{message.data() + (length / 2), length - (length / 2)});
        INFO("length " << length);
        REQUIRE(hex(halves.finish()) == expected);
    }
}

// ---------------------------------------------------------------------------
// sha256(ImageSource&)
// ---------------------------------------------------------------------------

TEST_CASE("hashing a source matches hashing its bytes", "[sha256][source]")
{
    // Sizes around the 4 KiB read chunk: one short read, exactly one chunk,
    // one byte over, and several chunks.
    for (const std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{4095},
                                   std::size_t{4096}, std::size_t{4097}, std::size_t{9000}}) {
        const auto image = filler(size);
        MemoryImageSource source{ConstBytes{image}};

        const auto digest = smply::sha256(source);
        INFO("size " << size);
        REQUIRE(digest.has_value());
        REQUIRE(hex(*digest) == hash_of(ConstBytes{image}));
    }
}

TEST_CASE("hashing an empty source is the empty digest", "[sha256][source]")
{
    MemoryImageSource source{ConstBytes{}};
    const auto digest = smply::sha256(source);
    REQUIRE(digest.has_value());
    REQUIRE(hex(*digest) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("a source that reads short mid-image is refused, not retried", "[sha256][source]")
{
    // The ImageSource contract allows a short read only at the end. Looping
    // here would let a source that returns one byte per call turn a 16 MiB
    // image into sixteen million calls.
    ShortReadingImageSource source{4096};
    const auto digest = smply::sha256(source);
    REQUIRE_FALSE(digest.has_value());
    REQUIRE(digest.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("a source that fails a read fails the hash", "[sha256][source]")
{
    // The source's own error is passed through rather than replaced: only the
    // application knows what "the file went away" means.
    FailingImageSource source{4096};
    const auto digest = smply::sha256(source);
    REQUIRE_FALSE(digest.has_value());
    REQUIRE(digest.error().code() == ErrorCode::TransportError);
}

TEST_CASE("the digest is a 32-byte Hash", "[sha256][source]")
{
    // Hash is the upload `sha` type, fixed at 32 bytes; ImageHash is the other
    // one (protocol-notes section 7). Nailed down here so a change to either
    // type has to come past this test.
    const auto image = filler(10);
    MemoryImageSource source{ConstBytes{image}};
    const auto digest = smply::sha256(source);
    REQUIRE(digest.has_value());
    static_assert(std::tuple_size<Hash>::value == 32);
    REQUIRE(digest->size() == 32);
}
