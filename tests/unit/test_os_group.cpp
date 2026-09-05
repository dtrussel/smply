// SPDX-License-Identifier: Apache-2.0
//
// The OS group is thin by design, so most of what is worth testing is the
// encoding: every request vector here is hand-derived from the CBOR grammar
// and the field names in docs/protocol-notes.md section 5, not copied from
// what the writer happened to emit. An encoder that agrees with itself proves
// nothing.

#include "smply/groups/os.hpp"

#include "fake_transport.hpp"
#include "manual_clock.hpp"
#include "message_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

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
using smply::McumgrParameters;
using smply::MgmtError;
using smply::Operation;
using smply::OsManagement;
using smply::RequestHandle;
using smply::ResetOptions;
using smply::Result;
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

/// `Result<void>` has no value to capture, so this one only records that it
/// happened and how.
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
    OsManagement os;

    Fixture() : client{transport, clock}, os{client} {}

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
};

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

TEST_CASE("an unforced reset is the empty map", "[os][reset][encoding]")
{
    VoidOutcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.os.reset(outcome.callback()));

    const Header header = fixture.sent_header();
    REQUIRE(header.op == Operation::Write); // reset is registered write-only
    REQUIRE(header.group == Group::Os);
    REQUIRE(header.command == 5);
    REQUIRE(fixture.sent_payload() == bytes_of({0xA0}));
}

TEST_CASE("a forced reset sends force as a CBOR boolean", "[os][reset][encoding]")
{
    // A15: the specification says (int), the server decodes a boolean and
    // silently ignores anything else. The byte that matters is the 0xF5.
    VoidOutcome outcome;

    Fixture fixture;

    ResetOptions options;
    options.force = true;
    static_cast<void>(fixture.os.reset(options, outcome.callback()));

    REQUIRE(fixture.sent_payload() == bytes_of({0xA1, 0x65, 0x66, 0x6F, 0x72, 0x63, 0x65, 0xF5}));
}

TEST_CASE("a reset response completes the command", "[os][reset]")
{
    VoidOutcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.os.reset(outcome.callback()));
    fixture.respond(ConstBytes{bytes_of({0xA0})});

    REQUIRE(outcome.calls == 1);
    REQUIRE_FALSE(outcome.code.has_value());
}

TEST_CASE("a refused reset surfaces the device's code", "[os][reset]")
{
    // A reset hook may say no. EBUSY is the one that invites a retry with
    // force (protocol-notes section 5).
    VoidOutcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.os.reset(outcome.callback()));
    fixture.respond(ConstBytes{bytes_of({0xA1, 0x62, 0x72, 0x63, 0x0A})}); // {"rc": 10}

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::ProtocolError);
    REQUIRE(outcome.mgmt == MgmtError::smp(10));
}

TEST_CASE("a per-request timeout reaches the client", "[os][reset]")
{
    VoidOutcome outcome;

    Fixture fixture;

    ResetOptions options;
    options.timeout = std::chrono::seconds{30};
    static_cast<void>(fixture.os.reset(options, outcome.callback()));

    const auto deadline = fixture.client.next_deadline();
    REQUIRE(deadline.has_value());
    REQUIRE(*deadline == fixture.clock.now() + std::chrono::seconds{30});
}

// ---------------------------------------------------------------------------
// MCUmgr parameters
// ---------------------------------------------------------------------------

TEST_CASE("the parameters request is a read of an empty map", "[os][params][encoding]")
{
    Outcome<McumgrParameters> outcome;

    Fixture fixture;

    static_cast<void>(fixture.os.mcumgr_parameters(outcome.callback()));

    const Header header = fixture.sent_header();
    REQUIRE(header.op == Operation::Read); // registered read-only
    REQUIRE(header.group == Group::Os);
    REQUIRE(header.command == 6);
    REQUIRE(fixture.sent_payload() == bytes_of({0xA0}));
}

TEST_CASE("buf_size and buf_count are decoded", "[os][params]")
{
    Outcome<McumgrParameters> outcome;

    Fixture fixture;

    static_cast<void>(fixture.os.mcumgr_parameters(outcome.callback()));
    // {"buf_size": 256, "buf_count": 4}
    fixture.respond(ConstBytes{
        bytes_of({0xA2, 0x68, 0x62, 0x75, 0x66, 0x5F, 0x73, 0x69, 0x7A, 0x65, 0x19, 0x01,
                  0x00, 0x69, 0x62, 0x75, 0x66, 0x5F, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x04})});

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.value == McumgrParameters{.buf_size = 256, .buf_count = 4});
}

