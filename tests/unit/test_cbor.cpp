// SPDX-License-Identifier: Apache-2.0

#include "cbor/cbor.hpp"

#include "message_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using smply::ConstBytes;
using smply::ErrorCode;
using smply::MutBytes;
using smply::Result;
using smply::cbor::Reader;
using smply::cbor::Writer;
using smply::test::bytes_of;

namespace {

/// A comfortable encoding buffer for tests that are not about exhaustion.
using Buffer = std::array<std::byte, 512>;

std::vector<std::byte> encode_map(const std::function<void(Writer&)>& build)
{
    static Buffer buffer;
    Writer writer{MutBytes{buffer}};
    writer.open_map();
    build(writer);
    writer.close_map();
    const auto encoded = writer.finish();
    REQUIRE(encoded.has_value());
    return {encoded->begin(), encoded->end()};
}

} // namespace

// ---------------------------------------------------------------------------
// Golden vectors, hand-computed from RFC 8949. As with the SMP header, these
// are what would catch a misreading of the format itself; round-trip tests
// would happily agree with themselves.
// ---------------------------------------------------------------------------

TEST_CASE("golden vector: the empty map", "[cbor][golden]")
{
    // 0xA0 = map(0)
    const auto encoded = encode_map([](Writer&) {});
    REQUIRE(encoded == bytes_of({0xA0}));
}

TEST_CASE("golden vector: a small unsigned value", "[cbor][golden]")
{
    // A1                map(1)
    //    62 72 63       text(2) "rc"
    //    03             unsigned(3)
    const auto encoded = encode_map([](Writer& writer) { writer.put_uint("rc", 3); });
    REQUIRE(encoded == bytes_of({0xA1, 0x62, 0x72, 0x63, 0x03}));
}

TEST_CASE("golden vector: a value needing a one-byte extension", "[cbor][golden]")
{
    // 30 exceeds the 23 that fits in the initial byte, so it becomes 18 1E.
    const auto encoded = encode_map([](Writer& writer) { writer.put_uint("rc", 30); });
    REQUIRE(encoded == bytes_of({0xA1, 0x62, 0x72, 0x63, 0x18, 0x1E}));
}

TEST_CASE("golden vector: booleans", "[cbor][golden]")
{
    // F5 = true, F4 = false. MCUmgr's "confirm" field is exactly this shape.
    REQUIRE(encode_map([](Writer& writer) { writer.put_bool("confirm", true); }) ==
            bytes_of({0xA1, 0x67, 0x63, 0x6F, 0x6E, 0x66, 0x69, 0x72, 0x6D, 0xF5}));
    REQUIRE(encode_map([](Writer& writer) { writer.put_bool("confirm", false); }) ==
            bytes_of({0xA1, 0x67, 0x63, 0x6F, 0x6E, 0x66, 0x69, 0x72, 0x6D, 0xF4}));
}

TEST_CASE("golden vector: a byte string", "[cbor][golden]")
{
    // 43 = bytes(3). MCUmgr's "data" and "sha" fields are byte strings, and
    // encoding one as a text string would be silently wrong on the wire.
    const auto payload = bytes_of({0xDE, 0xAD, 0xBE});
    const auto encoded =
        encode_map([&](Writer& writer) { writer.put_bytes("d", ConstBytes{payload}); });
    REQUIRE(encoded == bytes_of({0xA1, 0x61, 0x64, 0x43, 0xDE, 0xAD, 0xBE}));
}

TEST_CASE("golden vector: a negative integer is not an unsigned one", "[cbor][golden]")
{
    // 20 = negative(-1). Distinct major type from unsigned, which is why
    // put_int and put_uint are separate rather than overloaded.
    const auto encoded = encode_map([](Writer& writer) { writer.put_int("rc", -1); });
    REQUIRE(encoded == bytes_of({0xA1, 0x62, 0x72, 0x63, 0x20}));
}

