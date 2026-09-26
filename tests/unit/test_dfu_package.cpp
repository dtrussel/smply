// SPDX-License-Identifier: Apache-2.0
//
// The multi-image DFU package reader (support/dfu_package/, ADR-0021): the
// stored-zip lister, the bounded JSON reader, and the package they make up.
//
// The archives are written byte by byte by tests/support/zip_builder.hpp and
// the images by image_builder.hpp, both independent of the code under test.
// The package is untrusted input, so most of this file is refusals: each bound,
// each structural check, and each claim of the manifest checked against the
// image it describes. fuzz_dfu_package covers what a table of cases cannot.

#include "image_builder.hpp"
#include "zip_builder.hpp"

#include "dfu_package/dfu_package.hpp"
#include "dfu_package/json.hpp"
#include "dfu_package/stored_zip.hpp"
#include "smply/error.hpp"
#include "smply/mcuboot_image.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using smply::ConstBytes;
using smply::ErrorCode;
using smply::ImageVersion;
using smply::dfu_package::DfuPackage;
using smply::dfu_package::ImageDependency;
using smply::dfu_package::JsonValue;
using smply::dfu_package::parse_json;
using smply::dfu_package::read_package;
using smply::dfu_package::read_stored_zip;
using smply::test::ImageBuilder;
using smply::test::ZipBuilder;

namespace {

[[nodiscard]] std::vector<std::byte> bytes_of(std::string_view text)
{
    std::vector<std::byte> out;
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

[[nodiscard]] bool same(ConstBytes actual, const std::vector<std::byte>& expected)
{
    return actual.size() == expected.size() &&
           std::equal(actual.begin(), actual.end(), expected.begin());
}

template<class T>
[[nodiscard]] ErrorCode code_of(const smply::Result<T>& result)
{
    return result.has_value() ? ErrorCode::Ok : result.error().code();
}

/// A signed-looking image: header, body and a SHA-256 TLV.
[[nodiscard]] std::vector<std::byte> image(std::uint8_t major, std::uint8_t minor = 0)
{
    return ImageBuilder{}
        .version(major, minor, 0, 0)
        .body(64)
        .tlv(0x10, std::vector<std::byte>(32))
        .build();
}

/// One `files[]` entry, as generate_zip.py writes it: image_index is a string.
[[nodiscard]] std::string file_entry(const std::string& name, std::size_t size,
                                     const std::string& index, const std::string& version)
{
    return R"({"board": "stm32h573", "soc": "stm32h5", "image_index": ")" + index +
           R"(", "slot_index_primary": "1", "slot_index_secondary": "2", "version_MCUBOOT": ")" +
           version + R"(", "load_address": 134217728, "size": )" + std::to_string(size) +
           R"(, "file": ")" + name + R"(", "modtime": 1758800000})";
}

/// The two-image package of docs/multi-image.md, listed out of order.
struct TwoImagePackage
{
    std::vector<std::byte> app = image(2);
    std::vector<std::byte> radio = image(6);
    std::string manifest =
        R"({"format-version": 1, "time": 1758800000, "name": "h5 and radio", "files": [)" +
        file_entry("radio.bin", radio.size(), "1", "6.0.0+0") + ", " +
        file_entry("app.bin", app.size(), "0", "2.0.0") + "]}";

    [[nodiscard]] std::vector<std::byte> build() const
    {
        return ZipBuilder{}
            .add("app.bin", app)
            .add("radio.bin", radio)
            .add("manifest.json", manifest)
            .build();
    }
};

/// A package of one image and \p manifest.
[[nodiscard]] std::vector<std::byte> one_image_package(const std::vector<std::byte>& file,
                                                       const std::string& manifest)
{
    return ZipBuilder{}.add("app.bin", file).add("manifest.json", manifest).build();
}

} // namespace

// --- The stored zip ----------------------------------------------------------

TEST_CASE("crc32 is zip's CRC-32", "[dfu_package][zip]")
{
    CHECK(smply::dfu_package::crc32(ConstBytes{bytes_of("123456789")}) == 0xCBF43926U);
    CHECK(smply::dfu_package::crc32(ConstBytes{}) == 0U);
}