TEST_CASE("ENOTSUP is an ordinary protocol error the caller can recognise", "[os][params]")
{
    // The command is optional. A minimal server answers rc 8, and the caller
    // must be able to tell that apart from a real failure so it can fall back
    // to a conservative buffer size rather than abandon the update.
    std::optional<smply::Error> captured;

    Fixture fixture;

    static_cast<void>(fixture.os.mcumgr_parameters([&](const Result<McumgrParameters>& result) {
        if (!result.has_value()) {
            captured = result.error();
        }
    }));
    fixture.respond(ConstBytes{bytes_of({0xA1, 0x62, 0x72, 0x63, 0x08})}); // {"rc": 8}

    REQUIRE(captured.has_value());
    REQUIRE(captured->code() == ErrorCode::ProtocolError);
    REQUIRE(smply::smp_error(*captured) == SmpError::NotSupported);
}

TEST_CASE("a group-scoped rc is never read as an SMP error", "[os][params]")
{
    // An OS-group rc of 8 means something else entirely from a flat rc of 8.
    // Mistaking one for the other is exactly what smp_error() prevents.
    std::optional<smply::Error> captured;

    Fixture fixture;

    static_cast<void>(fixture.os.mcumgr_parameters([&](const Result<McumgrParameters>& result) {
        if (!result.has_value()) {
            captured = result.error();
        }
    }));
    // {"err": {"group": 0, "rc": 8}}
    fixture.respond(ConstBytes{bytes_of({0xA1, 0x63, 0x65, 0x72, 0x72, 0xA2, 0x65, 0x67, 0x72, 0x6F,
                                         0x75, 0x70, 0x00, 0x62, 0x72, 0x63, 0x08})});

    REQUIRE(captured.has_value());
    REQUIRE(captured->mgmt() == MgmtError::scoped(Group::Os, 8));
    REQUIRE_FALSE(smply::smp_error(*captured).has_value());
}

TEST_CASE("a parameters response missing a field is rejected", "[os][params][hostile]")
{
    Outcome<McumgrParameters> outcome;

    Fixture fixture;

    static_cast<void>(fixture.os.mcumgr_parameters(outcome.callback()));
    // {"buf_size": 256} -- buf_count absent.
    fixture.respond(ConstBytes{
        bytes_of({0xA1, 0x68, 0x62, 0x75, 0x66, 0x5F, 0x73, 0x69, 0x7A, 0x65, 0x19, 0x01, 0x00})});

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("a wrong-typed parameter is a decode failure, not an absent field",
          "[os][params][hostile]")
{
    // The distinction P5 exists to preserve: a text buf_size must not read as
    // "absent" and then as a default.
    Outcome<McumgrParameters> outcome;

    Fixture fixture;

    static_cast<void>(fixture.os.mcumgr_parameters(outcome.callback()));
    // {"buf_size": "no", "buf_count": 4}
    fixture.respond(ConstBytes{
        bytes_of({0xA2, 0x68, 0x62, 0x75, 0x66, 0x5F, 0x73, 0x69, 0x7A, 0x65, 0x62, 0x6E,
                  0x6F, 0x69, 0x62, 0x75, 0x66, 0x5F, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x04})});

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::CborDecode);
    REQUIRE_FALSE(outcome.value.has_value());
}

