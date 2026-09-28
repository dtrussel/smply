// SPDX-License-Identifier: Apache-2.0
//
// The statistics group, group 2 (docs/protocol-notes.md section 10).
//
// Request vectors are hand-derived from the CBOR grammar and the field names in
// the protocol notes, not copied from what the writer emitted. Every response
// is built once with `Shape` and run in both container encodings, because a
// Zephyr device sends indefinite-length ones (A18).

#include "smply/groups/statistics.hpp"

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
#include <limits>
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
using smply::SmpClient;
using smply::SmpClientConfig;
using smply::SmpError;
using smply::StatisticsError;
using smply::StatisticsField;
using smply::StatisticsGroup;
using smply::StatisticsManagement;
using smply::Version;
using smply::test::bytes_of;
using smply::test::Encoding;
using smply::test::FakeTransport;
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

using Names = std::vector<std::string>;

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

/// Transport, clock, client and group. Declare anything a callback captures
/// before this: ~SmpClient completes outstanding requests.
struct Fixture
{
    FakeTransport transport;
    ManualClock clock;
    SmpClient client;
    StatisticsManagement stats;

    explicit Fixture(SmpClientConfig config = {}) : client{transport, clock, config}, stats{client}
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

    /// A response that correctly answers the last request sent.
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
};

/// {"err": {"group": group, "rc": rc}}, the SMP v2 error shape.
Shape scoped_error(Encoding encoding, std::uint64_t group, std::uint64_t rc)
{
    Shape doc{encoding};
    doc.map(1).text("err").map(2).text("group").uint(group).text("rc").uint(rc).end().end();
    return doc;
}

/// {"rc": rc}, the flat SMP v1 error shape.
std::vector<std::byte> flat_error(std::uint8_t rc)
{
    return bytes_of({0xA1, 0x62, 0x72, 0x63, rc});
}

/// {"stat_list": [names...]}
Shape list_response(Encoding encoding, const Names& names)
{
    Shape doc{encoding};
    doc.map(1).text("stat_list").array(names.size());
    for (const std::string& name : names) {
        doc.text(name);
    }
    doc.end().end();
    return doc;
}

/// {"name": name, "fields": {fields...}}
Shape group_response(Encoding encoding, std::string_view name,
                     const std::vector<StatisticsField>& fields)
{
    Shape doc{encoding};
    doc.map(2).text("name").text(name).text("fields").map(fields.size());
    for (const StatisticsField& field : fields) {
        doc.text(field.name).uint(field.value);
    }
    doc.end().end();
    return doc;
}

} // namespace

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

TEST_CASE("list groups is a read of an empty map", "[stat][encoding]")
{
    Outcome<Names> outcome;
    Fixture fixture;

    static_cast<void>(fixture.stats.list_groups(outcome.callback()));

    const Header header = fixture.sent_header();
    CHECK(header.op == Operation::Read); // registered read-only
    CHECK(header.group == Group::Stat);
    CHECK(header.command == 1);
    CHECK(fixture.sent_payload() == bytes_of({0xA0}));
}

TEST_CASE("read group carries the name under \"name\"", "[stat][encoding]")
{
    Outcome<StatisticsGroup> outcome;
    Fixture fixture;

    static_cast<void>(fixture.stats.read_group("net", outcome.callback()));

    const Header header = fixture.sent_header();
    CHECK(header.op == Operation::Read);
    CHECK(header.group == Group::Stat);
    CHECK(header.command == 0);
    // A1  map(1)  64 "name"  63 "net"
    CHECK(fixture.sent_payload() ==
          bytes_of({0xA1, 0x64, 0x6E, 0x61, 0x6D, 0x65, 0x63, 0x6E, 0x65, 0x74}));
}