TEST_CASE("a stored zip lists its files as views into the archive", "[dfu_package][zip]")
{
    const std::vector<std::byte> archive = ZipBuilder{}
                                               .add("a.bin", bytes_of("alpha"))
                                               .add("dir/", std::vector<std::byte>{})
                                               .add("b.bin", bytes_of("beta"))
                                               .comment("a comment with PK\x05\x06 in it")
                                               .build();
    const auto entries = read_stored_zip(ConstBytes{archive});
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() == 2); // the directory is left out
    CHECK((*entries)[0].name == "a.bin");
    CHECK(same((*entries)[0].data, bytes_of("alpha")));
    CHECK((*entries)[1].name == "b.bin");
    CHECK((*entries)[0].data.data() >= archive.data());
    CHECK((*entries)[1].data.data() < archive.data() + archive.size());
}

TEST_CASE("an empty zip has no files", "[dfu_package][zip]")
{
    const std::vector<std::byte> archive = ZipBuilder{}.build();
    const auto entries = read_stored_zip(ConstBytes{archive});
    REQUIRE(entries.has_value());
    CHECK(entries->empty());
}

TEST_CASE("a compressed or encrypted entry is refused, and says why", "[dfu_package][zip]")
{
    const auto refused = [](ZipBuilder::Entry entry) {
        const std::vector<std::byte> archive = ZipBuilder{}.add(std::move(entry)).build();
        return code_of(read_stored_zip(ConstBytes{archive}));
    };
    const auto deflated = [] {
        return ZipBuilder::Entry{.name = "a", .data = bytes_of("x"), .method = 8};
    };
    CHECK(refused(deflated()) == ErrorCode::InvalidArgument);
    CHECK(refused(ZipBuilder::Entry{.name = "a", .data = bytes_of("x"), .method = 12}) ==
          ErrorCode::InvalidArgument);
    CHECK(refused(ZipBuilder::Entry{.name = "a", .data = bytes_of("x"), .flags = 1}) ==
          ErrorCode::InvalidArgument);

    const std::vector<std::byte> archive = ZipBuilder{}.add(deflated()).build();
    const auto result = read_stored_zip(ConstBytes{archive});
    REQUIRE_FALSE(result.has_value());
    CHECK(std::string_view{result.error().where()}.find("deflated") != std::string_view::npos);
}

TEST_CASE("a damaged entry is malformed", "[dfu_package][zip]")
{
    const auto verdict = [](ZipBuilder::Entry entry) {
        const std::vector<std::byte> archive = ZipBuilder{}.add(std::move(entry)).build();
        return code_of(read_stored_zip(ConstBytes{archive}));
    };
    CHECK(verdict({.name = "a", .data = bytes_of("x"), .crc = 1U}) == ErrorCode::MalformedMessage);
    CHECK(verdict({.name = "a", .data = bytes_of("x"), .uncompressed = 2U}) ==
          ErrorCode::MalformedMessage);
    CHECK(verdict({.name = "a", .data = bytes_of("x"), .local_name = std::string{"b"}}) ==
          ErrorCode::MalformedMessage);
    CHECK(verdict({.name = "", .data = bytes_of("x")}) == ErrorCode::MalformedMessage);

    const std::vector<std::byte> twice =
        ZipBuilder{}.add("a", bytes_of("x")).add("a", bytes_of("y")).build();
    CHECK(code_of(read_stored_zip(ConstBytes{twice})) == ErrorCode::MalformedMessage);
}

