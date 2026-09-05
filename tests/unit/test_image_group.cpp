// SPDX-License-Identifier: Apache-2.0
//
// The image group is the first with real decoding: nested arrays, fields whose
// absence means something, and lengths the device chooses. Two rules shape this
// suite.
//
// Request vectors are hand-derived from the CBOR grammar and the field names in
// docs/protocol-notes.md section 6, never captured from what the writer emitted
// -- an encoder checked against its own output proves only that it is
// deterministic.
//
// Responses are built by `Cbor` below, a deliberately independent encoder: it
// shares no code with src/cbor/, so a decoder bug cannot be cancelled out by a
// matching encoder bug. `golden_state_response()` is written out byte by byte
// and asserted against the builder, which is what keeps the builder honest.

#include "smply/groups/image.hpp"

#include "fake_transport.hpp"
#include "manual_clock.hpp"
#include "message_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using smply::ConstBytes;
using smply::EraseOptions;
using smply::ErrorCode;
using smply::Group;
using smply::Hash;
using smply::Header;
using smply::ImageError;
using smply::ImageHash;
using smply::ImageManagement;
using smply::ImageSlot;
using smply::ImageState;
using smply::ImageVersion;
using smply::MgmtError;
using smply::Operation;
using smply::RequestHandle;
using smply::Result;
using smply::SetStateRequest;
using smply::SlotInfo;
using smply::SmpClient;
using smply::SmpError;
using smply::Version;
using smply::test::bytes_of;
using smply::test::FakeTransport;
using smply::test::make_message;
using smply::test::ManualClock;

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

// ---------------------------------------------------------------------------
// An independent CBOR encoder, for building responses
// ---------------------------------------------------------------------------

/// Definite-length CBOR, written from RFC 8949's head encoding directly.
class Cbor
{
public:
    Cbor& map(std::uint64_t pairs)
    {
        return head(5, pairs);
    }

    Cbor& array(std::uint64_t items)
    {
        return head(4, items);
    }

    Cbor& uint(std::uint64_t value)
    {
        return head(0, value);
    }

    /// A negative integer; \p value must be negative.
    Cbor& nint(std::int64_t value)
    {
        return head(1, static_cast<std::uint64_t>(-(value + 1)));
    }

    Cbor& text(std::string_view value)
    {
        head(3, value.size());
        for (const char ch : value) {
            out_.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
        }
        return *this;
    }

    Cbor& blob(ConstBytes value)
    {
        head(2, value.size());
        out_.insert(out_.end(), value.begin(), value.end());
        return *this;
    }

    Cbor& boolean(bool value)
    {
        out_.push_back(static_cast<std::byte>(value ? 0xF5 : 0xF4));
        return *this;
    }

    /// Raw bytes, for expressing something no well-formed encoder would write.
    Cbor& raw(const std::vector<std::byte>& value)
    {
        out_.insert(out_.end(), value.begin(), value.end());
        return *this;
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const
    {
        return out_;
    }

    [[nodiscard]] ConstBytes view() const
    {
        return ConstBytes{out_};
    }

private:
    Cbor& head(std::uint8_t major, std::uint64_t argument)
    {
        const auto tag = static_cast<std::uint8_t>(major << 5U);
        const auto push = [this](std::uint8_t byte) {
            out_.push_back(static_cast<std::byte>(byte));
        };
        if (argument < 24) {
            push(static_cast<std::uint8_t>(tag | argument));
        } else if (argument <= 0xFF) {
            push(static_cast<std::uint8_t>(tag | 24U));
            push(static_cast<std::uint8_t>(argument));
        } else if (argument <= 0xFFFF) {
            push(static_cast<std::uint8_t>(tag | 25U));
            push(static_cast<std::uint8_t>(argument >> 8U));
            push(static_cast<std::uint8_t>(argument));
        } else if (argument <= 0xFFFFFFFF) {
            push(static_cast<std::uint8_t>(tag | 26U));
            for (int shift = 24; shift >= 0; shift -= 8) {
                push(static_cast<std::uint8_t>(argument >> static_cast<unsigned>(shift)));
            }
        } else {
            push(static_cast<std::uint8_t>(tag | 27U));
            for (int shift = 56; shift >= 0; shift -= 8) {
                push(static_cast<std::uint8_t>(argument >> static_cast<unsigned>(shift)));
            }
        }
        return *this;
    }

    std::vector<std::byte> out_;
};

/// `count` bytes starting at `first`, as a hash-shaped blob.
std::vector<std::byte> digest(std::size_t count, std::uint8_t first = 0)
{
    std::vector<std::byte> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(static_cast<std::byte>((first + i) & 0xFFU));
    }
    return out;
}