TEST_CASE("golden vector: a nested map", "[cbor][golden]")
{
    // {"err": {"group": 1, "rc": 30}} -- the SMP v2 error shape.
    static Buffer buffer;
    Writer writer{MutBytes{buffer}};
    writer.open_map();
    // The writer has no nested-map-under-key helper yet; the reader is the side
    // that needs to parse this shape (see the mgmt_error tests below), so this
    // vector is built by hand to check the reader against.
    writer.close_map();

    const auto expected = bytes_of({0xA1, 0x63, 0x65, 0x72, 0x72, 0xA2, 0x65, 0x67, 0x72, 0x6F,
                                    0x75, 0x70, 0x01, 0x62, 0x72, 0x63, 0x18, 0x1E});
    Reader reader{ConstBytes{expected}};
    REQUIRE(reader.enter_map().has_value());
    REQUIRE(reader.enter_map("err").has_value());
    REQUIRE(reader.uint("group") == 1);
    REQUIRE(reader.uint("rc") == 30);
    REQUIRE(reader.leave_map().has_value());
    REQUIRE(reader.status().has_value());
}

// ---------------------------------------------------------------------------
// Round-trips
// ---------------------------------------------------------------------------

TEST_CASE("every value type round-trips", "[cbor]")
{
    const auto blob = bytes_of({0x01, 0x02, 0x03, 0x04});
    const auto encoded = encode_map([&](Writer& writer) {
        writer.put_uint("u", 4294967296ULL); // beyond 32 bits
        writer.put_int("i", -12345);
        writer.put_bool("b", true);
        writer.put_text("t", "hello");
        writer.put_bytes("y", ConstBytes{blob});
    });

    Reader reader{ConstBytes{encoded}};
    REQUIRE(reader.enter_map().has_value());

    REQUIRE(reader.uint("u") == 4294967296ULL);
    REQUIRE(reader.integer("i") == -12345);
    REQUIRE(reader.boolean("b") == true);
    REQUIRE(reader.text("t") == "hello");

    const auto read_bytes = reader.bytes("y");
    REQUIRE(read_bytes.has_value());
    REQUIRE(std::vector<std::byte>(read_bytes->begin(), read_bytes->end()) == blob);

    REQUIRE(reader.status().has_value());
}

TEST_CASE("boundary integer values round-trip", "[cbor]")
{
    const std::uint64_t value =
        GENERATE(std::uint64_t{0}, std::uint64_t{23}, std::uint64_t{24}, std::uint64_t{255},
                 std::uint64_t{256}, std::uint64_t{65535}, std::uint64_t{65536},
                 std::uint64_t{0xFFFFFFFFULL}, std::uint64_t{0xFFFFFFFFFFFFFFFFULL});

    const auto encoded = encode_map([&](Writer& writer) { writer.put_uint("v", value); });

    Reader reader{ConstBytes{encoded}};
    REQUIRE(reader.enter_map().has_value());
    REQUIRE(reader.uint("v") == value);
}

TEST_CASE("an empty byte string round-trips", "[cbor]")
{
    // The final upload chunk can legitimately be empty.
    const auto encoded = encode_map([](Writer& writer) { writer.put_bytes("d", ConstBytes{}); });

    Reader reader{ConstBytes{encoded}};
    REQUIRE(reader.enter_map().has_value());
    const auto value = reader.bytes("d");
    REQUIRE(value.has_value());
    REQUIRE(value->empty());
}

// ---------------------------------------------------------------------------
// Absent versus wrong: the distinction the whole façade is built around
// ---------------------------------------------------------------------------

TEST_CASE("an absent key is nullopt, not an error", "[cbor]")
{
    // MCUmgr omits fields rather than sending false or zero, so absence is the
    // single most common case and must not poison the reader.
    const auto encoded = encode_map([](Writer& writer) { writer.put_uint("present", 1); });

    Reader reader{ConstBytes{encoded}};
    REQUIRE(reader.enter_map().has_value());

    REQUIRE_FALSE(reader.uint("missing").has_value());
    REQUIRE_FALSE(reader.integer("missing").has_value());
    REQUIRE_FALSE(reader.boolean("missing").has_value());
    REQUIRE_FALSE(reader.text("missing").has_value());
    REQUIRE_FALSE(reader.bytes("missing").has_value());

    // The reader is still healthy and still usable.
    REQUIRE(reader.status().has_value());
    REQUIRE(reader.uint("present") == 1);
}