TEST_CASE("the end record is bounded and checked", "[dfu_package][zip]")
{
    const std::vector<std::byte> too_short = bytes_of("PK");
    CHECK(code_of(read_stored_zip(ConstBytes{too_short})) == ErrorCode::MalformedMessage);

    const std::vector<std::byte> not_zip(100, std::byte{0x41});
    CHECK(code_of(read_stored_zip(ConstBytes{not_zip})) == ErrorCode::MalformedMessage);

    const std::vector<std::byte> zip64 = ZipBuilder{}.entry_count(0xFFFF).build();
    CHECK(code_of(read_stored_zip(ConstBytes{zip64})) == ErrorCode::InvalidArgument);

    // More entries claimed than the directory holds.
    const std::vector<std::byte> overclaimed =
        ZipBuilder{}.add("a", bytes_of("x")).entry_count(2).build();
    CHECK(code_of(read_stored_zip(ConstBytes{overclaimed})) == ErrorCode::MalformedMessage);

    ZipBuilder many;
    for (std::size_t i = 0; i <= smply::dfu_package::kMaxZipEntries; ++i) {
        many.add("f" + std::to_string(i), bytes_of("x"));
    }
    const std::vector<std::byte> crowded = many.build();
    CHECK(code_of(read_stored_zip(ConstBytes{crowded})) == ErrorCode::MessageTooLarge);
}

TEST_CASE("every truncation and every flipped byte fails cleanly", "[dfu_package][zip]")
{
    // Not a proof -- fuzz_dfu_package is that -- but every offset of a real
    // archive, under the sanitizers, and a result that is either a value or an
    // error each time.
    const std::vector<std::byte> archive =
        ZipBuilder{}.add("a.bin", bytes_of("alpha")).add("b.bin", bytes_of("beta")).build();
    for (std::size_t length = 0; length < archive.size(); ++length) {
        CHECK_FALSE(read_stored_zip(ConstBytes{archive}.first(length)).has_value());
    }
    for (std::size_t at = 0; at < archive.size(); ++at) {
        std::vector<std::byte> damaged = archive;
        damaged[at] ^= std::byte{0xFF};
        const auto result = read_stored_zip(ConstBytes{damaged});
        if (result.has_value()) {
            // Only a field the reader does not use can change unnoticed.
            CHECK(result->size() <= 2);
        }
    }
}

// --- JSON --------------------------------------------------------------------

TEST_CASE("json reads a manifest's shapes", "[dfu_package][json]")
{
    const auto value = parse_json(
        R"( {"a": [1, -2.5e3, true, false, null], "b": {"c": "d"}, "n": 18446744073709551615} )");
    REQUIRE(value.has_value());
    CHECK(value->kind == JsonValue::Kind::Object);
    const JsonValue* a = value->find("a");
    REQUIRE(a != nullptr);
    REQUIRE(a->items.size() == 5);
    CHECK(a->items[0].as_uint() == 1U);
    CHECK_FALSE(a->items[1].as_uint().has_value());
    CHECK(a->items[1].text == "-2.5e3");
    CHECK(a->items[2].boolean);
    CHECK_FALSE(a->items[3].boolean);
    CHECK(a->items[4].kind == JsonValue::Kind::Null);
    CHECK(value->find("b")->find("c")->text == "d");
    CHECK(value->find("n")->as_uint() == 18446744073709551615U);
    CHECK(value->find("missing") == nullptr);
}

TEST_CASE("json unescapes strings to UTF-8", "[dfu_package][json]")
{
    const auto value = parse_json(R"(["a\"\\\/\b\f\n\r\t", "\u00e9\u20ac", "\ud83d\ude00"])");
    REQUIRE(value.has_value());
    CHECK(value->items[0].text == "a\"\\/\b\f\n\r\t");
    CHECK(value->items[1].text == "\xC3\xA9\xE2\x82\xAC");
    CHECK(value->items[2].text == "\xF0\x9F\x98\x80");
}

TEST_CASE("json refuses what is not JSON", "[dfu_package][json]")
{
    for (const char* text : {"",
                             "   ",
                             "[1,]",
                             "{\"a\":1,}",
                             "{a:1}",
                             "01",
                             "1.",
                             "1e",
                             "-",
                             "tru",
                             "[1 2]",
                             R"({"a" 1})",
                             R"("abc)",
                             R"("\x")",
                             R"("\u12")",
                             R"("\udc00")",
                             R"("\ud800")",
                             R"("\ud800\u0041")",
                             "\"a\tb\"",
                             R"({"a":1,"a":2})",
                             "[] []",
                             "[",
                             "{",
                             "{\"a\":",
                             "\"\\"}) {
        CAPTURE(text);
        CHECK(code_of(parse_json(text)) == ErrorCode::MalformedMessage);
    }
}