/// A two-slot image-state response, written out byte by byte.
///
/// {"images": [{"slot":0,"version":"1.0.0","hash":h'00..1F',"bootable":true,
///              "confirmed":true,"active":true},
///             {"slot":1,"version":"1.0.1","hash":h'20..3F',"bootable":true,
///              "pending":true}],
///  "splitStatus": 0}
std::vector<std::byte> golden_state_response()
{
    auto out = bytes_of({0xA2, 0x66, 'i', 'm', 'a', 'g',  'e', 's', 0x82, //
                         0xA6,                                            //
                         0x64, 's',  'l', 'o', 't', 0x00,                 //
                         0x67, 'v',  'e', 'r', 's', 'i',  'o', 'n',       //
                         0x65, '1',  '.', '0', '.', '0',                  //
                         0x64, 'h',  'a', 's', 'h', 0x58, 0x20});
    const auto first = digest(32, 0x00);
    out.insert(out.end(), first.begin(), first.end());

    const auto middle = bytes_of({0x68, 'b', 'o', 'o', 't', 'a',  'b', 'l',  'e', 0xF5,       //
                                  0x69, 'c', 'o', 'n', 'f', 'i',  'r', 'm',  'e', 'd',  0xF5, //
                                  0x66, 'a', 'c', 't', 'i', 'v',  'e', 0xF5,                  //
                                  0xA5,                                                       //
                                  0x64, 's', 'l', 'o', 't', 0x01,                             //
                                  0x67, 'v', 'e', 'r', 's', 'i',  'o', 'n',                   //
                                  0x65, '1', '.', '0', '.', '1',                              //
                                  0x64, 'h', 'a', 's', 'h', 0x58, 0x20});
    out.insert(out.end(), middle.begin(), middle.end());

    const auto second = digest(32, 0x20);
    out.insert(out.end(), second.begin(), second.end());

    const auto tail =
        bytes_of({0x68, 'b', 'o', 'o', 't', 'a', 'b', 'l', 'e',  0xF5, //
                  0x67, 'p', 'e', 'n', 'd', 'i', 'n', 'g', 0xF5,       //
                  0x6B, 's', 'p', 'l', 'i', 't', 'S', 't', 'a',  't',  'u', 's', 0x00});
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

/// One image-state entry, with only the fields the specification requires.
void put_minimal_slot(Cbor& cbor, std::uint64_t slot, std::string_view version)
{
    cbor.map(2).text("slot").uint(slot).text("version").text(version);
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

/// Collects one command's outcome.
template<class T>
struct Outcome
{
    int calls = 0;
    std::optional<ErrorCode> code;
    std::optional<T> value;
    std::optional<MgmtError> mgmt;

    [[nodiscard]] auto callback()
    {
        return [this](Result<T> result) {
            ++calls;
            if (result.has_value()) {
                value = *result;
            } else {
                code = result.error().code();
                mgmt = result.error().mgmt();
            }
        };
    }
};

/// `Result<void>` has no value to capture.
struct VoidOutcome
{
    int calls = 0;
    std::optional<ErrorCode> code;
    std::optional<MgmtError> mgmt;

    [[nodiscard]] auto callback()
    {
        return [this](Result<void> result) {
            ++calls;
            if (!result.has_value()) {
                code = result.error().code();
                mgmt = result.error().mgmt();
            }
        };
    }
};

/// Transport, clock, client and group.
///
/// The declaration-order rule from test_smp_client.cpp applies here too:
/// declare anything a callback captures *before* the fixture, because
/// ~SmpClient completes outstanding requests and so runs callbacks during the
/// fixture's destruction.
struct Fixture
{
    FakeTransport transport;
    ManualClock clock;
    SmpClient client;
    ImageManagement image;

    Fixture() : client{transport, clock}, image{client} {}

    /// The CBOR payload of the last request sent, with the header stripped.
    [[nodiscard]] std::vector<std::byte> sent_payload() const
    {
        const auto sent = transport.last_sent();
        REQUIRE(sent.size() >= smply::kHeaderSize);
        return {sent.begin() + smply::kHeaderSize, sent.end()};
    }

    [[nodiscard]] Header sent_header() const
    {
        const auto decoded = smply::decode_header(transport.last_sent());
        REQUIRE(decoded.has_value());
        return *decoded;
    }

    /// A response that correctly answers the last request sent.
    void respond(ConstBytes payload)
    {
        const Header request = sent_header();
        const Header reply{.op = smply::response_to(request.op),
                           .version = Version::V1,
                           .flags = 0,
                           .length = 0,
                           .group = request.group,
                           .seq = request.seq,
                           .command = request.command};
        const auto message = make_message(reply, payload);
        transport.deliver(ConstBytes{message});
    }

    /// Issues get_state and answers it with \p payload.
    [[nodiscard]] Outcome<ImageState> state_from(ConstBytes payload)
    {
        Outcome<ImageState> outcome;
        static_cast<void>(image.get_state(outcome.callback()));
        respond(payload);
        return outcome;
    }
};

/// A group-scoped image error, `{"err": {"group": 1, "rc": rc}}`.
std::vector<std::byte> image_err(std::uint64_t rc)
{
    Cbor cbor;
    cbor.map(1).text("err").map(2).text("group").uint(1).text("rc").uint(rc);
    return cbor.bytes();
}

} // namespace

// ---------------------------------------------------------------------------
// Request encodings
// ---------------------------------------------------------------------------

TEST_CASE("get_state is a read of the empty map", "[image][state][encoding]")
{
    Outcome<ImageState> outcome;

    Fixture fixture;

    static_cast<void>(fixture.image.get_state(outcome.callback()));

    const Header header = fixture.sent_header();
    REQUIRE(header.op == Operation::Read);
    REQUIRE(header.group == Group::Image);
    REQUIRE(header.command == 0);
    REQUIRE(fixture.sent_payload() == bytes_of({0xA0}));
}

TEST_CASE("get_slot_info is a read of the empty map", "[image][slotinfo][encoding]")
{
    Outcome<SlotInfo> outcome;

    Fixture fixture;

    static_cast<void>(fixture.image.get_slot_info(outcome.callback()));

    const Header header = fixture.sent_header();
    REQUIRE(header.op == Operation::Read);
    REQUIRE(header.group == Group::Image);
    REQUIRE(header.command == 6);
    REQUIRE(fixture.sent_payload() == bytes_of({0xA0}));
}

TEST_CASE("set_state marks an image for test with its hash", "[image][setstate][encoding]")
{
    // {"hash": h'00..1F', "confirm": false}. The 0xF4 is what distinguishes a
    // trial boot from a confirmation, so it is encoded rather than omitted.
    Outcome<ImageState> outcome;

    Fixture fixture;

    Hash hash{};
    for (std::size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<std::byte>(i);
    }
    SetStateRequest request;
    request.hash = ImageHash::from(hash);
    static_cast<void>(fixture.image.set_state(request, outcome.callback()));

    auto expected = bytes_of({0xA2, 0x64, 'h', 'a', 's', 'h', 0x58, 0x20});
    const auto body = digest(32, 0x00);
    expected.insert(expected.end(), body.begin(), body.end());
    const auto tail = bytes_of({0x67, 'c', 'o', 'n', 'f', 'i', 'r', 'm', 0xF4});
    expected.insert(expected.end(), tail.begin(), tail.end());

    const Header header = fixture.sent_header();
    REQUIRE(header.op == Operation::Write);
    REQUIRE(header.command == 0);
    REQUIRE(fixture.sent_payload() == expected);
}

TEST_CASE("set_state confirms the running image with no hash", "[image][setstate][encoding]")
{
    // "the currently running application will be assumed as target for
    // confirmation" (protocol-notes section 6), so the hash is omitted.
    Outcome<ImageState> outcome;

    Fixture fixture;

    SetStateRequest request;
    request.confirm = true;
    static_cast<void>(fixture.image.set_state(request, outcome.callback()));

    REQUIRE(fixture.sent_payload() ==
            bytes_of({0xA1, 0x67, 'c', 'o', 'n', 'f', 'i', 'r', 'm', 0xF5}));
}

TEST_CASE("set_state carries a 64-byte hash", "[image][setstate][encoding]")
{
    // IMAGE_SHA_LEN is 64 on a bootloader built for SHA-512, so the byte-string
    // header is 0x58 0x40 rather than 0x58 0x20.
    Outcome<ImageState> outcome;

    Fixture fixture;

    const auto long_hash = ImageHash::from(ConstBytes{digest(64, 0x00)});
    REQUIRE(long_hash.has_value());
    SetStateRequest request;
    request.hash = *long_hash;
    request.confirm = true;
    static_cast<void>(fixture.image.set_state(request, outcome.callback()));

    const auto payload = fixture.sent_payload();
    REQUIRE(payload.size() == 1 + 5 + 2 + 64 + 8 + 1);
    REQUIRE(payload[6] == std::byte{0x58});
    REQUIRE(payload[7] == std::byte{0x40});
}

TEST_CASE("a test with no hash is refused before anything is sent", "[image][setstate]")
{
    // The device answers IMG_MGMT_ERR_INVALID_HASH for exactly this; saying so
    // here costs no round trip. Refused, but not inside the call.
    Outcome<ImageState> outcome;

    Fixture fixture;

    const RequestHandle handle = fixture.image.set_state(SetStateRequest{}, outcome.callback());

    REQUIRE_FALSE(handle.valid());
    REQUIRE(fixture.transport.send_count() == 0);
    REQUIRE(outcome.calls == 0);

    fixture.client.poll(fixture.clock.now());
    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::InvalidArgument);
}