TEST_CASE("a key of the wrong type is a sticky error, not a wrong value", "[cbor]")
{
    const auto encoded = encode_map([](Writer& writer) { writer.put_text("n", "not a number"); });

    Reader reader{ConstBytes{encoded}};
    REQUIRE(reader.enter_map().has_value());

    REQUIRE_FALSE(reader.uint("n").has_value());

    // Crucially this is a failure, not an absence: silently treating a
    // wrong-typed field as missing would let a malformed response look like a
    // successful one with defaults.
    const auto status = reader.status();
    REQUIRE_FALSE(status.has_value());
    REQUIRE(status.error().code() == ErrorCode::CborDecode);
}

TEST_CASE("the first error is the one reported", "[cbor]")
{
    const auto encoded = encode_map([](Writer& writer) {
        writer.put_text("a", "text");
        writer.put_text("b", "text");
    });

    Reader reader{ConstBytes{encoded}};
    REQUIRE(reader.enter_map().has_value());

    REQUIRE_FALSE(reader.uint("a").has_value());
    REQUIRE_FALSE(reader.uint("b").has_value());

    REQUIRE_FALSE(reader.status().has_value());
    REQUIRE(reader.status().error().code() == ErrorCode::CborDecode);
}

TEST_CASE("a failed reader stops returning values", "[cbor]")
{
    const auto encoded = encode_map([](Writer& writer) {
        writer.put_text("bad", "text");
        writer.put_uint("good", 7);
    });

    Reader reader{ConstBytes{encoded}};
    REQUIRE(reader.enter_map().has_value());

    REQUIRE_FALSE(reader.uint("bad").has_value());  // poisons the reader
    REQUIRE_FALSE(reader.uint("good").has_value()); // even though this is fine
    REQUIRE_FALSE(reader.status().has_value());
}

// ---------------------------------------------------------------------------
// Malformed and hostile input
// ---------------------------------------------------------------------------

TEST_CASE("input that is not a map is rejected", "[cbor]")
{
    // 0x01 is unsigned(1): well-formed CBOR, but not the map every MCUmgr
    // response is required to be.
    const auto encoded = bytes_of({0x01});

    Reader reader{ConstBytes{encoded}};
    const auto entered = reader.enter_map();

    REQUIRE_FALSE(entered.has_value());
    REQUIRE(entered.error().code() == ErrorCode::CborDecode);
}

TEST_CASE("empty input is rejected", "[cbor]")
{
    Reader reader{ConstBytes{}};
    REQUIRE_FALSE(reader.enter_map().has_value());
}

TEST_CASE("every prefix of a valid encoding is rejected without crashing", "[cbor][hostile]")
{
    // Truncation is what a dropped connection produces, and what a hostile peer
    // produces deliberately. No prefix may be mistaken for a complete document.
    const auto blob = bytes_of({0xAA, 0xBB, 0xCC, 0xDD});
    const auto full = encode_map([&](Writer& writer) {
        writer.put_uint("off", 1024);
        writer.put_text("rsn", "a reason");
        writer.put_bytes("sha", ConstBytes{blob});
        writer.put_bool("match", true);
    });

    const std::size_t length = GENERATE_COPY(range(std::size_t{0}, full.size()));
    INFO("prefix length " << length << " of " << full.size());

    Reader reader{ConstBytes{full}.first(length)};
    if (reader.enter_map().has_value()) {
        // A prefix may still open the map; reading through it must then fail
        // rather than yield truncated values that look plausible.
        static_cast<void>(reader.uint("off"));
        static_cast<void>(reader.text("rsn"));
        static_cast<void>(reader.bytes("sha"));
        static_cast<void>(reader.boolean("match"));
        REQUIRE_FALSE(reader.status().has_value());
    }
}