TEST_CASE("json bounds depth, count, length and size", "[dfu_package][json]")
{
    using namespace smply::dfu_package;
    const std::string deepest = std::string(kMaxJsonDepth, '[') + std::string(kMaxJsonDepth, ']');
    CHECK(parse_json(deepest).has_value());
    const std::string deeper =
        std::string(kMaxJsonDepth + 1, '[') + std::string(kMaxJsonDepth + 1, ']');
    CHECK(code_of(parse_json(deeper)) == ErrorCode::MessageTooLarge);

    std::string many = "[";
    for (std::size_t i = 0; i < kMaxJsonValues; ++i) {
        many += "0,";
    }
    many += "0]";
    CHECK(code_of(parse_json(many)) == ErrorCode::MessageTooLarge);

    const std::string long_string = "\"" + std::string(kMaxJsonString + 1, 'x') + "\"";
    CHECK(code_of(parse_json(long_string)) == ErrorCode::MessageTooLarge);
    const std::string long_number(kMaxJsonString + 1, '1');
    CHECK(code_of(parse_json(long_number)) == ErrorCode::MessageTooLarge);

    const std::string huge = "\"" + std::string(kMaxJsonSize, 'x') + "\"";
    CHECK(code_of(parse_json(huge)) == ErrorCode::MessageTooLarge);
}

TEST_CASE("as_uint is only for integers that fit", "[dfu_package][json]")
{
    const auto value = parse_json(R"([18446744073709551616, -1, 1.0, "1"])");
    REQUIRE(value.has_value());
    for (const JsonValue& item : value->items) {
        CHECK_FALSE(item.as_uint().has_value());
    }
}

// --- The package -------------------------------------------------------------

TEST_CASE("an nRF Connect SDK-style package reads as its images, by index", "[dfu_package]")
{
    const TwoImagePackage source;
    const std::vector<std::byte> archive = source.build();
    const auto package = read_package(ConstBytes{archive});
    REQUIRE(package.has_value());

    CHECK(package->format_version == 1U);
    CHECK(package->name == "h5 and radio");
    REQUIRE(package->images.size() == 2);
    const auto& app = package->images[0];
    const auto& radio = package->images[1];
    CHECK(app.image == 0);
    CHECK(app.file == "app.bin");
    CHECK(same(app.bytes, source.app));
    CHECK(app.header.version == ImageVersion{.major = 2});
    CHECK(app.board == "stm32h573");
    CHECK(app.soc == "stm32h5");
    CHECK(app.version == "2.0.0");
    CHECK(app.dependencies.empty());
    CHECK(radio.image == 1);
    CHECK(same(radio.bytes, source.radio));
}

TEST_CASE("image_index may be a number, and a lone file may omit it", "[dfu_package]")
{
    const std::vector<std::byte> app = image(2);
    const std::string size = std::to_string(app.size());

    const std::vector<std::byte> numeric = one_image_package(
        app, R"({"files": [{"file": "app.bin", "image_index": 3, "size": )" + size + "}]}");
    const auto three = read_package(ConstBytes{numeric});
    REQUIRE(three.has_value());
    CHECK(three->images[0].image == 3);
    CHECK_FALSE(three->format_version.has_value());

    const std::vector<std::byte> lone =
        one_image_package(app, R"({"files": [{"file": "app.bin"}]})");
    const auto zero = read_package(ConstBytes{lone});
    REQUIRE(zero.has_value());
    CHECK(zero->images[0].image == 0);
    CHECK_FALSE(zero->images[0].version.has_value());
}