TEST_CASE("a group name of exactly the limit is sent whole", "[stat][encoding][limits]")
{
    Outcome<StatisticsGroup> outcome;
    Fixture fixture;

    const std::string at_limit(smply::limits::kMaxStatisticsNameLength, 'g');
    REQUIRE(fixture.stats.read_group(at_limit, outcome.callback()).valid());

    const auto payload = fixture.sent_payload();
    // Map, "name", a two-byte text header (0x78, length), then the name.
    REQUIRE(payload.size() == 1 + 5 + 2 + at_limit.size());
    CHECK(payload[6] == std::byte{0x78});
    CHECK(payload[7] == std::byte{smply::limits::kMaxStatisticsNameLength});
}

TEST_CASE("an SMP v2 client sends its requests as v2", "[stat][encoding]")
{
    Outcome<Names> outcome;
    Fixture fixture{SmpClientConfig{.smp_version = Version::V2}};

    static_cast<void>(fixture.stats.list_groups(outcome.callback()));
    CHECK(fixture.sent_header().version == Version::V2);
}

// ---------------------------------------------------------------------------
// Argument validation
// ---------------------------------------------------------------------------

TEST_CASE("an invalid group name is refused before anything is sent", "[stat][arguments]")
{
    const std::string over(smply::limits::kMaxStatisticsNameLength + 1, 'g');
    const std::string with_nul{"ne\0t", 4};
    const std::string name = GENERATE_COPY(std::string{}, over, with_nul);

    Outcome<StatisticsGroup> outcome;
    Fixture fixture;

    const RequestHandle handle = fixture.stats.read_group(name, outcome.callback());
    CHECK_FALSE(handle.valid());
    CHECK(fixture.transport.send_count() == 0);
    // Refused, but never inside the call.
    CHECK(outcome.calls == 0);

    fixture.client.poll(fixture.clock.now());
    CHECK(outcome.calls == 1);
    CHECK(outcome.code() == ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// List groups
// ---------------------------------------------------------------------------

TEST_CASE("a group list is returned in the device's order", "[stat][list]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    const Names names = GENERATE(Names{}, Names{"ble"}, Names{"ble_ll", "net", "smp_svr"});

    Outcome<Names> outcome;
    Fixture fixture;

    static_cast<void>(fixture.stats.list_groups(outcome.callback()));
    CHECK(outcome.calls == 0); // nothing completes before the response
    fixture.respond(list_response(encoding, names).view());

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.value.has_value());
    CHECK(*outcome.value == names);
}

TEST_CASE("a list of exactly kMaxStatisticsGroups is accepted", "[stat][list][limits]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    Names names;
    for (std::size_t i = 0; i < smply::limits::kMaxStatisticsGroups; ++i) {
        names.push_back("g" + std::to_string(i));
    }

    Outcome<Names> outcome;
    Fixture fixture;
    static_cast<void>(fixture.stats.list_groups(outcome.callback()));
    fixture.respond(list_response(encoding, names).view());

    REQUIRE(outcome.value.has_value());
    CHECK(outcome.value->size() == smply::limits::kMaxStatisticsGroups);
}

TEST_CASE("a list over kMaxStatisticsGroups is refused", "[stat][list][limits][hostile]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    const Names names(smply::limits::kMaxStatisticsGroups + 1, "g");

    Outcome<Names> outcome;
    Fixture fixture;
    static_cast<void>(fixture.stats.list_groups(outcome.callback()));
    fixture.respond(list_response(encoding, names).view());

    CHECK(outcome.code() == ErrorCode::CborDecode);
}

TEST_CASE("a listed group name is bounded", "[stat][list][limits][hostile]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    const std::size_t length = GENERATE(smply::limits::kMaxStatisticsNameLength,
                                        smply::limits::kMaxStatisticsNameLength + 1);

    Outcome<Names> outcome;
    Fixture fixture;
    static_cast<void>(fixture.stats.list_groups(outcome.callback()));
    fixture.respond(list_response(encoding, Names{std::string(length, 'n')}).view());

    if (length <= smply::limits::kMaxStatisticsNameLength) {
        REQUIRE(outcome.value.has_value());
        CHECK(outcome.value->front().size() == length);
    } else {
        CHECK(outcome.code() == ErrorCode::CborDecode);
    }
}