TEST_CASE("arbitrary bytes never crash the reader", "[cbor][hostile]")
{
    // A deterministic floor under the fuzzers: `fuzz_cbor_image_state` and
    // `fuzz_cbor_upload_response` search this space far harder, but they are
    // not part of ctest, so this is what runs on every build.
    const auto seed = static_cast<std::uint8_t>(GENERATE(range(0, 256)));
    const auto garbage = bytes_of({seed, 0xFF, 0x00, 0xA5, 0x5A, seed, 0x1F, 0xE0});

    Reader reader{ConstBytes{garbage}};
    if (reader.enter_map().has_value()) {
        static_cast<void>(reader.uint("a"));
        static_cast<void>(reader.text("b"));
        static_cast<void>(reader.bytes("c"));
    }
    static_cast<void>(reader.status());
    SUCCEED();
}

TEST_CASE("deeply nested input is rejected rather than recursed", "[cbor][hostile]")
{
    // 25 levels of single-entry map. The bound must fire well before anything
    // resembling recursion depth becomes interesting.
    std::vector<std::byte> nested;
    constexpr int kLevels = 25;
    for (int level = 0; level < kLevels; ++level) {
        nested.push_back(std::byte{0xA1}); // map(1)
        nested.push_back(std::byte{0x61}); // text(1)
        nested.push_back(std::byte{'n'});
    }
    nested.push_back(std::byte{0x00}); // innermost value

    Reader reader{ConstBytes{nested}};
    Result<void> outcome = reader.enter_map();
    for (int level = 1; level < kLevels && outcome.has_value(); ++level) {
        outcome = reader.enter_map("n");
    }

    REQUIRE_FALSE(outcome.has_value());
}

// ---------------------------------------------------------------------------
// Writer bounds
// ---------------------------------------------------------------------------

TEST_CASE("a buffer too small reports the failure from finish", "[cbor]")
{
    // The realistic case: an upload chunk sized against a tight device budget.
    std::array<std::byte, 4> tiny{};
    Writer writer{MutBytes{tiny}};

    writer.open_map();
    writer.put_text("key", "a value far larger than four bytes");
    writer.close_map();

    const auto encoded = writer.finish();
    REQUIRE_FALSE(encoded.has_value());
    REQUIRE(encoded.error().code() == ErrorCode::CborEncode);
}

TEST_CASE("an over-long key fails rather than truncating", "[cbor]")
{
    Buffer buffer{};
    Writer writer{MutBytes{buffer}};

    const std::string long_key(smply::cbor::kMaxKeyLength + 1, 'k');
    writer.open_map();
    writer.put_uint(long_key, 1);
    writer.close_map();

    REQUIRE(writer.failed());
    REQUIRE_FALSE(writer.finish().has_value());
}

TEST_CASE("a key at exactly the limit is accepted", "[cbor]")
{
    Buffer buffer{};
    Writer writer{MutBytes{buffer}};

    const std::string key(smply::cbor::kMaxKeyLength, 'k');
    writer.open_map();
    writer.put_uint(key, 1);
    writer.close_map();

    REQUIRE_FALSE(writer.failed());
    const auto encoded = writer.finish();
    REQUIRE(encoded.has_value());

    Reader reader{*encoded};
    REQUIRE(reader.enter_map().has_value());
    REQUIRE(reader.uint(key) == 1);
}

TEST_CASE("writer nesting is bounded", "[cbor]")
{
    Buffer buffer{};
    Writer writer{MutBytes{buffer}, /*max_nesting=*/3};

    for (int level = 0; level < 5; ++level) {
        writer.open_map();
    }

    REQUIRE(writer.failed());
    REQUIRE_FALSE(writer.finish().has_value());
}

TEST_CASE("writes after a failure are ignored rather than compounding", "[cbor]")
{
    Buffer buffer{};
    Writer writer{MutBytes{buffer}};

    const std::string long_key(smply::cbor::kMaxKeyLength + 1, 'k');
    writer.open_map();
    writer.put_uint(long_key, 1); // poisons the writer
    writer.put_uint("fine", 2);
    writer.close_map();

    REQUIRE(writer.failed());
    REQUIRE_FALSE(writer.finish().has_value());
}

