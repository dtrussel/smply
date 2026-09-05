// SPDX-License-Identifier: Apache-2.0
//
// The TLV scanner is the only code in smply that indexes with offsets taken
// from a file, so most of this suite is about what it refuses. The layout it
// implements is MCUboot's own (docs/protocol-notes.md section 7): the protected
// and unprotected areas are contiguous and walked as one run, and
// ih_protect_tlv_size includes the protected area's own four-byte header.

#include "smply/mcuboot_image.hpp"

#include "fake_image_source.hpp"
#include "image_builder.hpp"
#include "smply/image_source.hpp"
#include "smply/limits.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using smply::ConstBytes;
using smply::ErrorCode;
using smply::find_image_tlv_hash;
using smply::ImageHash;
using smply::McubootImageInfo;
using smply::MemoryImageSource;
using smply::parse_mcuboot_header;
using smply::test::digest_of;
using smply::test::FailingImageSource;
using smply::test::ImageBuilder;
using smply::test::ShortReadingImageSource;

namespace Catch {
template<>
struct StringMaker<smply::ErrorCode>
{
    static std::string convert(smply::ErrorCode code)
    {
        return std::string{smply::to_string(code)};
    }
};
} // namespace Catch

namespace {

constexpr std::uint16_t kTlvSha256 = 0x10;
constexpr std::uint16_t kTlvSha384 = 0x11;
constexpr std::uint16_t kTlvSha512 = 0x12;
constexpr std::uint16_t kTlvKeyHash = 0x01;
constexpr std::uint16_t kTlvSecCnt = 0x50;

/// Parses \p image and scans it, so a test states one thing rather than three.
struct Scan
{
    std::vector<std::byte> image;
    McubootImageInfo info;

    explicit Scan(std::vector<std::byte> bytes) : image{std::move(bytes)}
    {
        const auto parsed = parse_mcuboot_header(ConstBytes{image});
        REQUIRE(parsed.has_value());
        info = *parsed;
    }