TEST_CASE("image_index is checked", "[dfu_package]")
{
    const std::vector<std::byte> app = image(2);
    const auto with_index = [&app](const std::string& index) {
        const std::vector<std::byte> archive = one_image_package(
            app, R"({"files": [{"file": "app.bin", "image_index": )" + index + "}]}");
        return code_of(read_package(ConstBytes{archive}));
    };
    CHECK(with_index(R"("255")") == ErrorCode::Ok);
    CHECK(with_index(R"("256")") == ErrorCode::InvalidArgument);
    CHECK(with_index(R"("x")") == ErrorCode::MalformedMessage);
    CHECK(with_index(R"("")") == ErrorCode::MalformedMessage);
    CHECK(with_index("-1") == ErrorCode::MalformedMessage);
    CHECK(with_index("true") == ErrorCode::MalformedMessage);

    // With two files, each must say which image it is.
    const std::vector<std::byte> archive =
        ZipBuilder{}
            .add("a.bin", app)
            .add("b.bin", app)
            .add("manifest.json",
                 std::string_view{R"({"files": [{"file": "a.bin", "image_index": "0"},
                                                {"file": "b.bin"}]})"})
            .build();
    CHECK(code_of(read_package(ConstBytes{archive})) == ErrorCode::MalformedMessage);
}

TEST_CASE("two files for one image are refused", "[dfu_package]")
{
    // What a direct-XIP build writes: one file per slot of the same image.
    // They are alternatives, not a set to send.
    const std::vector<std::byte> app = image(2);
    const std::vector<std::byte> archive =
        ZipBuilder{}
            .add("slot0.bin", app)
            .add("slot1.bin", app)
            .add("manifest.json",
                 std::string_view{R"({"files": [{"file": "slot0.bin", "image_index": "0"},
                                                {"file": "slot1.bin", "image_index": "0"}]})"})
            .build();
    CHECK(code_of(read_package(ConstBytes{archive})) == ErrorCode::InvalidArgument);
}

TEST_CASE("the manifest must agree with the zip and with the images", "[dfu_package]")
{
    const std::vector<std::byte> app = image(2, 1);
    const std::string size = std::to_string(app.size());
    const auto verdict = [&app](const std::string& manifest) {
        return code_of(read_package(ConstBytes{one_image_package(app, manifest)}));
    };

    CHECK(verdict(R"({"files": [{"file": "app.bin", "version_MCUBOOT": "2.1.0+0"}]})") ==
          ErrorCode::Ok);
    CHECK(verdict(R"({"files": [{"file": "app.bin", "version_MCUBOOT": "2.2.0"}]})") ==
          ErrorCode::MalformedMessage);
    CHECK(verdict(R"({"files": [{"file": "app.bin", "version_MCUBOOT": "two"}]})") ==
          ErrorCode::MalformedMessage);
    CHECK(verdict(R"({"files": [{"file": "app.bin", "size": 1}]})") == ErrorCode::MalformedMessage);
    CHECK(verdict(R"({"files": [{"file": "missing.bin"}]})") == ErrorCode::MalformedMessage);
    CHECK(verdict(R"({"files": [{"size": )" + size + "}]}") == ErrorCode::MalformedMessage);
    CHECK(verdict(R"({"files": [{"file": 7}]})") == ErrorCode::MalformedMessage);
    CHECK(verdict(R"({"files": [{"file": "app.bin", "board": 7}]})") ==
          ErrorCode::MalformedMessage);
    CHECK(verdict(R"({"files": ["app.bin"]})") == ErrorCode::MalformedMessage);
    CHECK(verdict(R"({"files": []})") == ErrorCode::MalformedMessage);
    CHECK(verdict(R"({"files": {}})") == ErrorCode::MalformedMessage);
    CHECK(verdict(R"({})") == ErrorCode::MalformedMessage);
    CHECK(verdict(R"([])") == ErrorCode::MalformedMessage);
    CHECK(verdict(R"({"files": [{"file": "app.bin"}], "format-version": "1"})") ==
          ErrorCode::MalformedMessage);
    CHECK(verdict(R"({"files": [{"file": "app.bin"}], "name": 1})") == ErrorCode::MalformedMessage);
    CHECK(verdict("not json") == ErrorCode::MalformedMessage);
}