TEST_CASE("a malformed list is a decode failure", "[stat][list][hostile]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    Shape doc{encoding};
    const int variant = GENERATE(0, 1, 2, 3);
    switch (variant) {
    case 0: // stat_list absent
        doc.map(1).text("other").uint(1).end();
        break;
    case 1: // stat_list not an array
        doc.map(1).text("stat_list").text("ble").end();
        break;
    case 2: // an element that is not text
        doc.map(1).text("stat_list").array(2).text("ble").uint(3).end().end();
        break;
    default: // an element that is a nested list
        doc.map(1).text("stat_list").array(1).array(1).text("ble").end().end().end();
        break;
    }

    Outcome<Names> outcome;
    Fixture fixture;
    static_cast<void>(fixture.stats.list_groups(outcome.callback()));
    fixture.respond(doc.view());

    CHECK(outcome.calls == 1);
    CHECK(outcome.code() == ErrorCode::CborDecode);
}

TEST_CASE("unknown keys and a legacy rc of zero beside the list are ignored", "[stat][list]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    // CONFIG_MCUMGR_SMP_LEGACY_RC_BEHAVIOUR writes "rc": 0 first.
    Shape doc{encoding};
    doc.map(3)
        .text("rc")
        .uint(0)
        .text("stat_list")
        .array(1)
        .text("ble")
        .end()
        .text("future")
        .uint(9)
        .end();

    Outcome<Names> outcome;
    Fixture fixture;
    static_cast<void>(fixture.stats.list_groups(outcome.callback()));
    fixture.respond(doc.view());

    REQUIRE(outcome.value.has_value());
    CHECK(*outcome.value == Names{"ble"});
}

// ---------------------------------------------------------------------------
// Read group
// ---------------------------------------------------------------------------

TEST_CASE("a group's fields are returned in the device's order", "[stat][group]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    const std::vector<StatisticsField> fields{{"tx_bytes", 1200}, {"rx_bytes", 34}, {"errors", 0}};

    Outcome<StatisticsGroup> outcome;
    Fixture fixture;
    static_cast<void>(fixture.stats.read_group("net", outcome.callback()));
    fixture.respond(group_response(encoding, "net", fields).view());

    REQUIRE(outcome.value.has_value());
    CHECK(outcome.value->name == "net");
    CHECK(outcome.value->fields == fields);
    REQUIRE(outcome.value->find("rx_bytes") != nullptr);
    CHECK(outcome.value->find("rx_bytes")->value == 34);
    CHECK(outcome.value->find("absent") == nullptr);
}

TEST_CASE("a group with no fields is a success", "[stat][group]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);

    Outcome<StatisticsGroup> outcome;
    Fixture fixture;
    static_cast<void>(fixture.stats.read_group("empty", outcome.callback()));
    fixture.respond(group_response(encoding, "empty", {}).view());

    REQUIRE(outcome.value.has_value());
    CHECK(outcome.value->name == "empty");
    CHECK(outcome.value->fields.empty());
}

TEST_CASE("statistic values keep the whole unsigned range", "[stat][group]")
{
    // A Zephyr server sends 32 bits (A28), but the protocol says uint and
    // another server may send 64.
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    const std::vector<StatisticsField> fields{
        {"u32_max", 0xFFFFFFFFU},
        {"above_u32", 0x100000000U},
        {"u64_max", std::numeric_limits<std::uint64_t>::max()}};

    Outcome<StatisticsGroup> outcome;
    Fixture fixture;
    static_cast<void>(fixture.stats.read_group("big", outcome.callback()));
    fixture.respond(group_response(encoding, "big", fields).view());

    REQUIRE(outcome.value.has_value());
    CHECK(outcome.value->fields == fields);
}