    [[nodiscard]] auto run()
    {
        MemoryImageSource source{ConstBytes{image}};
        return find_image_tlv_hash(source, info);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Finding the hash
// ---------------------------------------------------------------------------

TEST_CASE("the hash TLV is found in an unprotected-only image", "[tlv]")
{
    Scan scan{ImageBuilder{}.tlv(kTlvSha256, digest_of(32, 0xA0)).build()};

    const auto hash = scan.run();
    REQUIRE(hash.has_value());
    REQUIRE(hash->has_value());
    REQUIRE((*hash)->size() == 32);
    REQUIRE(**hash == *ImageHash::from(ConstBytes{digest_of(32, 0xA0)}));
}

TEST_CASE("the hash TLV is found past a protected area", "[tlv]")
{
    // The layout of a real signed image: protected entries first, then the
    // unprotected area's own header, then the hash. A scanner that stops at
    // the protected area's end, or that miscounts its four-byte header, finds
    // nothing here.
    Scan scan{ImageBuilder{}
                  .protected_tlv(kTlvSecCnt, digest_of(4, 0x01))
                  .tlv(kTlvKeyHash, digest_of(32, 0x10))
                  .tlv(kTlvSha256, digest_of(32, 0xB0))
                  .build()};

    const auto hash = scan.run();
    REQUIRE(hash.has_value());
    REQUIRE(hash->has_value());
    REQUIRE(**hash == *ImageHash::from(ConstBytes{digest_of(32, 0xB0)}));
}

TEST_CASE("the hash TLV is found after other unprotected entries", "[tlv]")
{
    Scan scan{ImageBuilder{}
                  .tlv(kTlvKeyHash, digest_of(32, 0x20))
                  .tlv(kTlvSha256, digest_of(32, 0xC0))
                  .tlv(0x22, digest_of(64, 0x30)) // a signature, after the hash
                  .build()};

    const auto hash = scan.run();
    REQUIRE(hash.has_value());
    REQUIRE(hash->has_value());
    REQUIRE(**hash == *ImageHash::from(ConstBytes{digest_of(32, 0xC0)}));
}

TEST_CASE("SHA-384 and SHA-512 TLVs are found at their own lengths", "[tlv]")
{
    // The reason ImageHash carries a length: a bootloader built for SHA-512
    // reports 64 bytes, and the file's TLV has to match it.
    SECTION("SHA-384")
    {
        Scan scan{ImageBuilder{}.tlv(kTlvSha384, digest_of(48, 0x40)).build()};
        const auto hash = scan.run();
        REQUIRE(hash.has_value());
        REQUIRE(hash->has_value());
        REQUIRE((*hash)->size() == 48);
    }
    SECTION("SHA-512")
    {
        Scan scan{ImageBuilder{}.tlv(kTlvSha512, digest_of(64, 0x50)).build()};
        const auto hash = scan.run();
        REQUIRE(hash.has_value());
        REQUIRE(hash->has_value());
        REQUIRE((*hash)->size() == 64);
    }
}

TEST_CASE("an image with no hash TLV is not an error", "[tlv]")
{
    // Absent is an answer, not a failure -- correlation is corroboration, not
    // a gate (ADR-0009).
    Scan scan{ImageBuilder{}.tlv(kTlvKeyHash, digest_of(32, 0x60)).build()};

    const auto hash = scan.run();
    REQUIRE(hash.has_value());
    REQUIRE_FALSE(hash->has_value());
}

TEST_CASE("an empty unprotected area is not an error", "[tlv]")
{
    Scan scan{ImageBuilder{}.build()};

    const auto hash = scan.run();
    REQUIRE(hash.has_value());
    REQUIRE_FALSE(hash->has_value());
}

TEST_CASE("an encrypted image is not scanned at all", "[tlv]")
{
    // The bytes on the device are not the bytes in the file, so the file's hash
    // could never match what image-state reports (A13). The hash TLV is present
    // here and still must not be returned.
    Scan scan{ImageBuilder{}.flags(0x04).tlv(kTlvSha256, digest_of(32, 0x70)).build()};
    REQUIRE(scan.info.encrypted);

    const auto hash = scan.run();
    REQUIRE(hash.has_value());
    REQUIRE_FALSE(hash->has_value());
}

// ---------------------------------------------------------------------------
// Malformed areas
// ---------------------------------------------------------------------------

TEST_CASE("a protected size disagreeing with its own header is refused", "[tlv][hostile]")
{
    // MCUboot requires ih_protect_tlv_size == the protected area's it_tlv_tot
    // exactly, and refuses the image otherwise. Only the header field is
    // changed here: overriding both would leave them agreeing, and the test
    // would pass without ever reaching the check it names.
    ImageBuilder builder;
    builder.protected_tlv(kTlvSecCnt, digest_of(4, 0x01)).tlv(kTlvSha256, digest_of(32, 0x80));
    const std::uint16_t real = builder.computed_protected_total();
    Scan scan{builder.header_protected_size(static_cast<std::uint16_t>(real + 4)).build()};

    const auto hash = scan.run();
    REQUIRE_FALSE(hash.has_value());
    REQUIRE(hash.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("a protected area smaller than its own header is refused", "[tlv][hostile]")
{
    // it_tlv_tot counts the four-byte info header, so a protected area of two
    // bytes cannot exist. Both fields are set, because here they agree -- the
    // value itself is the impossibility.
    Scan scan{ImageBuilder{}
                  .emit_protected_area(true)
                  .protected_area_total(2)
                  .header_protected_size(2)
                  .tlv(kTlvSha256, digest_of(32, 0x81))
                  .build()};

    const auto hash = scan.run();
    REQUIRE_FALSE(hash.has_value());
    REQUIRE(hash.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("a protected size with no protected area is refused", "[tlv][hostile]")
{
    // The header claims a protected area; the bytes hold only the unprotected
    // one. The two must agree.
    Scan scan{ImageBuilder{}.header_protected_size(8).tlv(kTlvSha256, digest_of(32, 0x82)).build()};

    const auto hash = scan.run();
    REQUIRE_FALSE(hash.has_value());
    REQUIRE(hash.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("a protected area with no unprotected area after it is refused", "[tlv][hostile]")
{
    // Truncating away the unprotected area's header leaves the read past the
    // end of the file.
    ImageBuilder builder;
    builder.protected_tlv(kTlvSecCnt, digest_of(4, 0x01));
    auto image = builder.build();
    image.resize(image.size() - 4);
    Scan scan{std::move(image)};

    const auto hash = scan.run();
    REQUIRE_FALSE(hash.has_value());
    REQUIRE(hash.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("a second area that is not the unprotected one is refused", "[tlv][hostile]")
{
    // The protected area is well formed, but what follows it is not an
    // unprotected TLV area at all.
    Scan scan{ImageBuilder{}
                  .protected_tlv(kTlvSecCnt, digest_of(4, 0x01))
                  .unprotected_area_magic(0x1234)
                  .tlv(kTlvSha256, digest_of(32, 0x83))
                  .build()};

    const auto hash = scan.run();
    REQUIRE_FALSE(hash.has_value());
    REQUIRE(hash.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("no TLV area after the body is refused", "[tlv][hostile]")
{
    Scan scan{ImageBuilder{}.unprotected_area_magic(0x1234).build()};

    const auto hash = scan.run();
    REQUIRE_FALSE(hash.has_value());
    REQUIRE(hash.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("an area smaller than its own header is refused", "[tlv][hostile]")
{
    // it_tlv_tot counts the four-byte info header, so anything below four is
    // impossible.
    for (const std::uint16_t total : {std::uint16_t{0}, std::uint16_t{3}}) {
        Scan scan{ImageBuilder{}.unprotected_total(total).build()};
        INFO("it_tlv_tot " << total);
        const auto hash = scan.run();
        REQUIRE_FALSE(hash.has_value());
        REQUIRE(hash.error().code() == ErrorCode::MalformedMessage);
    }
}

TEST_CASE("an area claiming to extend past the file is refused", "[tlv][hostile]")
{
    // it_tlv_tot is the file's own claim about how far it reaches. Bounded
    // against the actual length before a single entry is read.
    Scan scan{ImageBuilder{}.tlv(kTlvSha256, digest_of(32, 0x90)).unprotected_total(4096).build()};

    const auto hash = scan.run();
    REQUIRE_FALSE(hash.has_value());
    REQUIRE(hash.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("a TLV whose length overruns the area is refused", "[tlv][hostile]")
{
    // The entry header fits, its payload does not -- the case that would read
    // past the end if the length were trusted.
    ImageBuilder builder;
    builder.tlv(kTlvKeyHash, digest_of(8, 0x00));
    auto image = builder.build();
    // The last entry's it_len is four bytes back from the end of its payload.
    const std::size_t length_at = image.size() - 8 - 2;
    image[length_at] = std::byte{0xFF};
    image[length_at + 1] = std::byte{0x00};
    Scan scan{std::move(image)};

    const auto hash = scan.run();
    REQUIRE_FALSE(hash.has_value());
    REQUIRE(hash.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("a truncated entry header is refused", "[tlv][hostile]")
{
    // An area whose total leaves two bytes of a four-byte entry header.
    ImageBuilder builder;
    builder.tlv(kTlvKeyHash, digest_of(8, 0x00));
    Scan scan{builder.unprotected_total(6).build()};

    const auto hash = scan.run();
    REQUIRE_FALSE(hash.has_value());
    REQUIRE(hash.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("two hash TLVs are refused rather than resolved", "[tlv][hostile]")
{
    // What the device reports as ImageError::TlvMultipleHashesFound. Choosing
    // one would be guessing which image the file is.
    Scan scan{ImageBuilder{}
                  .tlv(kTlvSha256, digest_of(32, 0xA0))
                  .tlv(kTlvSha256, digest_of(32, 0xB0))
                  .build()};

    const auto hash = scan.run();
    REQUIRE_FALSE(hash.has_value());
    REQUIRE(hash.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("hash TLVs of different types still count as two", "[tlv][hostile]")
{
    Scan scan{ImageBuilder{}
                  .tlv(kTlvSha256, digest_of(32, 0xA0))
                  .tlv(kTlvSha512, digest_of(64, 0xB0))
                  .build()};

    const auto hash = scan.run();
    REQUIRE_FALSE(hash.has_value());
    REQUIRE(hash.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("a hash TLV of the wrong length for its type is refused", "[tlv][hostile]")
{
    // A 0x10 entry is SHA-256 and is 32 bytes. Accepting a shorter one would
    // hand the caller a truncated digest that silently fails to match.
    Scan scan{ImageBuilder{}.tlv(kTlvSha256, digest_of(16, 0xC0)).build()};

    const auto hash = scan.run();
    REQUIRE_FALSE(hash.has_value());
    REQUIRE(hash.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("a zero-length TLV advances and terminates", "[tlv][hostile]")
{
    // The "infinite loop" this scanner is often written to guard against
    // cannot happen: every step consumes at least the four-byte entry header,
    // so the offset strictly increases whatever the length says.
    Scan scan{ImageBuilder{}
                  .tlv(kTlvKeyHash, {})
                  .tlv(kTlvKeyHash, {})
                  .tlv(kTlvSha256, digest_of(32, 0xD0))
                  .build()};

    const auto hash = scan.run();
    REQUIRE(hash.has_value());
    REQUIRE(hash->has_value());
    REQUIRE(**hash == *ImageHash::from(ConstBytes{digest_of(32, 0xD0)}));
}

TEST_CASE("more entries than the cap is a bounded failure", "[tlv][hostile]")
{
    // The cap bounds the work a crafted file can demand. Zero-length entries
    // are the cheapest way to demand a lot of it.
    ImageBuilder builder;
    for (std::size_t i = 0; i <= smply::limits::kMaxImageTlvs; ++i) {
        builder.tlv(kTlvKeyHash, {});
    }
    Scan scan{builder.build()};

    const auto hash = scan.run();
    REQUIRE_FALSE(hash.has_value());
    REQUIRE(hash.error().code() == ErrorCode::MalformedMessage);
}

TEST_CASE("exactly the cap of entries is accepted", "[tlv]")
{
    // The boundary P8 got wrong in the CBOR reader: a cap of N must admit N.
    ImageBuilder builder;
    for (std::size_t i = 0; i + 1 < smply::limits::kMaxImageTlvs; ++i) {
        builder.tlv(kTlvKeyHash, {});
    }
    builder.tlv(kTlvSha256, digest_of(32, 0xE0));
    Scan scan{builder.build()};

    const auto hash = scan.run();
    REQUIRE(hash.has_value());
    REQUIRE(hash->has_value());
}

TEST_CASE("a source that fails a read fails the scan", "[tlv][hostile]")
{
    // The scan's own bounds checks cannot catch a source that simply breaks,
    // so its error is passed through unchanged.
    const auto parsed = parse_mcuboot_header(ConstBytes{ImageBuilder{}.build()});
    REQUIRE(parsed.has_value());

    FailingImageSource source{4096};
    const auto hash = find_image_tlv_hash(source, *parsed);
    REQUIRE_FALSE(hash.has_value());
    REQUIRE(hash.error().code() == ErrorCode::TransportError);
}

TEST_CASE("a source that reads short is refused, not retried", "[tlv][hostile]")
{
    // A short read away from the end of the image breaks the ImageSource
    // contract. Reported as caller misuse rather than looped on.
    const auto parsed = parse_mcuboot_header(ConstBytes{ImageBuilder{}.build()});
    REQUIRE(parsed.has_value());

    ShortReadingImageSource source{4096};
    const auto hash = find_image_tlv_hash(source, *parsed);
    REQUIRE_FALSE(hash.has_value());
    REQUIRE(hash.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("a body size pointing past the file is refused", "[tlv][hostile]")
{
    // The TLV area starts at header_size + image_size, both of which come from
    // the file. Bounded before the first read.
    auto image = ImageBuilder{}.tlv(kTlvSha256, digest_of(32, 0xF0)).build();
    const auto parsed = parse_mcuboot_header(ConstBytes{image});
    REQUIRE(parsed.has_value());
    McubootImageInfo info = *parsed;
    info.image_size = 100000;

    MemoryImageSource source{ConstBytes{image}};
    const auto hash = find_image_tlv_hash(source, info);
    REQUIRE_FALSE(hash.has_value());
    REQUIRE(hash.error().code() == ErrorCode::MalformedMessage);
}