TEST_CASE("erase without a slot leaves the choice to the device", "[image][erase][encoding]")
{
    VoidOutcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.image.erase(outcome.callback()));

    const Header header = fixture.sent_header();
    REQUIRE(header.op == Operation::Write);
    REQUIRE(header.command == 5);
    REQUIRE(fixture.sent_payload() == bytes_of({0xA0}));
}

TEST_CASE("erase names a slot when the caller chose one", "[image][erase][encoding]")
{
    VoidOutcome outcome;

    Fixture fixture;

    EraseOptions options;
    options.slot = 1;
    static_cast<void>(fixture.image.erase(options, outcome.callback()));

    REQUIRE(fixture.sent_payload() == bytes_of({0xA1, 0x64, 's', 'l', 'o', 't', 0x01}));
}

TEST_CASE("erase uses the long timeout, not the client's default", "[image][erase][timeout]")
{
    // A12: erase is synchronous on the device and may take tens of seconds.
    VoidOutcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.image.erase(outcome.callback()));

    const auto deadline = fixture.client.next_deadline();
    REQUIRE(deadline.has_value());
    REQUIRE(*deadline == fixture.clock.now() + smply::limits::kEraseTimeout);
}

TEST_CASE("erase honours an explicit timeout", "[image][erase][timeout]")
{
    VoidOutcome outcome;

    Fixture fixture;

    EraseOptions options;
    options.timeout = std::chrono::seconds{120};
    static_cast<void>(fixture.image.erase(options, outcome.callback()));

    const auto deadline = fixture.client.next_deadline();
    REQUIRE(deadline.has_value());
    REQUIRE(*deadline == fixture.clock.now() + std::chrono::seconds{120});
}

TEST_CASE("an impossible slot number is refused before anything is sent", "[image][erase]")
{
    VoidOutcome outcome;

    Fixture fixture;

    EraseOptions options;
    options.slot = 100000;
    const RequestHandle handle = fixture.image.erase(options, outcome.callback());

    REQUIRE_FALSE(handle.valid());
    REQUIRE(fixture.transport.send_count() == 0);
    REQUIRE(outcome.calls == 0);

    fixture.client.poll(fixture.clock.now());
    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Image state decoding
// ---------------------------------------------------------------------------

TEST_CASE("the response builder agrees with a hand-written vector", "[image][state]")
{
    // Proves the independent encoder used by the rest of this file, against
    // bytes derived from the CBOR grammar rather than from either encoder.
    Cbor cbor;
    cbor.map(2)
        .text("images")
        .array(2)
        .map(6)
        .text("slot")
        .uint(0)
        .text("version")
        .text("1.0.0")
        .text("hash")
        .blob(ConstBytes{digest(32, 0x00)})
        .text("bootable")
        .boolean(true)
        .text("confirmed")
        .boolean(true)
        .text("active")
        .boolean(true)
        .map(5)
        .text("slot")
        .uint(1)
        .text("version")
        .text("1.0.1")
        .text("hash")
        .blob(ConstBytes{digest(32, 0x20)})
        .text("bootable")
        .boolean(true)
        .text("pending")
        .boolean(true)
        .text("splitStatus")
        .uint(0);

    REQUIRE(cbor.bytes() == golden_state_response());
}

TEST_CASE("a two-slot state response decodes field by field", "[image][state]")
{
    Fixture fixture;

    const auto golden = golden_state_response();
    const auto outcome = fixture.state_from(ConstBytes{golden});

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.value.has_value());
    const ImageState& state = *outcome.value;
    REQUIRE(state.slots.size() == 2);
    REQUIRE(state.split_status == 0);

    REQUIRE(state.slots[0].image == 0);
    REQUIRE(state.slots[0].slot == 0);
    REQUIRE(state.slots[0].version == "1.0.0");
    REQUIRE(state.slots[0].hash.has_value());
    REQUIRE(state.slots[0].hash->size() == 32);
    REQUIRE(state.slots[0].bootable);
    REQUIRE(state.slots[0].confirmed);
    REQUIRE(state.slots[0].active);
    REQUIRE_FALSE(state.slots[0].pending);
    REQUIRE_FALSE(state.slots[0].permanent);

    REQUIRE(state.slots[1].slot == 1);
    REQUIRE(state.slots[1].version == "1.0.1");
    REQUIRE(state.slots[1].pending);
    REQUIRE_FALSE(state.slots[1].active);
    REQUIRE_FALSE(state.slots[1].confirmed);
}