// ---------------------------------------------------------------------------
// Arrays
// ---------------------------------------------------------------------------

TEST_CASE("an array of maps is visited element by element", "[cbor]")
{
    // {"images": [{"slot": 0}, {"slot": 1}]} -- the shape of an image-state
    // response, hand-built because the writer has no array support yet.
    const auto encoded = bytes_of({
        0xA1,                                     // map(1)
        0x66, 0x69, 0x6D, 0x61, 0x67, 0x65, 0x73, // "images"
        0x82,                                     // array(2)
        0xA1, 0x64, 0x73, 0x6C, 0x6F, 0x74, 0x00, // {"slot": 0}
        0xA1, 0x64, 0x73, 0x6C, 0x6F, 0x74, 0x01, // {"slot": 1}
    });

    Reader reader{ConstBytes{encoded}};
    REQUIRE(reader.enter_map().has_value());

    std::vector<std::uint64_t> slots;
    const auto outcome =
        reader.for_each_map_in_array("images", 16, [&](Reader& element) -> Result<void> {
            slots.push_back(element.uint("slot").value_or(999));
            return {};
        });

    REQUIRE(outcome.has_value());
    REQUIRE(slots == std::vector<std::uint64_t>{0, 1});
    REQUIRE(reader.status().has_value());
}

TEST_CASE("an absent array is an empty one, not an error", "[cbor]")
{
    // MCUmgr omits "images" entirely when no valid image can be reported --
    // normal after erasing a slot (protocol-notes section 6).
    const auto encoded = encode_map([](Writer& writer) { writer.put_uint("other", 1); });

    Reader reader{ConstBytes{encoded}};
    REQUIRE(reader.enter_map().has_value());

    int visits = 0;
    const auto outcome = reader.for_each_map_in_array("images", 16, [&](Reader&) -> Result<void> {
        ++visits;
        return {};
    });

    REQUIRE(outcome.has_value());
    REQUIRE(visits == 0);
    REQUIRE(reader.status().has_value());
}

TEST_CASE("an empty array visits nothing", "[cbor]")
{
    // {"images": []}
    const auto encoded = bytes_of({0xA1, 0x66, 0x69, 0x6D, 0x61, 0x67, 0x65, 0x73, 0x80});

    Reader reader{ConstBytes{encoded}};
    REQUIRE(reader.enter_map().has_value());

    int visits = 0;
    const auto outcome = reader.for_each_map_in_array("images", 16, [&](Reader&) -> Result<void> {
        ++visits;
        return {};
    });

    REQUIRE(outcome.has_value());
    REQUIRE(visits == 0);
}

TEST_CASE("array iteration is capped regardless of what the device sends", "[cbor][hostile]")
{
    // Eight elements, but the caller will only tolerate three.
    std::vector<std::byte> encoded =
        bytes_of({0xA1, 0x66, 0x69, 0x6D, 0x61, 0x67, 0x65, 0x73, 0x88});
    for (int i = 0; i < 8; ++i) {
        const auto element = bytes_of({0xA1, 0x64, 0x73, 0x6C, 0x6F, 0x74, 0x00});
        encoded.insert(encoded.end(), element.begin(), element.end());
    }

    Reader reader{ConstBytes{encoded}};
    REQUIRE(reader.enter_map().has_value());

    int visits = 0;
    const auto outcome = reader.for_each_map_in_array("images", 3, [&](Reader&) -> Result<void> {
        ++visits;
        return {};
    });

    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(outcome.error().code() == ErrorCode::CborDecode);
    REQUIRE(visits == 3); // stopped at the cap, did not run away
}

