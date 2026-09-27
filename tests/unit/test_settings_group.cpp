// SPDX-License-Identifier: Apache-2.0
//
// The settings group, group 3 (docs/protocol-notes.md section 11).
//
// Request vectors are hand-derived from the CBOR grammar and the field names in
// the protocol notes. Every command's operation and ID is checked explicitly,
// because two IDs are shared between a read and a write handler and the
// operation is all that tells them apart.

#include "smply/groups/settings.hpp"

#include "cbor_shapes.hpp"
#include "fake_transport.hpp"
#include "manual_clock.hpp"
#include "message_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using smply::ConstBytes;
using smply::ErrorCode;
using smply::Group;
using smply::Header;
using smply::MgmtError;
using smply::Operation;
using smply::RequestHandle;
using smply::Result;
using smply::SettingsError;
using smply::SettingsManagement;
using smply::SettingValue;
using smply::SmpClient;
using smply::SmpClientConfig;
using smply::SmpError;
using smply::Version;
using smply::test::bytes_of;
using smply::test::Encoding;
using smply::test::FakeTransport;
using smply::test::filler;
using smply::test::make_message;
using smply::test::ManualClock;
using smply::test::Shape;

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

/// Collects one command's outcome.
template<class T>
struct Outcome
{
    int calls = 0;
    std::optional<T> value;
    std::optional<smply::Error> error;

    [[nodiscard]] auto callback()
    {
        return [this](Result<T> result) {
            ++calls;
            if (result.has_value()) {
                value = std::move(*result);
            } else {
                error = result.error();
            }
        };
    }

    [[nodiscard]] std::optional<ErrorCode> code() const
    {
        return error.has_value() ? std::optional{error->code()} : std::nullopt;
    }
};

/// `Result<void>` has no value to capture.
struct VoidOutcome
{
    int calls = 0;
    std::optional<smply::Error> error;

    [[nodiscard]] auto callback()
    {
        return [this](Result<void> result) {
            ++calls;
            if (!result.has_value()) {
                error = result.error();
            }
        };
    }

    [[nodiscard]] bool succeeded() const
    {
        return calls == 1 && !error.has_value();
    }

    [[nodiscard]] std::optional<ErrorCode> code() const
    {
        return error.has_value() ? std::optional{error->code()} : std::nullopt;
    }
};

/// Transport, clock, client and group. Declare anything a callback captures
/// before this: ~SmpClient completes outstanding requests.
struct Fixture
{
    FakeTransport transport;
    ManualClock clock;
    SmpClient client;
    SettingsManagement settings;

    explicit Fixture(SmpClientConfig config = {})
        : client{transport, clock, config}, settings{client}
    {}

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

    void respond(ConstBytes payload)
    {
        const Header request = sent_header();
        const Header reply{.op = smply::response_to(request.op),
                           .version = request.version,
                           .flags = 0,
                           .length = 0,
                           .group = request.group,
                           .seq = request.seq,
                           .command = request.command};
        const auto message = make_message(reply, payload);
        transport.deliver(ConstBytes{message});
    }

    void respond_empty()
    {
        respond(ConstBytes{bytes_of({0xA0})});
    }
};

/// {"val": value} and optionally "max_size".
Shape read_response(Encoding encoding, ConstBytes value,
                    std::optional<std::uint64_t> max_size = std::nullopt)
{
    Shape doc{encoding};
    doc.map(max_size.has_value() ? 2 : 1).text("val");
    doc.raw().blob(value);
    if (max_size.has_value()) {
        doc.text("max_size").uint(*max_size);
    }
    doc.end();
    return doc;
}

Shape scoped_error(Encoding encoding, std::uint64_t group, std::uint64_t rc)
{
    Shape doc{encoding};
    doc.map(1).text("err").map(2).text("group").uint(group).text("rc").uint(rc).end().end();
    return doc;
}

std::vector<std::byte> flat_error(std::uint8_t rc)
{
    return bytes_of({0xA1, 0x62, 0x72, 0x63, rc});
}