TEST_CASE("an absent image number means image zero", "[image][state]")
{
    // A9: a single-image device omits "image" entirely.
    Fixture fixture;

    Cbor cbor;
    cbor.map(1).text("images").array(1);
    put_minimal_slot(cbor, 0, "1.0.0");

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.value.has_value());
    REQUIRE(outcome.value->slots.size() == 1);
    REQUIRE(outcome.value->slots[0].image == 0);
}

TEST_CASE("an empty images array is a successful, empty answer", "[image][state]")
{
    // "a response will only contain information for valid images" -- which is
    // what a freshly erased secondary slot looks like, not an error.
    Fixture fixture;

    Cbor cbor;
    cbor.map(1).text("images").array(0);

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.value.has_value());
    REQUIRE(outcome.value->slots.empty());
    REQUIRE_FALSE(outcome.value->split_status.has_value());
}

TEST_CASE("an absent images array is a successful, empty answer", "[image][state]")
{
    Fixture fixture;

    const auto outcome = fixture.state_from(ConstBytes{bytes_of({0xA0})});

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.value.has_value());
    REQUIRE(outcome.value->slots.empty());
}

TEST_CASE("absent flags are false", "[image][state]")
{
    // The rule the specification states: every flag is omitted when false.
    Fixture fixture;

    Cbor cbor;
    cbor.map(1).text("images").array(1);
    put_minimal_slot(cbor, 1, "0.0.0");

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.value.has_value());
    const ImageSlot& slot = outcome.value->slots.at(0);
    REQUIRE_FALSE(slot.bootable);
    REQUIRE_FALSE(slot.pending);
    REQUIRE_FALSE(slot.confirmed);
    REQUIRE_FALSE(slot.active);
    REQUIRE_FALSE(slot.permanent);
    REQUIRE_FALSE(slot.hash.has_value());
}

TEST_CASE("flags present and false are false", "[image][state]")
{
    // What the *implementation* does by default: only a build with
    // CONFIG_MCUMGR_GRP_IMG_FRUGAL_LIST omits a false flag, so a client that
    // treated "present" as "true" would misread every ordinary device
    // (protocol-notes section 6).
    Fixture fixture;

    Cbor cbor;
    cbor.map(1)
        .text("images")
        .array(1)
        .map(7)
        .text("slot")
        .uint(1)
        .text("version")
        .text("0.0.0")
        .text("bootable")
        .boolean(false)
        .text("pending")
        .boolean(false)
        .text("confirmed")
        .boolean(false)
        .text("active")
        .boolean(false)
        .text("permanent")
        .boolean(false);

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.value.has_value());
    const ImageSlot& slot = outcome.value->slots.at(0);
    REQUIRE_FALSE(slot.bootable);
    REQUIRE_FALSE(slot.pending);
    REQUIRE_FALSE(slot.confirmed);
    REQUIRE_FALSE(slot.active);
    REQUIRE_FALSE(slot.permanent);
}

TEST_CASE("every flag present and true is carried through", "[image][state]")
{
    Fixture fixture;

    Cbor cbor;
    cbor.map(1)
        .text("images")
        .array(1)
        .map(8)
        .text("image")
        .uint(1)
        .text("slot")
        .uint(1)
        .text("version")
        .text("0.0.0")
        .text("bootable")
        .boolean(true)
        .text("pending")
        .boolean(true)
        .text("confirmed")
        .boolean(true)
        .text("active")
        .boolean(true)
        .text("permanent")
        .boolean(true);

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.value.has_value());
    const ImageSlot& slot = outcome.value->slots.at(0);
    REQUIRE(slot.image == 1);
    REQUIRE(slot.bootable);
    REQUIRE(slot.pending);
    REQUIRE(slot.confirmed);
    REQUIRE(slot.active);
    REQUIRE(slot.permanent);
}

TEST_CASE("an unformattable version string is kept verbatim", "[image][state]")
{
    // The server substitutes a literal placeholder when it cannot format the
    // version. It is reported, not rejected: parsing is a separate, fallible
    // step. Spelled as a raw string so the ??> is not read as a trigraph.
    Fixture fixture;

    Cbor cbor;
    cbor.map(1).text("images").array(1);
    put_minimal_slot(cbor, 0, R"(<???>)");

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.value.has_value());
    REQUIRE(outcome.value->slots.at(0).version == R"(<???>)");
    REQUIRE_FALSE(ImageVersion::parse(outcome.value->slots.at(0).version).has_value());
}

TEST_CASE("a negative splitStatus is carried through", "[image][state]")
{
    Fixture fixture;

    Cbor cbor;
    cbor.map(2).text("images").array(0).text("splitStatus").nint(-1);

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.value.has_value());
    REQUIRE(outcome.value->split_status == -1);
}

TEST_CASE("a 64-byte hash is accepted whole", "[image][state]")
{
    // IMAGE_SHA_LEN is 64 under CONFIG_MCUBOOT_BOOTLOADER_USES_SHA512.
    Fixture fixture;

    Cbor cbor;
    cbor.map(1)
        .text("images")
        .array(1)
        .map(3)
        .text("slot")
        .uint(0)
        .text("version")
        .text("1.0.0")
        .text("hash")
        .blob(ConstBytes{digest(64, 0x00)});

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.value.has_value());
    const auto& hash = outcome.value->slots.at(0).hash;
    REQUIRE(hash.has_value());
    REQUIRE(hash->size() == 64);
    REQUIRE(hash->bytes()[63] == std::byte{63});
}

TEST_CASE("a hash longer than the bound is rejected", "[image][state][hostile]")
{
    Fixture fixture;

    Cbor cbor;
    cbor.map(1)
        .text("images")
        .array(1)
        .map(3)
        .text("slot")
        .uint(0)
        .text("version")
        .text("1.0.0")
        .text("hash")
        .blob(ConstBytes{digest(smply::limits::kMaxImageHashLength + 1, 0x00)});

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::CborDecode);
    REQUIRE_FALSE(outcome.value.has_value());
}