TEST_CASE("an array of exactly the cap is accepted", "[cbor]")
{
    // The boundary the cap has to get right: three elements with a cap of
    // three is legal. Testing the cap before entering an element rejects the
    // last legal one, because the end of the array is never looked for --
    // which is how P8 found this: a response holding exactly limits::kMaxImages
    // entries failed to decode.
    std::vector<std::byte> encoded =
        bytes_of({0xA1, 0x66, 0x69, 0x6D, 0x61, 0x67, 0x65, 0x73, 0x83});
    for (int i = 0; i < 3; ++i) {
        const auto element = bytes_of({0xA1, 0x64, 0x73, 0x6C, 0x6F, 0x74, 0x00});
        encoded.insert(encoded.end(), element.begin(), element.end());
    }

    Reader reader{ConstBytes{encoded}};
    REQUIRE(reader.enter_map().has_value());

    int visits = 0;
    const auto outcome = reader.for_each_map_in_array("images", 3, [&](Reader&) -> Result<void> {
        ++visits;
        return {};
    });

    REQUIRE(outcome.has_value());
    REQUIRE(visits == 3);
    REQUIRE(reader.status().has_value());
}

TEST_CASE("an error from the callback stops iteration and propagates", "[cbor]")
{
    const auto encoded = bytes_of({
        0xA1, 0x66, 0x69, 0x6D, 0x61, 0x67, 0x65, 0x73, 0x82, 0xA1, 0x64, 0x73,
        0x6C, 0x6F, 0x74, 0x00, 0xA1, 0x64, 0x73, 0x6C, 0x6F, 0x74, 0x01,
    });

    Reader reader{ConstBytes{encoded}};
    REQUIRE(reader.enter_map().has_value());

    int visits = 0;
    const auto outcome = reader.for_each_map_in_array("images", 16, [&](Reader&) -> Result<void> {
        ++visits;
        return smply::fail(ErrorCode::InvalidArgument, "test");
    });

    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(outcome.error().code() == ErrorCode::InvalidArgument);
    REQUIRE(visits == 1);
}

TEST_CASE("a key holding something other than an array is an error", "[cbor]")
{
    const auto encoded = encode_map([](Writer& writer) { writer.put_uint("images", 5); });

    Reader reader{ConstBytes{encoded}};
    REQUIRE(reader.enter_map().has_value());

    const auto outcome =
        reader.for_each_map_in_array("images", 16, [](Reader&) -> Result<void> { return {}; });

    REQUIRE_FALSE(outcome.has_value());
    REQUIRE(outcome.error().code() == ErrorCode::CborDecode);
}

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------

TEST_CASE("text and byte views point into the caller's buffer", "[cbor]")
{
    // Zero-copy is the point: nothing is allocated on the device's behalf.
    const auto blob = bytes_of({0x11, 0x22, 0x33});
    const auto encoded =
        encode_map([&](Writer& writer) { writer.put_bytes("d", ConstBytes{blob}); });

    Reader reader{ConstBytes{encoded}};
    REQUIRE(reader.enter_map().has_value());

    const auto view = reader.bytes("d");
    REQUIRE(view.has_value());

    const auto* base = encoded.data();
    REQUIRE(view->data() >= base);
    REQUIRE(view->data() + view->size() <= base + encoded.size());
}

// ---------------------------------------------------------------------------
// Bounds and sticky state
//
// Everything below was uncovered until P13, and the uncovered *list* is what
// said so: 21 of the 23 missing branches in src/cbor/ were the guards these
// cases exercise. The percentage said "write more tests"; the list said which.

TEST_CASE("a key longer than the label buffer is refused by every getter", "[cbor][limits]")
{
    // QCBOR's map API takes a C string, so a key is copied into a fixed buffer
    // first. The guard is repeated at every accessor, and one accessor missing
    // it would be a buffer overrun rather than a decode failure.
    const std::vector<std::byte> document =
        encode_map([](Writer& writer) { writer.put_uint("off", 42); });
    const std::string too_long(smply::cbor::kMaxKeyLength + 1, 'k');

    // A **fresh reader per accessor**, which is the whole point. The first
    // over-long key poisons the reader, and every later getter then returns on
    // the sticky-error check without ever reaching its own guard -- so a single
    // reader would exercise one of these seven and leave six untested while
    // passing. The uncovered-branch list is what showed that.
    const std::vector<std::function<bool(Reader&, const std::string&)>> accessors{
        [](Reader& r, const std::string& key) { return r.uint(key).has_value(); },
        [](Reader& r, const std::string& key) { return r.integer(key).has_value(); },
        [](Reader& r, const std::string& key) { return r.boolean(key).has_value(); },
        [](Reader& r, const std::string& key) { return r.text(key).has_value(); },
        [](Reader& r, const std::string& key) { return r.bytes(key).has_value(); },
        [](Reader& r, const std::string& key) { return r.enter_map(key).has_value(); },
        [](Reader& r, const std::string& key) {
            return r.for_each_map_in_array(key, 4, [](Reader&) { return Result<void>{}; })
                .has_value();
        },
    };

    for (const auto& access : accessors) {
        Reader reader{ConstBytes{document}};
        REQUIRE(reader.enter_map().has_value());
        CHECK_FALSE(access(reader, too_long));

        // A failure, not an absence: a key smply cannot even ask for is a bug
        // in smply, so the status must show it rather than reading as "the
        // device did not send that field".
        REQUIRE_FALSE(reader.ok());
        CHECK(reader.status().error().code() == ErrorCode::CborDecode);
    }
}