TEST_CASE("a package without a manifest, or not a package at all, is refused", "[dfu_package]")
{
    const std::vector<std::byte> bare = ZipBuilder{}.add("app.bin", image(2)).build();
    CHECK(code_of(read_package(ConstBytes{bare})) == ErrorCode::MalformedMessage);

    // zephyr.bin, not zephyr.signed.bin: the header's magic is wrong.
    const std::vector<std::byte> unsigned_image =
        ImageBuilder{}.magic(0x12345678U).body(64).build();
    const std::vector<std::byte> archive =
        one_image_package(unsigned_image, R"({"files": [{"file": "app.bin"}]})");
    CHECK(code_of(read_package(ConstBytes{archive})) == ErrorCode::InvalidArgument);

    const std::vector<std::byte> tiny = bytes_of("short");
    const std::vector<std::byte> stub =
        one_image_package(tiny, R"({"files": [{"file": "app.bin"}]})");
    CHECK(code_of(read_package(ConstBytes{stub})) == ErrorCode::MalformedMessage);

    const std::vector<std::byte> deflated =
        ZipBuilder{}
            .add(ZipBuilder::Entry{.name = "manifest.json", .data = bytes_of("{}"), .method = 8})
            .build();
    CHECK(code_of(read_package(ConstBytes{deflated})) == ErrorCode::InvalidArgument);
}

// --- Dependency TLVs ---------------------------------------------------------

namespace {

/// `struct image_dependency`: image id, three bytes of padding, and a version.
[[nodiscard]] std::vector<std::byte> dependency(std::uint8_t image, std::uint8_t major,
                                                std::uint8_t minor, std::uint16_t revision,
                                                std::uint32_t build)
{
    return {std::byte{image},
            std::byte{0},
            std::byte{0},
            std::byte{0},
            std::byte{major},
            std::byte{minor},
            static_cast<std::byte>(revision & 0xFFU),
            static_cast<std::byte>(revision >> 8U),
            static_cast<std::byte>(build & 0xFFU),
            static_cast<std::byte>((build >> 8U) & 0xFFU),
            static_cast<std::byte>((build >> 16U) & 0xFFU),
            static_cast<std::byte>(build >> 24U)};
}

[[nodiscard]] smply::Result<std::vector<ImageDependency>>
dependencies_of(const std::vector<std::byte>& file)
{
    const auto header =
        smply::parse_mcuboot_header(ConstBytes{file}.first(smply::kMcubootHeaderSize));
    REQUIRE(header.has_value());
    return smply::dfu_package::read_dependencies(ConstBytes{file}, *header);
}

} // namespace

TEST_CASE("dependency TLVs are reported, from either area", "[dfu_package][tlv]")
{
    const std::vector<std::byte> file = ImageBuilder{}
                                            .version(2, 0, 0, 0)
                                            .body(64)
                                            .protected_tlv(0x40, dependency(1, 6, 0, 3, 7))
                                            .protected_tlv(0x41, std::vector<std::byte>(4))
                                            .tlv(0x10, std::vector<std::byte>(32))
                                            .tlv(0x40, dependency(2, 1, 2, 3, 0))
                                            .build();
    const auto found = dependencies_of(file);
    REQUIRE(found.has_value());
    REQUIRE(found->size() == 2);
    CHECK((*found)[0] ==
          ImageDependency{.image = 1,
                          .minimum = ImageVersion{.major = 6, .revision = 3, .build = 7}});
    CHECK((*found)[1] ==
          ImageDependency{.image = 2,
                          .minimum = ImageVersion{.major = 1, .minor = 2, .revision = 3}});

    // And through the package, which reports them per image.
    const std::vector<std::byte> archive =
        one_image_package(file, R"({"files": [{"file": "app.bin"}]})");
    const auto package = read_package(ConstBytes{archive});
    REQUIRE(package.has_value());
    CHECK(package->images[0].dependencies == *found);
}