TEST_CASE("exactly kMaxStatisticsFields fields are accepted, one more is not",
          "[stat][group][limits][hostile]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    const std::size_t count =
        GENERATE(smply::limits::kMaxStatisticsFields, smply::limits::kMaxStatisticsFields + 1);
    std::vector<StatisticsField> fields;
    for (std::size_t i = 0; i < count; ++i) {
        fields.push_back(StatisticsField{.name = "f" + std::to_string(i), .value = i});
    }

    Outcome<StatisticsGroup> outcome;
    Fixture fixture;
    static_cast<void>(fixture.stats.read_group("many", outcome.callback()));
    fixture.respond(group_response(encoding, "many", fields).view());

    if (count <= smply::limits::kMaxStatisticsFields) {
        REQUIRE(outcome.value.has_value());
        CHECK(outcome.value->fields.size() == count);
    } else {
        CHECK(outcome.code() == ErrorCode::CborDecode);
    }
}

TEST_CASE("field and group names in a response are bounded", "[stat][group][limits][hostile]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    const std::string at_limit(smply::limits::kMaxStatisticsNameLength, 'n');
    const std::string over(smply::limits::kMaxStatisticsNameLength + 1, 'n');

    SECTION("names at the limit are accepted")
    {
        Outcome<StatisticsGroup> outcome;
        Fixture fixture;
        static_cast<void>(fixture.stats.read_group("g", outcome.callback()));
        fixture.respond(group_response(encoding, at_limit, {{at_limit, 1}}).view());
        REQUIRE(outcome.value.has_value());
        CHECK(outcome.value->name == at_limit);
        CHECK(outcome.value->fields.front().name == at_limit);
    }
    SECTION("an over-long field name is refused")
    {
        Outcome<StatisticsGroup> outcome;
        Fixture fixture;
        static_cast<void>(fixture.stats.read_group("g", outcome.callback()));
        fixture.respond(group_response(encoding, "g", {{over, 1}}).view());
        CHECK(outcome.code() == ErrorCode::CborDecode);
    }
    SECTION("an over-long group name is refused")
    {
        Outcome<StatisticsGroup> outcome;
        Fixture fixture;
        static_cast<void>(fixture.stats.read_group("g", outcome.callback()));
        fixture.respond(group_response(encoding, over, {}).view());
        CHECK(outcome.code() == ErrorCode::CborDecode);
    }
}

TEST_CASE("a repeated field name is refused", "[stat][group][hostile]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);

    Outcome<StatisticsGroup> outcome;
    Fixture fixture;
    static_cast<void>(fixture.stats.read_group("g", outcome.callback()));
    fixture.respond(group_response(encoding, "g", {{"a", 1}, {"b", 2}, {"a", 3}}).view());

    CHECK(outcome.code() == ErrorCode::CborDecode);
}

TEST_CASE("a malformed group response is a decode failure", "[stat][group][hostile]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    Shape doc{encoding};
    const int variant = GENERATE(range(0, 8));
    switch (variant) {
    case 0: // "fields" absent
        doc.map(1).text("name").text("g").end();
        break;
    case 1: // "name" absent
        doc.map(1).text("fields").map(0).end().end();
        break;
    case 2: // "name" not text
        doc.map(2).text("name").uint(1).text("fields").map(0).end().end();
        break;
    case 3: // "fields" not a map
        doc.map(2).text("name").text("g").text("fields").array(0).end().end();
        break;
    case 4: // a negative value
        doc.map(2).text("name").text("g").text("fields").map(1).text("a");
        doc.raw().nint(-5);
        doc.end().end();
        break;
    case 5: // a text value
        doc.map(2).text("name").text("g").text("fields").map(1).text("a").text("5").end().end();
        break;
    case 6: // a field key that is not text
        doc.map(2).text("name").text("g").text("fields").map(1).uint(1).uint(5).end().end();
        break;
    default:
        // The shape a Zephyr server writes when its walk fails after "fields"
        // was opened: the v2 error nested inside the map (A30).
        doc.map(2)
            .text("name")
            .text("g")
            .text("fields")
            .map(2)
            .text("a")
            .uint(1)
            .text("err")
            .map(2)
            .text("group")
            .uint(2)
            .text("rc")
            .uint(5)
            .end()
            .end()
            .end();
        break;
    }

    Outcome<StatisticsGroup> outcome;
    Fixture fixture;
    static_cast<void>(fixture.stats.read_group("g", outcome.callback()));
    fixture.respond(doc.view());

    CHECK(outcome.calls == 1);
    CHECK(outcome.code() == ErrorCode::CborDecode);
}