TEST_CASE("a key longer than the label buffer is refused by every setter", "[cbor][limits]")
{
    const std::string too_long(smply::cbor::kMaxKeyLength + 1, 'k');

    // A fresh writer each time, for the same reason as the reader above: the
    // first failure latches `failed_` and every later setter is a no-op.
    const std::vector<std::function<void(Writer&, const std::string&)>> setters{
        [](Writer& w, const std::string& key) { w.put_uint(key, 1); },
        [](Writer& w, const std::string& key) { w.put_int(key, -1); },
        [](Writer& w, const std::string& key) { w.put_bool(key, true); },
        [](Writer& w, const std::string& key) { w.put_text(key, "x"); },
        [](Writer& w, const std::string& key) { w.put_bytes(key, ConstBytes{}); },
    };

    for (const auto& set : setters) {
        Buffer buffer{};
        Writer writer{MutBytes{buffer}};
        writer.open_map();
        set(writer, too_long);
        CHECK(writer.failed());
        writer.close_map();
        CHECK_FALSE(writer.finish().has_value());
    }
}

TEST_CASE("closing a map that was never opened is a safe no-op", "[cbor][limits]")
{
    // Deliberately asymmetric with `Reader::leave_map()`, which records an
    // error for the same mistake. The reader's input comes from a device, so an
    // unbalanced structure there is something to report; the writer's calls all
    // come from smply itself, so the guard exists to stop the depth going
    // negative and corrupting QCBOR's state, not to diagnose a caller. It stays
    // a no-op rather than latching `failed_`, which would turn a harmless
    // double close into an encode failure.
    Buffer buffer{};
    Writer writer{MutBytes{buffer}};
    writer.close_map();
    CHECK_FALSE(writer.failed());

    // And the writer is still usable, which is the point of the guard.
    writer.open_map();
    writer.put_uint("off", 1);
    writer.close_map();
    writer.close_map();
    CHECK(writer.finish().has_value());
}

TEST_CASE("a nested map can be entered and left", "[cbor]")
{
    // The success side of `enter_map(key)`, which the SMP v2 `err` shape needs
    // and which nothing else in the suite exercised. Built by hand: `Writer`
    // has no way to open a map *under a key*, because nothing smply encodes is
    // nested -- only what it decodes is.
    //
    //   {"rc": 0, "err": {"group": 1, "rc": 24}}
    const auto document = bytes_of({0xA2, 0x62, 'r', 'c', 0x00, 0x63, 'e',  'r', 'r', 0xA2, 0x65,
                                    'g',  'r',  'o', 'u', 'p',  0x01, 0x62, 'r', 'c', 0x18, 0x18});

    Reader reader{ConstBytes{document}};
    REQUIRE(reader.enter_map().has_value());
    REQUIRE(reader.enter_map("err").has_value());
    CHECK(reader.uint("group") == 1);
    CHECK(reader.uint("rc") == 24);
    REQUIRE(reader.leave_map().has_value());
    CHECK(reader.uint("rc") == 0);
    CHECK(reader.ok());
}