TEST_CASE("an empty hash is a decode failure, not an absent one", "[image][state][hostile]")
{
    Fixture fixture;

    Cbor cbor;
    cbor.map(1)
        .text("images")
        .array(1)
        .map(3)
        .text("slot")
        .uint(0)
        .text("version")
        .text("1.0.0")
        .text("hash")
        .blob(ConstBytes{});

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("an over-long version string is rejected before the copy", "[image][state][hostile]")
{
    Fixture fixture;

    const std::string too_long(smply::limits::kMaxVersionStringLength + 1, 'x');
    Cbor cbor;
    cbor.map(1).text("images").array(1);
    put_minimal_slot(cbor, 0, too_long);

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("a version string of exactly the bound is accepted", "[image][state]")
{
    Fixture fixture;

    const std::string at_limit(smply::limits::kMaxVersionStringLength, 'x');
    Cbor cbor;
    cbor.map(1).text("images").array(1);
    put_minimal_slot(cbor, 0, at_limit);

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.value.has_value());
    REQUIRE(outcome.value->slots.at(0).version == at_limit);
}

TEST_CASE("more images than the bound is a bounded failure", "[image][state][hostile]")
{
    Fixture fixture;

    Cbor cbor;
    cbor.map(1).text("images").array(smply::limits::kMaxImages + 1);
    for (std::size_t i = 0; i <= smply::limits::kMaxImages; ++i) {
        put_minimal_slot(cbor, i % 2, "1.0.0");
    }

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("exactly the bound of images is accepted", "[image][state]")
{
    Fixture fixture;

    Cbor cbor;
    cbor.map(1).text("images").array(smply::limits::kMaxImages);
    for (std::size_t i = 0; i < smply::limits::kMaxImages; ++i) {
        put_minimal_slot(cbor, i % 2, "1.0.0");
    }

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.value.has_value());
    REQUIRE(outcome.value->slots.size() == smply::limits::kMaxImages);
}

TEST_CASE("a wrong-typed field is a decode failure, not a default", "[image][state][hostile]")
{
    // The distinction that matters: a wrong type poisons the reader and makes
    // every field look absent, so without the status() check this would decode
    // into a plausible entry full of defaults.
    Fixture fixture;

    Cbor cbor;
    cbor.map(1)
        .text("images")
        .array(1)
        .map(2)
        .text("slot")
        .text("zero") // a text string where a uint belongs
        .text("version")
        .text("1.0.0");

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::CborDecode);
    REQUIRE_FALSE(outcome.value.has_value());
}