TEST_CASE("every truncation of a group response is handled", "[stat][group][hostile]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    const Shape doc = group_response(encoding, "net", {{"rx", 1}, {"tx", 2}});
    const ConstBytes whole = doc.view();

    for (std::size_t length = 0; length < whole.size(); ++length) {
        Outcome<StatisticsGroup> outcome;
        Fixture fixture;
        static_cast<void>(fixture.stats.read_group("net", outcome.callback()));
        fixture.respond(whole.first(length));
        INFO("length " << length);
        CHECK(outcome.calls == 1);
        CHECK_FALSE(outcome.value.has_value());
    }
}

TEST_CASE("unknown keys beside the group data are ignored", "[stat][group]")
{
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);
    Shape doc{encoding};
    doc.map(4)
        .text("rc")
        .uint(0)
        .text("fields")
        .map(1)
        .text("a")
        .uint(7)
        .end()
        .text("future")
        .map(1)
        .text("x")
        .uint(1)
        .end()
        .text("name")
        .text("g")
        .end();

    Outcome<StatisticsGroup> outcome;
    Fixture fixture;
    static_cast<void>(fixture.stats.read_group("g", outcome.callback()));
    fixture.respond(doc.view());

    REQUIRE(outcome.value.has_value());
    CHECK(outcome.value->name == "g");
    CHECK(outcome.value->fields == std::vector<StatisticsField>{{"a", 7}});
}

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

TEST_CASE("an unknown group over SMP v2 is InvalidStatName", "[stat][errors]")
{
    // A29: the server reports an unknown group as INVALID_STAT_NAME (3), not
    // INVALID_GROUP (2).
    const Encoding encoding = GENERATE(Encoding::Definite, Encoding::Indefinite);

    Outcome<StatisticsGroup> outcome;
    Fixture fixture{SmpClientConfig{.smp_version = Version::V2}};
    static_cast<void>(fixture.stats.read_group("nope", outcome.callback()));
    fixture.respond(scoped_error(encoding, 2, 3).view());

    REQUIRE(outcome.error.has_value());
    CHECK(outcome.error->code() == ErrorCode::ProtocolError);
    CHECK(outcome.error->mgmt() == MgmtError::scoped(Group::Stat, 3));
    CHECK(smply::statistics_error(*outcome.error) == StatisticsError::InvalidStatName);
    CHECK_FALSE(smply::smp_error(*outcome.error).has_value());
}

TEST_CASE("an unknown group over SMP v1 arrives flat, without the group", "[stat][errors]")
{
    // ORIGINAL_PROTOCOL translates INVALID_STAT_NAME to MGMT_ERR_ENOENT.
    Outcome<StatisticsGroup> outcome;
    Fixture fixture;
    static_cast<void>(fixture.stats.read_group("nope", outcome.callback()));
    fixture.respond(ConstBytes{flat_error(5)});

    REQUIRE(outcome.error.has_value());
    CHECK(outcome.error->code() == ErrorCode::ProtocolError);
    CHECK(smply::smp_error(*outcome.error) == SmpError::NoEntry);
    CHECK_FALSE(smply::statistics_error(*outcome.error).has_value());
}

TEST_CASE("a list failure surfaces the device's code", "[stat][errors]")
{
    Outcome<Names> outcome;
    Fixture fixture;
    static_cast<void>(fixture.stats.list_groups(outcome.callback()));
    fixture.respond(ConstBytes{flat_error(8)}); // ENOTSUP: the group is not built in

    REQUIRE(outcome.error.has_value());
    CHECK(smply::smp_error(*outcome.error) == SmpError::NotSupported);
}