TEST_CASE("the nesting limit bounds how deep a document can take the reader", "[cbor][limits]")
{
    // A device cannot make the decoder recurse without bound: the cap is
    // checked before entering, so the depth never exceeds it even briefly.
    const std::vector<std::byte> document =
        encode_map([](Writer& writer) { writer.put_uint("off", 1); });

    Reader reader{ConstBytes{document}, 1};
    REQUIRE(reader.enter_map().has_value());

    // Depth is now 1, which is the cap: both forms refuse, as does the array
    // walk, which enters an element.
    CHECK_FALSE(reader.enter_map().has_value());
    CHECK_FALSE(reader.enter_map("nested").has_value());
    CHECK_FALSE(reader.for_each_map_in_array("images", 4, [](Reader&) { return Result<void>{}; })
                    .has_value());
    CHECK_FALSE(reader.ok());
}

TEST_CASE("leave_map without a matching enter is a failure, not a crash", "[cbor][limits]")
{
    const std::vector<std::byte> document =
        encode_map([](Writer& writer) { writer.put_uint("off", 1); });

    Reader reader{ConstBytes{document}};
    CHECK_FALSE(reader.leave_map().has_value());
    CHECK(reader.status().error().code() == ErrorCode::CborDecode);
}

TEST_CASE("a poisoned reader refuses everything afterwards", "[cbor]")
{
    // First failure wins and is sticky, so a caller that checks status() once
    // at the end cannot miss an error that happened early.
    const std::vector<std::byte> document =
        encode_map([](Writer& writer) { writer.put_text("version", "1.2.3"); });

    Reader reader{ConstBytes{document}};
    REQUIRE(reader.enter_map().has_value());

    // Present, but not an integer: a genuine decode failure rather than an
    // absent field.
    CHECK(reader.uint("version") == std::nullopt);
    REQUIRE_FALSE(reader.ok());
    const Result<void> first = reader.status();

    CHECK(reader.uint("version") == std::nullopt);
    CHECK(reader.text("version") == std::nullopt);
    CHECK_FALSE(reader.enter_map().has_value());
    CHECK_FALSE(reader.enter_map("nested").has_value());
    CHECK_FALSE(reader.for_each_map_in_array("images", 4, [](Reader&) { return Result<void>{}; })
                    .has_value());

    // Still the *first* error, not the most recent one.
    CHECK(reader.status().error() == first.error());
}

TEST_CASE("entering a map that is not there leaves the reader usable", "[cbor]")
{
    // `enter_map(key)` is the one failure that is deliberately NOT sticky, so
    // it can be used to probe for an optional nested map. Poisoning the reader
    // here would make an absent optional field indistinguishable from a
    // malformed document -- see the note in cbor.hpp.
    const std::vector<std::byte> document = encode_map([](Writer& writer) {
        writer.put_uint("rc", 0);
        writer.put_text("version", "1.2.3");
    });

    Reader reader{ConstBytes{document}};
    REQUIRE(reader.enter_map().has_value());

    CHECK_FALSE(reader.enter_map("err").has_value());
    CHECK(reader.ok());

    // And reading carries on normally afterwards.
    CHECK(reader.uint("rc") == 0);
    CHECK(reader.text("version") == "1.2.3");
    CHECK(reader.ok());
}

TEST_CASE("a document that is not a map at all fails to enter", "[cbor]")
{
    const std::vector<std::byte> not_a_map = bytes_of({0x63, 'a', 'b', 'c'});
    Reader reader{ConstBytes{not_a_map}};
    CHECK_FALSE(reader.enter_map().has_value());
    CHECK(reader.status().error().code() == ErrorCode::CborDecode);
}

TEST_CASE("a truncated document fails rather than reading past the end", "[cbor]")
{
    const std::vector<std::byte> document =
        encode_map([](Writer& writer) { writer.put_bytes("hash", ConstBytes{}); });
    const ConstBytes truncated = ConstBytes{document}.first(document.size() - 1);

    Reader reader{truncated};
    if (reader.enter_map().has_value()) {
        CHECK(reader.bytes("hash") == std::nullopt);
    }
    CHECK_FALSE(reader.ok());
}