TEST_CASE("an entry with no slot is rejected", "[image][state][hostile]")
{
    Fixture fixture;

    Cbor cbor;
    cbor.map(1).text("images").array(1).map(1).text("version").text("1.0.0");

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("an entry with no version is rejected", "[image][state][hostile]")
{
    Fixture fixture;

    Cbor cbor;
    cbor.map(1).text("images").array(1).map(1).text("slot").uint(0);

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("an array element that is not a map is rejected", "[image][state][hostile]")
{
    Fixture fixture;

    Cbor cbor;
    cbor.map(1).text("images").array(1).uint(7);

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("a slot number beyond 32 bits is rejected", "[image][state][hostile]")
{
    Fixture fixture;

    Cbor cbor;
    cbor.map(1)
        .text("images")
        .array(1)
        .map(2)
        .text("slot")
        .uint(0x1'0000'0000ULL)
        .text("version")
        .text("1.0.0");

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("an image number beyond 32 bits is rejected", "[image][state][hostile]")
{
    Fixture fixture;

    Cbor cbor;
    cbor.map(1)
        .text("images")
        .array(1)
        .map(3)
        .text("image")
        .uint(0x1'0000'0000ULL)
        .text("slot")
        .uint(0)
        .text("version")
        .text("1.0.0");

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("a wrong-typed splitStatus is a decode failure", "[image][state][hostile]")
{
    // Checked after the array walk, which is the one place a top-level field
    // can poison a reader that has already produced a plausible slot list.
    Fixture fixture;

    Cbor cbor;
    cbor.map(2).text("images").array(0).text("splitStatus").text("nope");

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.code == ErrorCode::CborDecode);
    REQUIRE_FALSE(outcome.value.has_value());
}

TEST_CASE("a splitStatus beyond 32 bits is rejected", "[image][state][hostile]")
{
    Fixture fixture;

    Cbor cbor;
    cbor.map(2).text("images").array(0).text("splitStatus").uint(0x8000'0000ULL);

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("a state response with no callback is simply dropped", "[image][state]")
{
    // A caller that wants the request sent but not the answer is legal; the
    // group must not dereference an empty std::function.
    Fixture fixture;

    static_cast<void>(fixture.image.get_state({}));
    const auto golden = golden_state_response();
    REQUIRE_NOTHROW(fixture.respond(ConstBytes{golden}));

    static_cast<void>(fixture.image.erase({}));
    REQUIRE_NOTHROW(fixture.respond(ConstBytes{bytes_of({0xA0})}));
}

TEST_CASE("every truncation of a state response is handled", "[image][state][hostile]")
{
    // The prefix sweep P5 applies to the reader, at the group level: no prefix
    // of a valid response may crash or be mistaken for a good one.
    const auto full = golden_state_response();

    for (std::size_t length = 0; length < full.size(); ++length) {
        Outcome<ImageState> outcome;

        Fixture fixture;

        static_cast<void>(fixture.image.get_state(outcome.callback()));
        fixture.respond(ConstBytes{full.data(), length});

        REQUIRE(outcome.calls == 1);
        REQUIRE(outcome.code.has_value());
    }
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

TEST_CASE("the accessors find slots by role and by hash", "[image][state]")
{
    Fixture fixture;

    const auto golden = golden_state_response();
    const auto outcome = fixture.state_from(ConstBytes{golden});
    REQUIRE(outcome.value.has_value());
    const ImageState& state = *outcome.value;

    const ImageSlot* active = state.active_slot();
    REQUIRE(active != nullptr);
    REQUIRE(active->slot == 0);

    const ImageSlot* secondary = state.secondary();
    REQUIRE(secondary != nullptr);
    REQUIRE(secondary->slot == 1);

    const auto wanted = ImageHash::from(ConstBytes{digest(32, 0x20)});
    REQUIRE(wanted.has_value());
    const ImageSlot* found = state.find_by_hash(*wanted);
    REQUIRE(found != nullptr);
    REQUIRE(found->slot == 1);

    const auto absent = ImageHash::from(ConstBytes{digest(32, 0x80)});
    REQUIRE(absent.has_value());
    REQUIRE(state.find_by_hash(*absent) == nullptr);

    // Nothing was reported for image 1.
    REQUIRE(state.active_slot(1) == nullptr);
    REQUIRE(state.secondary(1) == nullptr);
}

TEST_CASE("secondary is nullptr once the slot has been erased", "[image][state]")
{
    // After an erase the device reports nothing for that slot, because it holds
    // no valid image -- which is exactly what "no secondary" should mean.
    Fixture fixture;

    Cbor cbor;
    cbor.map(1)
        .text("images")
        .array(1)
        .map(3)
        .text("slot")
        .uint(0)
        .text("version")
        .text("1.0.0")
        .text("active")
        .boolean(true);

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.value.has_value());
    REQUIRE(outcome.value->active_slot() != nullptr);
    REQUIRE(outcome.value->secondary() == nullptr);
}

TEST_CASE("find_by_hash ignores slots that reported no hash", "[image][state]")
{
    Fixture fixture;

    Cbor cbor;
    cbor.map(1).text("images").array(1);
    put_minimal_slot(cbor, 0, "1.0.0");

    const auto outcome = fixture.state_from(cbor.view());

    REQUIRE(outcome.value.has_value());
    REQUIRE(outcome.value->find_by_hash(ImageHash{}) == nullptr);
}

// ---------------------------------------------------------------------------
// set_state and erase responses
// ---------------------------------------------------------------------------

TEST_CASE("set_state answers with the refreshed slot table", "[image][setstate]")
{
    Outcome<ImageState> outcome;

    Fixture fixture;

    SetStateRequest request;
    request.confirm = true;
    static_cast<void>(fixture.image.set_state(request, outcome.callback()));

    const auto golden = golden_state_response();
    fixture.respond(ConstBytes{golden});

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.value.has_value());
    REQUIRE(outcome.value->slots.size() == 2);
}

TEST_CASE("a rejected set_state surfaces the image group's own code", "[image][setstate]")
{
    Outcome<ImageState> outcome;

    Fixture fixture;

    SetStateRequest request;
    request.hash = ImageHash::from(Hash{});
    static_cast<void>(fixture.image.set_state(request, outcome.callback()));
    const auto payload = image_err(8); // IMG_MGMT_ERR_HASH_NOT_FOUND
    fixture.respond(ConstBytes{payload});

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::ProtocolError);
    REQUIRE(outcome.mgmt.has_value());
    REQUIRE(outcome.mgmt->group_scoped);
    REQUIRE(outcome.mgmt->group == Group::Image);
    REQUIRE(outcome.mgmt->rc == 8);
}

TEST_CASE("an erase response completes the command", "[image][erase]")
{
    VoidOutcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.image.erase(outcome.callback()));
    fixture.respond(ConstBytes{bytes_of({0xA0})});

    REQUIRE(outcome.calls == 1);
    REQUIRE_FALSE(outcome.code.has_value());
}

TEST_CASE("a legacy erase response carrying rc zero is a success", "[image][erase]")
{
    // CONFIG_MCUMGR_SMP_LEGACY_RC_BEHAVIOUR adds {"rc": 0} to a successful
    // response. Zero is success, not an error.
    VoidOutcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.image.erase(outcome.callback()));
    fixture.respond(ConstBytes{bytes_of({0xA1, 0x62, 'r', 'c', 0x00})});

    REQUIRE(outcome.calls == 1);
    REQUIRE_FALSE(outcome.code.has_value());
}

TEST_CASE("erasing a slot that is in use surfaces the device's refusal", "[image][erase]")
{
    // Over SMP v1 the server translates IMG_MGMT_ERR_NO_FREE_SLOT to
    // MGMT_ERR_EBADSTATE, which is what the specification's erase note
    // describes (protocol-notes section 9, A16).
    VoidOutcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.image.erase(outcome.callback()));
    fixture.respond(ConstBytes{bytes_of({0xA1, 0x62, 'r', 'c', 0x06})});

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::ProtocolError);
    REQUIRE(outcome.mgmt.has_value());
    REQUIRE_FALSE(outcome.mgmt->group_scoped);
}

// ---------------------------------------------------------------------------
// Slot info
// ---------------------------------------------------------------------------

TEST_CASE("a slot-info response decodes its nested arrays", "[image][slotinfo]")
{
    Outcome<SlotInfo> outcome;

    Fixture fixture;

    static_cast<void>(fixture.image.get_slot_info(outcome.callback()));

    Cbor cbor;
    cbor.map(1)
        .text("images")
        .array(1)
        .map(3)
        .text("image")
        .uint(0)
        .text("slots")
        .array(2)
        .map(2)
        .text("slot")
        .uint(0)
        .text("size")
        .uint(0x00060000)
        .map(3)
        .text("slot")
        .uint(1)
        .text("size")
        .uint(0x00060000)
        .text("upload_image_id")
        .uint(2)
        .text("max_image_size")
        .uint(0x0005F000);
    fixture.respond(cbor.view());

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.value.has_value());
    REQUIRE(outcome.value->images.size() == 1);
    const auto& entry = outcome.value->images.at(0);
    REQUIRE(entry.image == 0);
    REQUIRE(entry.max_image_size == 0x0005F000);
    REQUIRE(entry.slots.size() == 2);
    REQUIRE(entry.slots[0].slot == 0);
    REQUIRE(entry.slots[0].size == 0x00060000);
    REQUIRE_FALSE(entry.slots[0].upload_image_id.has_value());
    REQUIRE_FALSE(entry.slots[0].open_error.has_value());
    REQUIRE(entry.slots[1].upload_image_id == 2);
}

TEST_CASE("a slot that would not open reports its rc instead of a size", "[image][slotinfo]")
{
    // Undocumented in the specification: the server emits a per-slot "rc" in
    // place of "size" when flash_area_open() fails. Nested inside the slot map,
    // so it is never the message-level rc.
    Outcome<SlotInfo> outcome;

    Fixture fixture;

    static_cast<void>(fixture.image.get_slot_info(outcome.callback()));

    Cbor cbor;
    cbor.map(1)
        .text("images")
        .array(1)
        .map(2)
        .text("image")
        .uint(0)
        .text("slots")
        .array(1)
        .map(2)
        .text("slot")
        .uint(1)
        .text("rc")
        .nint(-2);
    fixture.respond(cbor.view());

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.value.has_value());
    const auto& descriptor = outcome.value->images.at(0).slots.at(0);
    REQUIRE(descriptor.slot == 1);
    REQUIRE_FALSE(descriptor.size.has_value());
    REQUIRE(descriptor.open_error == -2);
}