/// The bytes a map key or short text value encodes to: a one-byte header and
/// the characters. Only for strings under 24 bytes.
std::vector<std::byte> text_item(std::string_view text)
{
    REQUIRE(text.size() < 24);
    std::vector<std::byte> out{static_cast<std::byte>(0x60 + text.size())};
    for (const char ch : text) {
        out.push_back(static_cast<std::byte>(ch));
    }
    return out;
}

std::vector<std::byte> concat(std::initializer_list<std::vector<std::byte>> parts)
{
    std::vector<std::byte> out;
    for (const auto& part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

/// Every setting-name command, so the name rules are tested once for all.
enum class NamedCommand : std::uint8_t
{
    Read,
    ReadWithSize,
    Write,
    Erase,
    Save,
};

RequestHandle issue(Fixture& fixture, NamedCommand command, std::string_view name,
                    VoidOutcome& done, Outcome<SettingValue>& read)
{
    switch (command) {
    case NamedCommand::Read:
        return fixture.settings.read(name, read.callback());
    case NamedCommand::ReadWithSize:
        return fixture.settings.read(name, 16, read.callback());
    case NamedCommand::Write:
        return fixture.settings.write(name, ConstBytes{bytes_of({1})}, done.callback());
    case NamedCommand::Erase:
        return fixture.settings.erase(name, done.callback());
    case NamedCommand::Save:
        return fixture.settings.save(name, done.callback());
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// Encoding: operation, command and payload of every command
// ---------------------------------------------------------------------------

TEST_CASE("read is a read of command 0 carrying the name", "[settings][encoding]")
{
    Outcome<SettingValue> outcome;
    Fixture fixture;
    static_cast<void>(fixture.settings.read("app/foo", outcome.callback()));

    const Header header = fixture.sent_header();
    CHECK(header.op == Operation::Read);
    CHECK(header.group == Group::Settings);
    CHECK(header.command == 0);
    // {"name": "app/foo"} -- and no "max_size" at all.
    CHECK(fixture.sent_payload() ==
          concat({bytes_of({0xA1}), text_item("name"), text_item("app/foo")}));
}

TEST_CASE("read with a size adds max_size as an unsigned integer", "[settings][encoding]")
{
    Outcome<SettingValue> outcome;
    Fixture fixture;
    static_cast<void>(fixture.settings.read("app/foo", 200, outcome.callback()));

    // {"name": "app/foo", "max_size": 200} -- 200 needs the one-byte extension.
    CHECK(fixture.sent_payload() ==
          concat({bytes_of({0xA2}), text_item("name"), text_item("app/foo"), text_item("max_size"),
                  bytes_of({0x18, 0xC8})}));
}

TEST_CASE("write is a write of command 0 carrying the value as bytes", "[settings][encoding]")
{
    VoidOutcome outcome;
    Fixture fixture;
    // Embedded zeros: the value is binary, never a C string.
    const auto value = bytes_of({0x00, 0x7F, 0x00, 0xFF});
    static_cast<void>(fixture.settings.write("app/foo", ConstBytes{value}, outcome.callback()));

    const Header header = fixture.sent_header();
    CHECK(header.op == Operation::Write);
    CHECK(header.group == Group::Settings);
    CHECK(header.command == 0);
    // {"name": "app/foo", "val": h'007F00FF'}
    CHECK(fixture.sent_payload() ==
          concat({bytes_of({0xA2}), text_item("name"), text_item("app/foo"), text_item("val"),
                  bytes_of({0x44, 0x00, 0x7F, 0x00, 0xFF})}));
}

TEST_CASE("an empty value is written as an empty byte string", "[settings][encoding]")
{
    VoidOutcome outcome;
    Fixture fixture;
    static_cast<void>(fixture.settings.write("a", ConstBytes{}, outcome.callback()));

    CHECK(fixture.sent_payload() == concat({bytes_of({0xA2}), text_item("name"), text_item("a"),
                                            text_item("val"), bytes_of({0x40})}));
}

TEST_CASE("erase is a write of command 1 carrying the name", "[settings][encoding]")
{
    VoidOutcome outcome;
    Fixture fixture;
    static_cast<void>(fixture.settings.erase("app/foo", outcome.callback()));

    const Header header = fixture.sent_header();
    CHECK(header.op == Operation::Write); // registered write-only
    CHECK(header.command == 1);
    CHECK(fixture.sent_payload() ==
          concat({bytes_of({0xA1}), text_item("name"), text_item("app/foo")}));
}

TEST_CASE("commit is a write of command 2 with an empty map", "[settings][encoding]")
{
    VoidOutcome outcome;
    Fixture fixture;
    static_cast<void>(fixture.settings.commit(outcome.callback()));

    const Header header = fixture.sent_header();
    CHECK(header.op == Operation::Write); // registered write-only
    CHECK(header.group == Group::Settings);
    CHECK(header.command == 2);
    CHECK(fixture.sent_payload() == bytes_of({0xA0}));
}

TEST_CASE("load is a read of command 3 with an empty map", "[settings][encoding]")
{
    VoidOutcome outcome;
    Fixture fixture;
    static_cast<void>(fixture.settings.load(outcome.callback()));

    const Header header = fixture.sent_header();
    CHECK(header.op == Operation::Read);
    CHECK(header.command == 3);
    CHECK(fixture.sent_payload() == bytes_of({0xA0}));
}

TEST_CASE("save everything is a write of command 3 with no name at all", "[settings][encoding]")
{
    VoidOutcome outcome;
    Fixture fixture;
    static_cast<void>(fixture.settings.save(outcome.callback()));

    const Header header = fixture.sent_header();
    CHECK(header.op == Operation::Write);
    CHECK(header.command == 3);
    CHECK(fixture.sent_payload() == bytes_of({0xA0}));
}

TEST_CASE("save of a subtree carries its name", "[settings][encoding]")
{
    VoidOutcome outcome;
    Fixture fixture;
    static_cast<void>(fixture.settings.save("app", outcome.callback()));

    const Header header = fixture.sent_header();
    CHECK(header.op == Operation::Write);
    CHECK(header.command == 3);
    CHECK(fixture.sent_payload() ==
          concat({bytes_of({0xA1}), text_item("name"), text_item("app")}));
}

TEST_CASE("the longest legal name and value are sent whole", "[settings][encoding][limits]")
{
    VoidOutcome outcome;
    Fixture fixture;
    const std::string name(smply::limits::kMaxSettingNameLength, 'n');
    const auto value = filler(smply::limits::kMaxSettingValueLength);
    REQUIRE(fixture.settings.write(name, ConstBytes{value}, outcome.callback()).valid());

    const auto payload = fixture.sent_payload();
    // map, "name", 78 len, name, "val", 59 01 00, value
    REQUIRE(payload.size() == 1 + 5 + 2 + name.size() + 4 + 3 + value.size());
    const std::vector<std::byte> tail{payload.end() - static_cast<std::ptrdiff_t>(value.size()),
                                      payload.end()};
    CHECK(tail == value);
}

// ---------------------------------------------------------------------------
// Argument validation
// ---------------------------------------------------------------------------

TEST_CASE("an invalid setting name is refused before anything is sent", "[settings][arguments]")
{
    const std::string over(smply::limits::kMaxSettingNameLength + 1, 'n');
    const std::string with_nul{"app\0foo", 7};
    const std::string name = GENERATE_COPY(std::string{}, over, with_nul);
    const NamedCommand command =
        GENERATE(NamedCommand::Read, NamedCommand::ReadWithSize, NamedCommand::Write,
                 NamedCommand::Erase, NamedCommand::Save);

    VoidOutcome done;
    Outcome<SettingValue> read;
    Fixture fixture;

    CHECK_FALSE(issue(fixture, command, name, done, read).valid());
    CHECK(fixture.transport.send_count() == 0);
    CHECK(done.calls + read.calls == 0); // never inside the call

    fixture.client.poll(fixture.clock.now());
    CHECK(done.calls + read.calls == 1);
    CHECK((done.code() == ErrorCode::InvalidArgument || read.code() == ErrorCode::InvalidArgument));
}

TEST_CASE("a name of exactly the limit is accepted by every command", "[settings][arguments]")
{
    const NamedCommand command =
        GENERATE(NamedCommand::Read, NamedCommand::ReadWithSize, NamedCommand::Write,
                 NamedCommand::Erase, NamedCommand::Save);
    VoidOutcome done;
    Outcome<SettingValue> read;
    Fixture fixture;

    CHECK(
        issue(fixture, command, std::string(smply::limits::kMaxSettingNameLength, 'n'), done, read)
            .valid());
    CHECK(fixture.transport.send_count() == 1);
}

TEST_CASE("a write value is bounded by kMaxSettingValueLength", "[settings][arguments][limits]")
{
    const std::size_t size =
        GENERATE(smply::limits::kMaxSettingValueLength, smply::limits::kMaxSettingValueLength + 1);
    VoidOutcome outcome;
    Fixture fixture;
    const auto value = filler(size);

    const RequestHandle handle = fixture.settings.write("a", ConstBytes{value}, outcome.callback());
    if (size <= smply::limits::kMaxSettingValueLength) {
        CHECK(handle.valid());
    } else {
        CHECK_FALSE(handle.valid());
        CHECK(fixture.transport.send_count() == 0);
        CHECK(outcome.calls == 0);
        fixture.client.poll(fixture.clock.now());
        CHECK(outcome.code() == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("a read's max_size must be between 1 and kMaxSettingValueLength",
          "[settings][arguments][limits]")
{
    const std::uint32_t max_size =
        GENERATE(std::uint32_t{0}, std::uint32_t{1},
                 static_cast<std::uint32_t>(smply::limits::kMaxSettingValueLength),
                 static_cast<std::uint32_t>(smply::limits::kMaxSettingValueLength + 1), UINT32_MAX);
    Outcome<SettingValue> outcome;
    Fixture fixture;

    const RequestHandle handle = fixture.settings.read("a", max_size, outcome.callback());
    const bool legal = max_size >= 1 && max_size <= smply::limits::kMaxSettingValueLength;
    CHECK(handle.valid() == legal);
    CHECK(fixture.transport.send_count() == (legal ? 1U : 0U));
    if (!legal) {
        CHECK(outcome.calls == 0);
        fixture.client.poll(fixture.clock.now());
        CHECK(outcome.code() == ErrorCode::InvalidArgument);
    }
}

// ---------------------------------------------------------------------------
// Read responses
// ---------------------------------------------------------------------------

TEST_CASE("a read returns the value's bytes exactly", "[settings][read]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    const auto value = bytes_of({0x00, 0x01, 0x00, 0xFE, 0x00});

    Outcome<SettingValue> outcome;
    Fixture fixture;
    static_cast<void>(fixture.settings.read("app/foo", outcome.callback()));
    CHECK(outcome.calls == 0);
    fixture.respond(read_response(encoding, ConstBytes{value}).view());

    REQUIRE(outcome.value.has_value());
    CHECK(outcome.value->value == value);
    CHECK_FALSE(outcome.value->max_size.has_value());
}

TEST_CASE("an empty value is a successful read", "[settings][read]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    Outcome<SettingValue> outcome;
    Fixture fixture;
    static_cast<void>(fixture.settings.read("app/foo", outcome.callback()));
    fixture.respond(read_response(encoding, ConstBytes{}).view());

    REQUIRE(outcome.value.has_value());
    CHECK(outcome.value->value.empty());
}

TEST_CASE("the device's own read limit is reported when it capped the read", "[settings][read]")
{
    // Asked for 100, the device's VALUE_LEN is 32: it reads 32 and says so.
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    const auto value = filler(32);
    Outcome<SettingValue> outcome;
    Fixture fixture;
    static_cast<void>(fixture.settings.read("app/foo", 100, outcome.callback()));
    fixture.respond(read_response(encoding, ConstBytes{value}, 32).view());

    REQUIRE(outcome.value.has_value());
    CHECK(outcome.value->value == value);
    CHECK(outcome.value->max_size == 32U);
}

TEST_CASE("a reported max_size beyond 32 bits is refused", "[settings][read][hostile]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    const std::uint64_t reported =
        GENERATE(std::uint64_t{0xFFFFFFFFU}, std::uint64_t{0x100000000U});
    Outcome<SettingValue> outcome;
    Fixture fixture;
    static_cast<void>(fixture.settings.read("a", outcome.callback()));
    fixture.respond(read_response(encoding, ConstBytes{}, reported).view());

    if (reported <= 0xFFFFFFFFU) {
        REQUIRE(outcome.value.has_value());
        CHECK(outcome.value->max_size == 0xFFFFFFFFU);
    } else {
        CHECK(outcome.code() == ErrorCode::CborDecode);
    }
}

TEST_CASE("a read reply is bounded by the size asked for", "[settings][read][limits][hostile]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);

    SECTION("without max_size, by kMaxSettingValueLength")
    {
        const std::size_t size = GENERATE(smply::limits::kMaxSettingValueLength,
                                          smply::limits::kMaxSettingValueLength + 1);
        const auto value = filler(size);
        Outcome<SettingValue> outcome;
        Fixture fixture;
        static_cast<void>(fixture.settings.read("a", outcome.callback()));
        fixture.respond(read_response(encoding, ConstBytes{value}).view());
        if (size <= smply::limits::kMaxSettingValueLength) {
            REQUIRE(outcome.value.has_value());
            CHECK(outcome.value->value == value);
        } else {
            CHECK(outcome.code() == ErrorCode::CborDecode);
        }
    }
    SECTION("with max_size, by max_size")
    {
        const std::size_t size = GENERATE(std::size_t{8}, std::size_t{9});
        Outcome<SettingValue> outcome;
        Fixture fixture;
        static_cast<void>(fixture.settings.read("a", 8, outcome.callback()));
        fixture.respond(read_response(encoding, ConstBytes{filler(size)}).view());
        if (size <= 8) {
            CHECK(outcome.value.has_value());
        } else {
            // More than was asked for is not an answer to the question.
            CHECK(outcome.code() == ErrorCode::CborDecode);
        }
    }
}

TEST_CASE("a malformed read reply is a decode failure", "[settings][read][hostile]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    Shape doc{encoding};
    const int variant = GENERATE(0, 1, 2, 3);
    switch (variant) {
    case 0: // "val" absent
        doc.map(0).end();
        break;
    case 1: // "val" as text: a value is bytes, and text is not accepted in its place
        doc.map(1).text("val").text("abc").end();
        break;
    case 2: // "max_size" negative
        doc.map(2).text("val");
        doc.raw().blob(ConstBytes{});
        doc.text("max_size");
        doc.raw().nint(-1);
        doc.end();
        break;
    default: // "max_size" as text
        doc.map(2).text("val");
        doc.raw().blob(ConstBytes{});
        doc.text("max_size").text("32").end();
        break;
    }

    Outcome<SettingValue> outcome;
    Fixture fixture;
    static_cast<void>(fixture.settings.read("a", outcome.callback()));
    fixture.respond(doc.view());

    CHECK(outcome.calls == 1);
    CHECK(outcome.code() == ErrorCode::CborDecode);
}

TEST_CASE("unknown keys beside a read reply are ignored", "[settings][read]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    Shape doc{encoding};
    doc.map(3).text("rc").uint(0).text("future").array(0).end().text("val");
    doc.raw().blob(ConstBytes{bytes_of({7})});
    doc.end();

    Outcome<SettingValue> outcome;
    Fixture fixture;
    static_cast<void>(fixture.settings.read("a", outcome.callback()));
    fixture.respond(doc.view());

    REQUIRE(outcome.value.has_value());
    CHECK(outcome.value->value == bytes_of({7}));
}

TEST_CASE("every truncation of a read reply is handled", "[settings][read][hostile]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    const Shape doc = read_response(encoding, ConstBytes{bytes_of({1, 2, 3})}, 32);
    const ConstBytes whole = doc.view();

    for (std::size_t length = 0; length < whole.size(); ++length) {
        Outcome<SettingValue> outcome;
        Fixture fixture;
        static_cast<void>(fixture.settings.read("a", outcome.callback()));
        fixture.respond(whole.first(length));
        INFO("length " << length);
        CHECK(outcome.calls == 1);
        CHECK_FALSE(outcome.value.has_value());
    }
}

// ---------------------------------------------------------------------------
// Commands with nothing to decode
// ---------------------------------------------------------------------------

TEST_CASE("every command without a value completes on an empty map", "[settings]")
{
    VoidOutcome outcome;
    Fixture fixture;
    const int command = GENERATE(range(0, 6));
    switch (command) {
    case 0:
        static_cast<void>(fixture.settings.write("a", ConstBytes{}, outcome.callback()));
        break;
    case 1:
        static_cast<void>(fixture.settings.erase("a", outcome.callback()));
        break;
    case 2:
        static_cast<void>(fixture.settings.commit(outcome.callback()));
        break;
    case 3:
        static_cast<void>(fixture.settings.load(outcome.callback()));
        break;
    case 4:
        static_cast<void>(fixture.settings.save(outcome.callback()));
        break;
    default:
        static_cast<void>(fixture.settings.save("a", outcome.callback()));
        break;
    }
    CHECK(outcome.calls == 0);
    fixture.respond_empty();
    CHECK(outcome.succeeded());
}

TEST_CASE("a legacy rc of zero is a success", "[settings]")
{
    VoidOutcome outcome;
    Fixture fixture;
    static_cast<void>(fixture.settings.commit(outcome.callback()));
    fixture.respond(ConstBytes{flat_error(0)});
    CHECK(outcome.succeeded());
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

TEST_CASE("a settings error over SMP v2 keeps its group code", "[settings][errors]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    Outcome<SettingValue> outcome;
    Fixture fixture{SmpClientConfig{.smp_version = Version::V2}};
    static_cast<void>(fixture.settings.read("app/missing", outcome.callback()));
    fixture.respond(scoped_error(encoding, 3, 3).view());

    REQUIRE(outcome.error.has_value());
    CHECK(outcome.error->code() == ErrorCode::ProtocolError);
    CHECK(outcome.error->mgmt() == MgmtError::scoped(Group::Settings, 3));
    CHECK(smply::settings_error(*outcome.error) == SettingsError::KeyNotFound);
    CHECK_FALSE(smply::smp_error(*outcome.error).has_value());
}

TEST_CASE("a settings error over SMP v1 arrives translated and without its group",
          "[settings][errors]")
{
    // ORIGINAL_PROTOCOL: KEY_TOO_LONG -> EINVAL, KEY_NOT_FOUND -> ENOENT, and
    // the rest to EUNKNOWN. None of them names the group.
    const std::uint8_t rc = GENERATE(std::uint8_t{1}, std::uint8_t{3}, std::uint8_t{5});
    VoidOutcome outcome;
    Fixture fixture;
    static_cast<void>(fixture.settings.write("app/foo", ConstBytes{}, outcome.callback()));
    fixture.respond(ConstBytes{flat_error(rc)});

    REQUIRE(outcome.error.has_value());
    CHECK(outcome.error->code() == ErrorCode::ProtocolError);
    CHECK(smply::smp_error(*outcome.error) == static_cast<SmpError>(rc));
    CHECK_FALSE(smply::settings_error(*outcome.error).has_value());
}

TEST_CASE("an access hook's refusal in another group is not a settings error", "[settings][errors]")
{
    // A32: the settings access hook may answer with any group's code.
    VoidOutcome outcome;
    Fixture fixture{SmpClientConfig{.smp_version = Version::V2}};
    static_cast<void>(fixture.settings.commit(outcome.callback()));
    fixture.respond(scoped_error(Encoding::Indefinite, 64, 3).view());

    REQUIRE(outcome.error.has_value());
    CHECK(outcome.error->mgmt() == MgmtError::scoped(Group::PerUser, 3));
    CHECK_FALSE(smply::settings_error(*outcome.error).has_value());
}

TEST_CASE("settings_error reads only the settings group's codes", "[settings][errors]")
{
    using smply::Error;
    CHECK_FALSE(smply::settings_error(Error{}).has_value());
    CHECK_FALSE(smply::settings_error(Error{ErrorCode::Timeout}).has_value());
    CHECK_FALSE(
        smply::settings_error(Error{ErrorCode::ProtocolError, MgmtError::smp(2)}).has_value());
    CHECK_FALSE(
        smply::settings_error(Error{ErrorCode::ProtocolError, MgmtError::scoped(Group::Stat, 2)})
            .has_value());
    CHECK(smply::settings_error(
              Error{ErrorCode::ProtocolError, MgmtError::scoped(Group::Settings, 2)}) ==
          SettingsError::KeyTooLong);
    CHECK(smply::settings_error(
              Error{ErrorCode::ProtocolError, MgmtError::scoped(Group::Settings, 9)}) ==
          SettingsError::SaveFailedValueTooLongToRead);
    const auto future = smply::settings_error(
        Error{ErrorCode::ProtocolError, MgmtError::scoped(Group::Settings, 77)});
    REQUIRE(future.has_value());
    CHECK(static_cast<std::uint16_t>(*future) == 77);
}

// ---------------------------------------------------------------------------
// Inherited from the client
// ---------------------------------------------------------------------------

TEST_CASE("a settings request times out like any other", "[settings]")
{
    VoidOutcome outcome;
    Fixture fixture;
    static_cast<void>(fixture.settings.save(outcome.callback()));

    fixture.clock.advance(std::chrono::seconds{10});
    fixture.client.poll(fixture.clock.now());

    CHECK(outcome.calls == 1);
    CHECK(outcome.code() == ErrorCode::Timeout);
}

TEST_CASE("a cancelled settings request completes as Cancelled on the next poll", "[settings]")
{
    Outcome<SettingValue> outcome;
    Fixture fixture;
    const RequestHandle handle = fixture.settings.read("a", outcome.callback());

    fixture.client.cancel(handle);
    CHECK(outcome.calls == 0);
    fixture.client.poll(fixture.clock.now());
    CHECK(outcome.code() == ErrorCode::Cancelled);

    fixture.respond(read_response(Encoding::Definite, ConstBytes{}).view());
    fixture.client.poll(fixture.clock.now());
    CHECK(outcome.calls == 1);
}

TEST_CASE("a settings request on a dropped link fails without being sent", "[settings]")
{
    VoidOutcome outcome;
    Fixture fixture;
    fixture.transport.disconnect();

    CHECK_FALSE(fixture.settings.load(outcome.callback()).valid());
    CHECK(outcome.calls == 0);
    fixture.client.poll(fixture.clock.now());
    CHECK(outcome.code() == ErrorCode::Disconnected);
}

TEST_CASE("a null settings callback is accepted and simply not invoked", "[settings]")
{
    SECTION("read")
    {
        Fixture fixture;
        static_cast<void>(fixture.settings.read("a", nullptr));
        fixture.respond(read_response(Encoding::Definite, ConstBytes{}).view());
    }
    SECTION("commit")
    {
        Fixture fixture;
        static_cast<void>(fixture.settings.commit(nullptr));
        fixture.respond_empty();
    }
    SECTION("rejected")
    {
        Fixture fixture;
        static_cast<void>(fixture.settings.erase("", nullptr));
        fixture.client.poll(fixture.clock.now());
    }
    SUCCEED(); // reaching here without a crash is the assertion
}