TEST_CASE("a parameter too large for 32 bits is rejected", "[os][params][hostile]")
{
    Outcome<McumgrParameters> outcome;

    Fixture fixture;

    static_cast<void>(fixture.os.mcumgr_parameters(outcome.callback()));
    // {"buf_size": 4294967296, "buf_count": 4}
    fixture.respond(
        ConstBytes{bytes_of({0xA2, 0x68, 0x62, 0x75, 0x66, 0x5F, 0x73, 0x69, 0x7A, 0x65,
                             0x1B, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x69,
                             0x62, 0x75, 0x66, 0x5F, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x04})});

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("a parameters response that is not a map is rejected", "[os][params][hostile]")
{
    Outcome<McumgrParameters> outcome;

    Fixture fixture;

    static_cast<void>(fixture.os.mcumgr_parameters(outcome.callback()));
    fixture.respond(ConstBytes{bytes_of({0x63, 0x62, 0x61, 0x64})}); // the text "bad"

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

// ---------------------------------------------------------------------------
// Echo
// ---------------------------------------------------------------------------

TEST_CASE("the echo request carries the string under \"d\"", "[os][echo][encoding]")
{
    Outcome<std::string> outcome;

    Fixture fixture;

    static_cast<void>(fixture.os.echo("hi", outcome.callback()));

    const Header header = fixture.sent_header();
    REQUIRE(header.group == Group::Os);
    REQUIRE(header.command == 0);
    REQUIRE(fixture.sent_payload() == bytes_of({0xA1, 0x61, 0x64, 0x62, 0x68, 0x69}));
}

TEST_CASE("the echoed string is returned", "[os][echo]")
{
    Outcome<std::string> outcome;

    Fixture fixture;

    static_cast<void>(fixture.os.echo("hi", outcome.callback()));
    fixture.respond(ConstBytes{bytes_of({0xA1, 0x61, 0x72, 0x62, 0x68, 0x69})}); // {"r": "hi"}

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.value == std::string{"hi"});
}

TEST_CASE("an empty echo string round-trips", "[os][echo]")
{
    Outcome<std::string> outcome;

    Fixture fixture;

    static_cast<void>(fixture.os.echo("", outcome.callback()));
    REQUIRE(fixture.sent_payload() == bytes_of({0xA1, 0x61, 0x64, 0x60}));

    fixture.respond(ConstBytes{bytes_of({0xA1, 0x61, 0x72, 0x60})}); // {"r": ""}
    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.value == std::string{});
}

TEST_CASE("an over-long echo string is refused before anything is sent", "[os][echo]")
{
    Outcome<std::string> outcome;

    Fixture fixture;

    const std::string too_long(smply::limits::kMaxEchoLength + 1, 'x');
    const RequestHandle handle = fixture.os.echo(too_long, outcome.callback());

    REQUIRE_FALSE(handle.valid());
    REQUIRE(fixture.transport.send_count() == 0);
    // Refused, but not inside the call: the callback is deferred like any other.
    REQUIRE(outcome.calls == 0);

    fixture.client.poll(fixture.clock.now());
    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::InvalidArgument);
}

TEST_CASE("an echo string of exactly the limit is accepted", "[os][echo]")
{
    Outcome<std::string> outcome;

    Fixture fixture;

    const std::string at_limit(smply::limits::kMaxEchoLength, 'x');
    const RequestHandle handle = fixture.os.echo(at_limit, outcome.callback());

    REQUIRE(handle.valid());
    REQUIRE(fixture.transport.send_count() == 1);
}

TEST_CASE("an echo reply longer than the limit is rejected", "[os][echo][hostile]")
{
    // A device answering with more than it was sent is not answering the
    // question, and the copy must be bounded before it is made.
    Outcome<std::string> outcome;

    Fixture fixture;

    static_cast<void>(fixture.os.echo("hi", outcome.callback()));

    // {"r": <129 bytes>}: 0x78 0x81 is a text string of length 129.
    std::vector<std::byte> reply = bytes_of({0xA1, 0x61, 0x72, 0x78, 0x81});
    reply.insert(reply.end(), smply::limits::kMaxEchoLength + 1, std::byte{'x'});
    fixture.respond(ConstBytes{reply});

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("a wrong-typed echo reply is a decode failure, not an absent one", "[os][echo][hostile]")
{
    // {"r": 5}. The reader is poisoned rather than reporting "r" as absent,
    // which is the distinction that stops a malformed reply reading as an
    // empty echo.
    Outcome<std::string> outcome;

    Fixture fixture;

    static_cast<void>(fixture.os.echo("hi", outcome.callback()));
    fixture.respond(ConstBytes{bytes_of({0xA1, 0x61, 0x72, 0x05})});

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::CborDecode);
    REQUIRE_FALSE(outcome.value.has_value());
}