TEST_CASE("more slots than the bound is a bounded failure", "[image][slotinfo][hostile]")
{
    Outcome<SlotInfo> outcome;

    Fixture fixture;

    static_cast<void>(fixture.image.get_slot_info(outcome.callback()));

    Cbor cbor;
    cbor.map(1).text("images").array(1).map(2).text("image").uint(0).text("slots").array(
        smply::limits::kMaxSlotsPerImage + 1);
    for (std::size_t i = 0; i <= smply::limits::kMaxSlotsPerImage; ++i) {
        cbor.map(1).text("slot").uint(i);
    }
    fixture.respond(cbor.view());

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("a slot descriptor with no slot number is rejected", "[image][slotinfo][hostile]")
{
    Outcome<SlotInfo> outcome;

    Fixture fixture;

    static_cast<void>(fixture.image.get_slot_info(outcome.callback()));

    Cbor cbor;
    cbor.map(1).text("images").array(1).map(1).text("slots").array(1).map(1).text("size").uint(
        1024);
    fixture.respond(cbor.view());

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("an image with no slots array decodes as an empty one", "[image][slotinfo]")
{
    Outcome<SlotInfo> outcome;

    Fixture fixture;

    static_cast<void>(fixture.image.get_slot_info(outcome.callback()));

    Cbor cbor;
    cbor.map(1).text("images").array(1).map(1).text("image").uint(1);
    fixture.respond(cbor.view());

    REQUIRE(outcome.value.has_value());
    REQUIRE(outcome.value->images.at(0).image == 1);
    REQUIRE(outcome.value->images.at(0).slots.empty());
    REQUIRE_FALSE(outcome.value->images.at(0).max_image_size.has_value());
}

/// Issues get_slot_info and answers it with one image whose slots array holds
/// exactly the descriptor \p build writes.
template<class Build>
Outcome<SlotInfo> slot_info_from_descriptor(Fixture& fixture, Build build)
{
    Outcome<SlotInfo> outcome;
    static_cast<void>(fixture.image.get_slot_info(outcome.callback()));

    Cbor cbor;
    cbor.map(1).text("images").array(1).map(2).text("image").uint(0).text("slots").array(1);
    build(cbor);
    fixture.respond(cbor.view());
    return outcome;
}

TEST_CASE("a wrong-typed slot descriptor field is a decode failure", "[image][slotinfo][hostile]")
{
    Fixture fixture;

    const auto outcome = slot_info_from_descriptor(
        fixture, [](Cbor& cbor) { cbor.map(2).text("slot").uint(0).text("size").text("big"); });

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("slot-info numbers beyond 32 bits are rejected", "[image][slotinfo][hostile]")
{
    // Every count and size in this group is a uint32 on the wire. A larger one
    // is a device saying something smply will not act on.
    SECTION("slot number")
    {
        Fixture fixture;
        const auto outcome = slot_info_from_descriptor(
            fixture, [](Cbor& cbor) { cbor.map(1).text("slot").uint(0x1'0000'0000ULL); });
        REQUIRE(outcome.code == ErrorCode::CborDecode);
    }
    SECTION("slot size")
    {
        Fixture fixture;
        const auto outcome = slot_info_from_descriptor(fixture, [](Cbor& cbor) {
            cbor.map(2).text("slot").uint(0).text("size").uint(0x1'0000'0000ULL);
        });
        REQUIRE(outcome.code == ErrorCode::CborDecode);
    }
    SECTION("upload image id")
    {
        Fixture fixture;
        const auto outcome = slot_info_from_descriptor(fixture, [](Cbor& cbor) {
            cbor.map(2).text("slot").uint(0).text("upload_image_id").uint(0x1'0000'0000ULL);
        });
        REQUIRE(outcome.code == ErrorCode::CborDecode);
    }
    SECTION("per-slot rc")
    {
        Fixture fixture;
        const auto outcome = slot_info_from_descriptor(fixture, [](Cbor& cbor) {
            cbor.map(2).text("slot").uint(0).text("rc").nint(-2147483649LL);
        });
        REQUIRE(outcome.code == ErrorCode::CborDecode);
    }
}

TEST_CASE("a wrong-typed slot-info image field is a decode failure", "[image][slotinfo][hostile]")
{
    Outcome<SlotInfo> outcome;

    Fixture fixture;

    static_cast<void>(fixture.image.get_slot_info(outcome.callback()));

    Cbor cbor;
    cbor.map(1).text("images").array(1).map(1).text("image").text("zero");
    fixture.respond(cbor.view());

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("slot-info image numbers beyond 32 bits are rejected", "[image][slotinfo][hostile]")
{
    SECTION("image number")
    {
        Outcome<SlotInfo> outcome;

        Fixture fixture;

        static_cast<void>(fixture.image.get_slot_info(outcome.callback()));
        Cbor cbor;
        cbor.map(1).text("images").array(1).map(1).text("image").uint(0x1'0000'0000ULL);
        fixture.respond(cbor.view());

        REQUIRE(outcome.code == ErrorCode::CborDecode);
    }
    SECTION("max image size")
    {
        Outcome<SlotInfo> outcome;

        Fixture fixture;

        static_cast<void>(fixture.image.get_slot_info(outcome.callback()));
        Cbor cbor;
        cbor.map(1)
            .text("images")
            .array(1)
            .map(2)
            .text("image")
            .uint(0)
            .text("max_image_size")
            .uint(0x1'0000'0000ULL);
        fixture.respond(cbor.view());

        REQUIRE(outcome.code == ErrorCode::CborDecode);
    }
}

TEST_CASE("an unsupported slot-info command is an ordinary device error", "[image][slotinfo]")
{
    // A8: the whole command is optional. ENOTSUP is a normal outcome and the
    // caller falls back rather than failing an update.
    Outcome<SlotInfo> outcome;

    Fixture fixture;

    static_cast<void>(fixture.image.get_slot_info(outcome.callback()));
    fixture.respond(ConstBytes{bytes_of({0xA1, 0x62, 'r', 'c', 0x08})});

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::ProtocolError);
    REQUIRE(smply::smp_error(smply::Error{ErrorCode::ProtocolError, *outcome.mgmt}) ==
            SmpError::NotSupported);
}

// ---------------------------------------------------------------------------
// image_error()
// ---------------------------------------------------------------------------

TEST_CASE("image_error reads only a group-scoped image code", "[image][error]")
{
    using smply::Error;
    using smply::image_error;

    REQUIRE(image_error(Error{ErrorCode::ProtocolError, MgmtError::scoped(Group::Image, 8)}) ==
            ImageError::HashNotFound);
    // Another group's 8 means something else entirely.
    REQUIRE_FALSE(
        image_error(Error{ErrorCode::ProtocolError, MgmtError::scoped(Group::Os, 8)}).has_value());
    // A flat SMP rc is not a group code, even when the number matches.
    REQUIRE_FALSE(image_error(Error{ErrorCode::ProtocolError, MgmtError::smp(8)}).has_value());
    // No device report at all.
    REQUIRE_FALSE(image_error(Error{ErrorCode::Timeout}).has_value());
}

TEST_CASE("an unknown image code is carried through numerically", "[image][error]")
{
    // A2: the enumeration is append-only and grows per release, so a value
    // smply does not name must survive rather than be rejected.
    using smply::Error;
    using smply::image_error;

    const auto code =
        image_error(Error{ErrorCode::ProtocolError, MgmtError::scoped(Group::Image, 99)});
    REQUIRE(code.has_value());
    REQUIRE(static_cast<std::uint16_t>(*code) == 99);
}

// ---------------------------------------------------------------------------
// ImageVersion
// ---------------------------------------------------------------------------

TEST_CASE("a three-part version parses", "[image][version]")
{
    const auto parsed = ImageVersion::parse("1.2.3");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->major == 1);
    REQUIRE(parsed->minor == 2);
    REQUIRE(parsed->revision == 3);
    REQUIRE(parsed->build == 0);
    REQUIRE(parsed->to_string() == "1.2.3");
}

TEST_CASE("the device's dotted build number parses", "[image][version]")
{
    // img_mgmt_ver_str() writes "major.minor.revision" and appends ".build"
    // only when the build number is non-zero. This is what a response carries.
    const auto parsed = ImageVersion::parse("1.2.3.4");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->build == 4);
    REQUIRE(parsed->to_string() == "1.2.3.4");
}

TEST_CASE("imgtool's plus-separated build number parses", "[image][version]")
{
    // "1.2.3+4" is what a person types for imgtool; it is accepted, but
    // to_string() renders the device's form so a round trip is stable.
    const auto parsed = ImageVersion::parse("1.2.3+4");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->build == 4);
    REQUIRE(parsed->to_string() == "1.2.3.4");
}