TEST_CASE("a broken TLV area is refused", "[dfu_package][tlv]")
{
    const auto verdict = [](const ImageBuilder& builder) {
        return code_of(dependencies_of(builder.build()));
    };
    const auto base = [] { return ImageBuilder{}.body(64).tlv(0x10, std::vector<std::byte>(32)); };

    CHECK(verdict(base()) == ErrorCode::Ok);
    CHECK(verdict(base().tlv(0x40, std::vector<std::byte>(8))) == ErrorCode::MalformedMessage);
    CHECK(verdict(base().unprotected_total(0xFFFF)) == ErrorCode::MalformedMessage);
    CHECK(verdict(base().unprotected_total(2)) == ErrorCode::MalformedMessage);
    CHECK(verdict(base().unprotected_area_magic(0x1234)) == ErrorCode::MalformedMessage);
    CHECK(verdict(base().unprotected_total(4 + 36 + 2)) == ErrorCode::MalformedMessage);
    CHECK(verdict(base().header_protected_size(8)) == ErrorCode::MalformedMessage);

    const auto protected_base = [&base] {
        return base().protected_tlv(0x40, dependency(1, 1, 0, 0, 0));
    };
    CHECK(verdict(protected_base()) == ErrorCode::Ok);
    CHECK(verdict(protected_base().header_protected_size(4)) == ErrorCode::MalformedMessage);
    CHECK(verdict(protected_base().protected_area_total(2).header_protected_size(2)) ==
          ErrorCode::MalformedMessage);
    CHECK(verdict(protected_base().unprotected_area_magic(0x6908)) == ErrorCode::MalformedMessage);

    // No TLV area at all: the file ends with the body.
    const std::vector<std::byte> bodiless = ImageBuilder{}.body(64).build();
    const std::vector<std::byte> cut(bodiless.begin(), bodiless.begin() + 32 + 64);
    CHECK(code_of(dependencies_of(cut)) == ErrorCode::MalformedMessage);

    // More entries than kMaxImageTlvs.
    ImageBuilder crowded = base();
    for (std::size_t i = 0; i <= smply::limits::kMaxImageTlvs; ++i) {
        crowded.tlv(0x50, {});
    }
    CHECK(verdict(crowded) == ErrorCode::MalformedMessage);
}

TEST_CASE("a package whose own images disagree is refused", "[dfu_package][tlv]")
{
    // Image 0 needs image 1 at `minimum` or later; the package carries radio
    // `radio_major`.0.0.`radio_build`. MCUboot compares without the build
    // number by default (protocol-notes S41), and so does the check.
    const auto verdict = [](std::uint8_t radio_major, std::uint32_t radio_build,
                            const std::vector<std::byte>& dep) {
        const std::vector<std::byte> app = ImageBuilder{}
                                               .version(2, 0, 0, 0)
                                               .body(64)
                                               .protected_tlv(0x40, dep)
                                               .tlv(0x10, std::vector<std::byte>(32))
                                               .build();
        const std::vector<std::byte> radio = ImageBuilder{}
                                                 .version(radio_major, 0, 0, radio_build)
                                                 .body(64)
                                                 .tlv(0x10, std::vector<std::byte>(32))
                                                 .build();
        const std::vector<std::byte> archive =
            ZipBuilder{}
                .add("app.bin", app)
                .add("radio.bin", radio)
                .add("manifest.json",
                     std::string_view{R"({"files": [{"file": "app.bin", "image_index": "0"},
                                                    {"file": "radio.bin", "image_index": "1"}]})"})
                .build();
        return code_of(read_package(ConstBytes{archive}));
    };

    CHECK(verdict(6, 0, dependency(1, 6, 0, 0, 0)) == ErrorCode::Ok);
    CHECK(verdict(7, 0, dependency(1, 6, 0, 0, 0)) == ErrorCode::Ok);
    CHECK(verdict(5, 0, dependency(1, 6, 0, 0, 0)) == ErrorCode::InvalidArgument);
    CHECK(verdict(6, 0, dependency(1, 6, 1, 0, 0)) == ErrorCode::InvalidArgument);
    CHECK(verdict(6, 0, dependency(1, 6, 0, 1, 0)) == ErrorCode::InvalidArgument);
    // The build number does not count: 6.0.0 build 0 satisfies 6.0.0 build 9.
    CHECK(verdict(6, 0, dependency(1, 6, 0, 0, 9)) == ErrorCode::Ok);
    // A dependency on an image outside the package is the device's to judge.
    CHECK(verdict(5, 0, dependency(2, 9, 0, 0, 0)) == ErrorCode::Ok);
}