TEST_CASE("statistics_error reads only the statistics group's codes", "[stat][errors]")
{
    using smply::Error;
    CHECK_FALSE(smply::statistics_error(Error{}).has_value());
    CHECK_FALSE(smply::statistics_error(Error{ErrorCode::Timeout}).has_value());
    // A flat rc of 3 is SmpError::InvalidArgument, not InvalidStatName.
    CHECK_FALSE(
        smply::statistics_error(Error{ErrorCode::ProtocolError, MgmtError::smp(3)}).has_value());
    // Another group's code of the same number is not a statistics code.
    CHECK_FALSE(smply::statistics_error(
                    Error{ErrorCode::ProtocolError, MgmtError::scoped(Group::Settings, 3)})
                    .has_value());
    CHECK(smply::statistics_error(
              Error{ErrorCode::ProtocolError, MgmtError::scoped(Group::Stat, 5)}) ==
          StatisticsError::WalkAborted);
    // A code a later release adds is carried through, not rejected (A2).
    const auto future = smply::statistics_error(
        Error{ErrorCode::ProtocolError, MgmtError::scoped(Group::Stat, 42)});
    REQUIRE(future.has_value());
    CHECK(static_cast<std::uint16_t>(*future) == 42);
}

// ---------------------------------------------------------------------------
// Inherited from the client
// ---------------------------------------------------------------------------

TEST_CASE("a statistics request times out like any other", "[stat]")
{
    Outcome<Names> outcome;
    Fixture fixture;
    static_cast<void>(fixture.stats.list_groups(outcome.callback()));

    fixture.clock.advance(std::chrono::seconds{10});
    fixture.client.poll(fixture.clock.now());

    CHECK(outcome.calls == 1);
    CHECK(outcome.code() == ErrorCode::Timeout);
}

TEST_CASE("a cancelled statistics request completes as Cancelled on the next poll", "[stat]")
{
    Outcome<StatisticsGroup> outcome;
    Fixture fixture;
    const RequestHandle handle = fixture.stats.read_group("net", outcome.callback());

    fixture.client.cancel(handle);
    CHECK(outcome.calls == 0);
    fixture.client.poll(fixture.clock.now());
    CHECK(outcome.calls == 1);
    CHECK(outcome.code() == ErrorCode::Cancelled);

    // A late answer to it is discarded, not delivered.
    fixture.respond(group_response(Encoding::Definite, "net", {}).view());
    fixture.client.poll(fixture.clock.now());
    CHECK(outcome.calls == 1);
}

TEST_CASE("a statistics request on a dropped link fails without being sent", "[stat]")
{
    Outcome<Names> outcome;
    Fixture fixture;
    fixture.transport.disconnect();

    CHECK_FALSE(fixture.stats.list_groups(outcome.callback()).valid());
    CHECK(fixture.transport.send_count() == 0);
    CHECK(outcome.calls == 0);
    fixture.client.poll(fixture.clock.now());
    CHECK(outcome.code() == ErrorCode::Disconnected);
}

TEST_CASE("a null statistics callback is accepted and simply not invoked", "[stat]")
{
    SECTION("list")
    {
        Fixture fixture;
        static_cast<void>(fixture.stats.list_groups(nullptr));
        fixture.respond(list_response(Encoding::Definite, {"a"}).view());
    }
    SECTION("read")
    {
        Fixture fixture;
        static_cast<void>(fixture.stats.read_group("a", nullptr));
        fixture.respond(group_response(Encoding::Definite, "a", {}).view());
    }
    SECTION("rejected")
    {
        Fixture fixture;
        static_cast<void>(fixture.stats.read_group("", nullptr));
        fixture.client.poll(fixture.clock.now());
    }
    SUCCEED(); // reaching here without a crash is the assertion
}