TEST_CASE("the widest legal version parses", "[image][version]")
{
    const auto parsed = ImageVersion::parse("255.255.65535.4294967295");
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->major == 255);
    REQUIRE(parsed->minor == 255);
    REQUIRE(parsed->revision == 65535);
    REQUIRE(parsed->build == 4294967295U);
    REQUIRE(parsed->to_string() == "255.255.65535.4294967295");
}

TEST_CASE("malformed version strings are rejected", "[image][version]")
{
    const std::array<const char*, 15> rejected = {
        "",          // nothing at all
        R"(<???>)",  // what the device sends when it cannot format one
        "1",         // too few components
        "1.2",       //
        "1.2.",      // a separator with no component
        "1.2.3.",    //
        "1.2.3.4.5", // trailing junk
        "1.2.3x",    //
        " 1.2.3",    // leading space
        "+1.2.3",    // a sign
        "-1.2.3",    //
        "256.0.0",   // out of range for its component
        "0.256.0",   //
        "0.0.65536", //
        "1.2.3.4294967296",
    };
    for (const char* text : rejected) {
        INFO("version: " << text);
        const auto parsed = ImageVersion::parse(text);
        REQUIRE_FALSE(parsed.has_value());
        REQUIRE(parsed.error().code() == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("an over-long version string is refused", "[image][version]")
{
    const std::string too_long(smply::limits::kMaxVersionStringLength + 1, '1');
    REQUIRE_FALSE(ImageVersion::parse(too_long).has_value());
}

// ---------------------------------------------------------------------------
// ImageHash
// ---------------------------------------------------------------------------

TEST_CASE("an ImageHash carries its own length", "[image][hash]")
{
    const auto short_hash = ImageHash::from(ConstBytes{digest(32, 0x00)});
    const auto long_hash = ImageHash::from(ConstBytes{digest(64, 0x00)});
    REQUIRE(short_hash.has_value());
    REQUIRE(long_hash.has_value());
    REQUIRE(short_hash->size() == 32);
    REQUIRE(long_hash->size() == 64);
    // A shorter hash is not equal to a longer one that starts with it.
    REQUIRE_FALSE(*short_hash == *long_hash);
    REQUIRE(*short_hash == *ImageHash::from(ConstBytes{digest(32, 0x00)}));
}

TEST_CASE("a 32-byte Hash converts without failing", "[image][hash]")
{
    // The conversion exists so a hash read out of a file's TLVs can be compared
    // with what a device reports -- not so the upload sha can be passed here.
    Hash raw{};
    raw[0] = std::byte{0xAB};
    const ImageHash hash = ImageHash::from(raw);
    REQUIRE(hash.size() == 32);
    REQUIRE(hash.bytes()[0] == std::byte{0xAB});
}

TEST_CASE("a default ImageHash is empty", "[image][hash]")
{
    const ImageHash hash;
    REQUIRE(hash.empty());
    REQUIRE(hash.bytes().empty());
}

TEST_CASE("an over-long hash is refused", "[image][hash]")
{
    const auto hash = ImageHash::from(ConstBytes{digest(smply::limits::kMaxImageHashLength + 1)});
    REQUIRE_FALSE(hash.has_value());
    REQUIRE(hash.error().code() == ErrorCode::CborDecode);
}