TEST_CASE("an echo reply with no text is rejected", "[os][echo][hostile]")
{
    Outcome<std::string> outcome;

    Fixture fixture;

    static_cast<void>(fixture.os.echo("hi", outcome.callback()));
    fixture.respond(ConstBytes{bytes_of({0xA0})}); // {} -- success, but no "r"

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("every truncation of an echo reply is handled", "[os][echo][hostile]")
{
    // The same prefix sweep P5 applies to the reader, at the group level: no
    // prefix of a valid reply may crash or be mistaken for a good one.
    const auto full = bytes_of({0xA1, 0x61, 0x72, 0x62, 0x68, 0x69});

    for (std::size_t length = 0; length < full.size(); ++length) {
        Outcome<std::string> outcome;

        Fixture fixture;

        static_cast<void>(fixture.os.echo("hi", outcome.callback()));
        fixture.respond(ConstBytes{full}.first(length));

        REQUIRE(outcome.calls == 1);
        REQUIRE(outcome.code == ErrorCode::CborDecode);
        REQUIRE_FALSE(outcome.value.has_value());
    }
}

// ---------------------------------------------------------------------------
// Shared behaviour
// ---------------------------------------------------------------------------

TEST_CASE("a command on a dropped link fails without being sent", "[os]")
{
    VoidOutcome outcome;

    Fixture fixture;

    fixture.transport.disconnect();
    const RequestHandle handle = fixture.os.reset(outcome.callback());

    REQUIRE_FALSE(handle.valid());
    REQUIRE(fixture.transport.send_count() == 0);

    fixture.client.poll(fixture.clock.now());
    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::Disconnected);
}

TEST_CASE("a command times out like any other request", "[os]")
{
    Outcome<std::string> outcome;

    Fixture fixture;

    static_cast<void>(fixture.os.echo("hi", outcome.callback()));
    fixture.clock.advance(std::chrono::seconds{10});
    fixture.client.poll(fixture.clock.now());

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::Timeout);
}

TEST_CASE("a null callback is accepted and simply not invoked", "[os]")
{
    // Every command has its own response lambda, so each needs its own check
    // that a missing callback is a no-op rather than a call through an empty
    // std::function.
    SECTION("echo")
    {
        Fixture fixture;
        static_cast<void>(fixture.os.echo("hi", nullptr));
        REQUIRE(fixture.transport.send_count() == 1);
        fixture.respond(ConstBytes{bytes_of({0xA1, 0x61, 0x72, 0x62, 0x68, 0x69})});
    }
    SECTION("reset")
    {
        Fixture fixture;
        static_cast<void>(fixture.os.reset(nullptr));
        REQUIRE(fixture.transport.send_count() == 1);
        fixture.respond(ConstBytes{bytes_of({0xA0})});
    }
    SECTION("mcumgr parameters")
    {
        Fixture fixture;
        static_cast<void>(fixture.os.mcumgr_parameters(nullptr));
        REQUIRE(fixture.transport.send_count() == 1);
        fixture.respond(ConstBytes{
            bytes_of({0xA2, 0x68, 0x62, 0x75, 0x66, 0x5F, 0x73, 0x69, 0x7A, 0x65, 0x19, 0x01,
                      0x00, 0x69, 0x62, 0x75, 0x66, 0x5F, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x04})});
    }
    SUCCEED(); // reaching here without a crash is the assertion
}

TEST_CASE("a reset that fails before sending still reports on the next poll", "[os][reset]")
{
    VoidOutcome outcome;

    Fixture fixture;

    fixture.transport.disconnect();
    static_cast<void>(fixture.os.reset(outcome.callback()));
    REQUIRE(outcome.calls == 0);

    fixture.client.poll(fixture.clock.now());
    REQUIRE(outcome.code == ErrorCode::Disconnected);
}

TEST_CASE("parameters on a dropped link fails without being sent", "[os][params]")
{
    Outcome<McumgrParameters> outcome;

    Fixture fixture;

    fixture.transport.disconnect();
    const RequestHandle handle = fixture.os.mcumgr_parameters(outcome.callback());

    REQUIRE_FALSE(handle.valid());
    REQUIRE(fixture.transport.send_count() == 0);

    fixture.client.poll(fixture.clock.now());
    REQUIRE(outcome.code == ErrorCode::Disconnected);
}

TEST_CASE("commands issued together get distinct sequence numbers", "[os]")
{
    smply::SmpClientConfig config;
    config.max_in_flight = 2;

    Outcome<std::string> first;
    Outcome<McumgrParameters> second;

    FakeTransport transport;
    const ManualClock clock;
    SmpClient client{transport, clock, config};
    OsManagement os{client};

    static_cast<void>(os.echo("hi", first.callback()));
    const auto first_header = smply::decode_header(transport.last_sent());
    REQUIRE(first_header.has_value());

    static_cast<void>(os.mcumgr_parameters(second.callback()));
    const auto second_header = smply::decode_header(transport.last_sent());
    REQUIRE(second_header.has_value());

    REQUIRE(first_header->seq != second_header->seq);
    REQUIRE(client.in_flight() == 2);
}

} // namespace
