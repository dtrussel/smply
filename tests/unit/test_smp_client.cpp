// SPDX-License-Identifier: Apache-2.0
//
// The correlation rules here are the ones that decide whether a stale or
// hostile message can complete a live request. Getting them wrong does not
// produce a crash -- it produces a firmware update that believes a step
// succeeded when it did not.

#include "smply/smp_client.hpp"

#include "fake_transport.hpp"
#include "manual_clock.hpp"
#include "message_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

using smply::ConstBytes;
using smply::Duration;
using smply::ErrorCode;
using smply::Group;
using smply::Header;
using smply::Operation;
using smply::RawResponse;
using smply::RequestHandle;
using smply::RequestSpec;
using smply::Result;
using smply::SmpClient;
using smply::SmpClientConfig;
using smply::Version;
using smply::test::bytes_of;
using smply::test::FakeTransport;
using smply::test::make_message;
using smply::test::ManualClock;

namespace Catch {
/// Without this an ErrorCode mismatch prints as "{?}", which says nothing about
/// what actually went wrong.
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

/// An empty CBOR map: the minimal successful MCUmgr payload.
std::vector<std::byte> ok_payload()
{
    return bytes_of({0xA0});
}

/// {"rc": 3} -- MGMT_ERR_EINVAL.
std::vector<std::byte> error_payload()
{
    return bytes_of({0xA1, 0x62, 0x72, 0x63, 0x03});
}

/// Collects one request's outcome.
struct Outcome
{
    int calls = 0;
    std::optional<ErrorCode> code;
    std::optional<Header> header;
    std::vector<std::byte> payload;

    [[nodiscard]] auto callback()
    {
        return [this](Result<RawResponse> result) {
            ++calls;
            if (result.has_value()) {
                header = result->header;
                payload.assign(result->payload.begin(), result->payload.end());
            } else {
                code = result.error().code();
            }
        };
    }
};

/// The request every test issues unless it needs something specific.
///
/// A free function rather than a Fixture member: it needs nothing from the
/// fixture, and reaching a static through an instance reads as though it did.
[[nodiscard]] RequestSpec spec(Operation op = Operation::Read, Group group = Group::Image,
                               std::uint8_t command = 0)
{
    return RequestSpec{.op = op, .group = group, .command = command, .payload = {}, .timeout = {}};
}

/// Transport, clock and client, wired together.
///
/// **Declare anything a callback captures -- an `Outcome`, a counter, a handle
/// -- *before* the fixture.** `~SmpClient` completes every request still
/// outstanding, so a callback runs during the fixture's destruction. A capture
/// declared after the fixture is destroyed first, and the callback then writes
/// through a dangling pointer. That is a stack-use-after-scope which only
/// Clang's AddressSanitizer reports; GCC's does not, and an ordinary build is
/// silent. The blank line after the declarations in each test is the seam.
struct Fixture
{
    FakeTransport transport;
    ManualClock clock;
    SmpClient client;

    explicit Fixture(SmpClientConfig config = {}) : client{transport, clock, config} {}

    /// The sequence number the client used for its most recent send.
    [[nodiscard]] std::uint8_t sent_seq() const
    {
        const auto decoded = smply::decode_header(transport.last_sent());
        REQUIRE(decoded.has_value());
        return decoded->seq;
    }

    /// Delivers a well-formed response matching `header`.
    void respond(const Header& header, ConstBytes payload)
    {
        const auto message = make_message(header, payload);
        transport.deliver(ConstBytes{message});
    }

    /// A response that correctly answers the last request sent.
    [[nodiscard]] Header reply_header(Operation op = Operation::ReadResponse,
                                      Group group = Group::Image, std::uint8_t command = 0) const
    {
        return Header{.op = op,
                      .version = Version::V1,
                      .flags = 0,
                      .length = 0,
                      .group = group,
                      .seq = sent_seq(),
                      .command = command};
    }
};

} // namespace

// ---------------------------------------------------------------------------
// The happy path
// ---------------------------------------------------------------------------

TEST_CASE("a request is sent and its response delivered", "[client]")
{
    Outcome outcome;

    Fixture fixture;

    const auto handle = fixture.client.request(spec(), outcome.callback());

    REQUIRE(handle.valid());
    REQUIRE(fixture.transport.send_count() == 1);
    REQUIRE(fixture.client.in_flight() == 1);
    REQUIRE(outcome.calls == 0); // never called from inside request()

    const auto payload = ok_payload();
    fixture.respond(fixture.reply_header(), ConstBytes{payload});

    REQUIRE(outcome.calls == 1);
    REQUIRE_FALSE(outcome.code.has_value());
    REQUIRE(outcome.payload == payload);
    REQUIRE(fixture.client.in_flight() == 0);
}

TEST_CASE("the outgoing message carries the configured version and fields", "[client]")
{
    SmpClientConfig config;
    config.smp_version = Version::V2;
    Outcome outcome;

    Fixture fixture{config};

    const auto body = bytes_of({0xA1, 0x61, 0x64, 0x01});
    RequestSpec request_spec = spec(Operation::Write, Group::Os, 5);
    request_spec.payload = ConstBytes{body};

    static_cast<void>(fixture.client.request(request_spec, outcome.callback()));

    const auto header = smply::decode_header(fixture.transport.last_sent());
    REQUIRE(header.has_value());
    REQUIRE(header->op == Operation::Write);
    REQUIRE(header->version == Version::V2);
    REQUIRE(header->group == Group::Os);
    REQUIRE(header->command == 5);
    REQUIRE(header->length == body.size());
}

TEST_CASE("the payload is copied, not borrowed past the call", "[client]")
{
    // RequestSpec::payload is borrowed for the duration of request() only.
    Outcome outcome;
    std::vector<std::byte> expected;

    Fixture fixture;

    {
        const auto body = bytes_of({0xA1, 0x61, 0x64, 0x2A});
        expected = body;
        RequestSpec request_spec = spec(Operation::Write);
        request_spec.payload = ConstBytes{body};
        static_cast<void>(fixture.client.request(request_spec, outcome.callback()));
    }

    const auto sent = fixture.transport.last_sent();
    const auto tail = std::vector<std::byte>(sent.begin() + smply::kHeaderSize, sent.end());
    REQUIRE(tail == expected);
}

TEST_CASE("a device-reported error becomes a protocol failure", "[client]")
{
    Outcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(), outcome.callback()));

    const auto payload = error_payload();
    fixture.respond(fixture.reply_header(), ConstBytes{payload});

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::ProtocolError);
}

TEST_CASE("successive requests use different sequence numbers", "[client]")
{
    std::vector<std::uint8_t> seqs;

    Fixture fixture;

    for (int i = 0; i < 5; ++i) {
        Outcome outcome;
        static_cast<void>(fixture.client.request(spec(), outcome.callback()));
        seqs.push_back(fixture.sent_seq());
        fixture.respond(fixture.reply_header(), ConstBytes{ok_payload()});
        // Completed within the iteration, which is what makes a loop-local
        // Outcome safe: nothing pending outlives it.
        REQUIRE(outcome.calls == 1);
    }

    for (std::size_t i = 1; i < seqs.size(); ++i) {
        REQUIRE(seqs[i] != seqs[i - 1]);
    }
}

// ---------------------------------------------------------------------------
// Correlation. A response must match on all four of sequence, group, command
// and operation.
// ---------------------------------------------------------------------------

TEST_CASE("a response with the wrong sequence is dropped", "[client][correlation]")
{
    Outcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(), outcome.callback()));

    Header wrong = fixture.reply_header();
    wrong.seq = static_cast<std::uint8_t>(wrong.seq + 1);
    fixture.respond(wrong, ConstBytes{ok_payload()});

    REQUIRE(outcome.calls == 0);
    REQUIRE(fixture.client.in_flight() == 1); // still waiting
    REQUIRE(fixture.client.stats().unmatched == 1);
}

TEST_CASE("a response with the wrong group is dropped and the request left pending",
          "[client][correlation]")
{
    // The sequence matches but the group does not. Completing the request would
    // let an unrelated exchange answer it; the specification does not define
    // this case, so discarding is the only choice that cannot mis-complete.
    Outcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(), outcome.callback()));
    fixture.respond(fixture.reply_header(Operation::ReadResponse, Group::Os, 0),
                    ConstBytes{ok_payload()});

    REQUIRE(outcome.calls == 0);
    REQUIRE(fixture.client.in_flight() == 1);
    REQUIRE(fixture.client.stats().mismatched == 1);
}

TEST_CASE("a response with the wrong command is dropped", "[client][correlation]")
{
    Outcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(), outcome.callback()));
    fixture.respond(fixture.reply_header(Operation::ReadResponse, Group::Image, 9),
                    ConstBytes{ok_payload()});

    REQUIRE(outcome.calls == 0);
    REQUIRE(fixture.client.stats().mismatched == 1);
}

TEST_CASE("a response with the wrong operation is dropped", "[client][correlation]")
{
    // A Read must be answered by a ReadResponse. A WriteResponse carrying the
    // right sequence is not this request's answer.
    Outcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(Operation::Read), outcome.callback()));
    fixture.respond(fixture.reply_header(Operation::WriteResponse), ConstBytes{ok_payload()});

    REQUIRE(outcome.calls == 0);
    REQUIRE(fixture.client.stats().mismatched == 1);
}

TEST_CASE("a write is answered by a write-response", "[client][correlation]")
{
    Outcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(Operation::Write), outcome.callback()));
    fixture.respond(fixture.reply_header(Operation::WriteResponse), ConstBytes{ok_payload()});

    REQUIRE(outcome.calls == 1);
    REQUIRE_FALSE(outcome.code.has_value());
}

TEST_CASE("a mismatched response still lets the request time out normally", "[client][correlation]")
{
    Outcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(), outcome.callback()));
    fixture.respond(fixture.reply_header(Operation::ReadResponse, Group::Os, 0),
                    ConstBytes{ok_payload()});

    fixture.clock.advance(std::chrono::seconds{10});
    fixture.client.poll(fixture.clock.now());

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::Timeout);
}

TEST_CASE("a duplicate response is delivered once", "[client][correlation]")
{
    Outcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(), outcome.callback()));

    const auto header = fixture.reply_header();
    const auto payload = ok_payload();
    fixture.respond(header, ConstBytes{payload});
    fixture.respond(header, ConstBytes{payload});

    REQUIRE(outcome.calls == 1);
    // The second is absorbed by the retired set, not counted as unsolicited.
    REQUIRE(fixture.client.stats().late == 1);
}

TEST_CASE("an unsolicited response is counted and dropped", "[client]")
{
    Fixture fixture;

    const Header header{.op = Operation::ReadResponse,
                        .version = Version::V1,
                        .flags = 0,
                        .length = 0,
                        .group = Group::Image,
                        .seq = 77,
                        .command = 0};
    fixture.respond(header, ConstBytes{ok_payload()});

    REQUIRE(fixture.client.stats().unmatched == 1);
    REQUIRE(fixture.client.stats().received == 1);
}

// ---------------------------------------------------------------------------
// The retired set. This is what stops a late answer being mis-attributed once
// the 8-bit sequence number wraps.
// ---------------------------------------------------------------------------

TEST_CASE("a late response cannot complete a request that reused its sequence",
          "[client][correlation][security]")
{
    // The scenario the retired set exists for: request A times out, a later
    // request B is issued, and A's answer finally arrives. Without the retired
    // set, B's sequence allocation could collide with A's and B would be
    // completed by A's data.
    SmpClientConfig config;
    config.max_retired_seqs = 64;
    Outcome first;
    Outcome second;

    Fixture fixture{config};

    static_cast<void>(fixture.client.request(spec(), first.callback()));
    const std::uint8_t first_seq = fixture.sent_seq();

    fixture.clock.advance(std::chrono::seconds{10});
    fixture.client.poll(fixture.clock.now());
    REQUIRE(first.code == ErrorCode::Timeout);

    static_cast<void>(fixture.client.request(spec(), second.callback()));
    const std::uint8_t second_seq = fixture.sent_seq();

    // The allocator must not have handed out the retired number again.
    REQUIRE(second_seq != first_seq);

    // Now the first request's answer arrives, far too late.
    Header stale = fixture.reply_header();
    stale.seq = first_seq;
    fixture.respond(stale, ConstBytes{ok_payload()});

    REQUIRE(second.calls == 0); // not mis-attributed
    REQUIRE(fixture.client.stats().late == 1);

    // The live request still gets its own answer.
    Header correct = fixture.reply_header();
    correct.seq = second_seq;
    fixture.respond(correct, ConstBytes{ok_payload()});
    REQUIRE(second.calls == 1);
}

TEST_CASE("the retired set is bounded", "[client][correlation]")
{
    // With a small ring, the oldest entries are forgotten. That is the intended
    // trade: bounded memory, at the cost of a very old response being counted
    // as unsolicited rather than late.
    SmpClientConfig config;
    config.max_retired_seqs = 4;
    std::vector<std::uint8_t> seqs;

    Fixture fixture{config};

    for (int i = 0; i < 10; ++i) {
        Outcome outcome;
        static_cast<void>(fixture.client.request(spec(), outcome.callback()));
        seqs.push_back(fixture.sent_seq());
        fixture.respond(fixture.reply_header(), ConstBytes{ok_payload()});
        REQUIRE(outcome.calls == 1); // nothing pending outlives the iteration
    }

    // The very first sequence is long forgotten.
    Header ancient = fixture.reply_header();
    ancient.seq = seqs.front();
    fixture.respond(ancient, ConstBytes{ok_payload()});

    REQUIRE(fixture.client.stats().unmatched == 1);
}

// ---------------------------------------------------------------------------
// Timeouts
// ---------------------------------------------------------------------------

TEST_CASE("a request times out exactly at its deadline", "[client][timeout]")
{
    SmpClientConfig config;
    config.default_timeout = std::chrono::seconds{5};
    Outcome outcome;

    Fixture fixture{config};

    static_cast<void>(fixture.client.request(spec(), outcome.callback()));

    // One millisecond early: still pending.
    fixture.clock.advance(std::chrono::milliseconds{4999});
    fixture.client.poll(fixture.clock.now());
    REQUIRE(outcome.calls == 0);

    fixture.clock.advance(std::chrono::milliseconds{1});
    fixture.client.poll(fixture.clock.now());
    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::Timeout);
    REQUIRE(fixture.client.stats().timeouts == 1);
}

TEST_CASE("a per-request timeout overrides the default", "[client][timeout]")
{
    // Image erase and the first upload chunk need far longer than the default.
    SmpClientConfig config;
    config.default_timeout = std::chrono::seconds{5};
    Outcome outcome;

    Fixture fixture{config};

    RequestSpec request_spec = spec();
    request_spec.timeout = std::chrono::seconds{60};
    static_cast<void>(fixture.client.request(request_spec, outcome.callback()));

    fixture.clock.advance(std::chrono::seconds{30});
    fixture.client.poll(fixture.clock.now());
    REQUIRE(outcome.calls == 0);

    fixture.clock.advance(std::chrono::seconds{30});
    fixture.client.poll(fixture.clock.now());
    REQUIRE(outcome.code == ErrorCode::Timeout);
}

TEST_CASE("next_deadline reports when there is work to do", "[client][timeout]")
{
    SmpClientConfig config;
    config.default_timeout = std::chrono::seconds{5};
    Outcome outcome;

    Fixture fixture{config};

    REQUIRE_FALSE(fixture.client.next_deadline().has_value());

    const auto start = fixture.clock.now();
    static_cast<void>(fixture.client.request(spec(), outcome.callback()));

    const auto deadline = fixture.client.next_deadline();
    REQUIRE(deadline.has_value());
    REQUIRE(*deadline == start + std::chrono::seconds{5});

    fixture.respond(fixture.reply_header(), ConstBytes{ok_payload()});
    REQUIRE_FALSE(fixture.client.next_deadline().has_value());
}

TEST_CASE("next_deadline signals immediate work as a past time", "[client][timeout]")
{
    // A deferred completion is ready now, so an event loop must poll rather
    // than sleep.
    Outcome outcome;

    Fixture fixture;

    fixture.transport.disconnect();
    static_cast<void>(fixture.client.request(spec(), outcome.callback()));

    const auto deadline = fixture.client.next_deadline();
    REQUIRE(deadline.has_value());
    REQUIRE(*deadline == smply::TimePoint::min());
}

TEST_CASE("polling with nothing outstanding is harmless", "[client][timeout]")
{
    Fixture fixture;
    fixture.clock.advance(std::chrono::hours{1});
    fixture.client.poll(fixture.clock.now());
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Cancellation
// ---------------------------------------------------------------------------

TEST_CASE("cancelling completes the request once, on the next poll", "[client][cancel]")
{
    Outcome outcome;

    Fixture fixture;

    const auto handle = fixture.client.request(spec(), outcome.callback());
    fixture.client.cancel(handle);

    // Removed from the table at once, so nothing can complete it in the
    // meantime -- but the callback waits for poll().
    REQUIRE(fixture.client.in_flight() == 0);
    REQUIRE(outcome.calls == 0);

    fixture.client.poll(fixture.clock.now());
    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::Cancelled);
}

TEST_CASE("a response arriving after cancellation is discarded", "[client][cancel]")
{
    Outcome outcome;

    Fixture fixture;

    const auto handle = fixture.client.request(spec(), outcome.callback());
    const auto header = fixture.reply_header();
    fixture.client.cancel(handle);

    fixture.respond(header, ConstBytes{ok_payload()});
    fixture.client.poll(fixture.clock.now());

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::Cancelled); // not the response
    REQUIRE(fixture.client.stats().late == 1);
}

TEST_CASE("cancelling twice completes once", "[client][cancel]")
{
    Outcome outcome;

    Fixture fixture;

    const auto handle = fixture.client.request(spec(), outcome.callback());
    fixture.client.cancel(handle);
    fixture.client.cancel(handle);
    fixture.client.poll(fixture.clock.now());

    REQUIRE(outcome.calls == 1);
}

TEST_CASE("a stale handle cannot cancel a newer request", "[client][cancel]")
{
    // Handles are generation-tagged precisely so a handle to a completed
    // request cannot reach across and cancel whatever now occupies its slot.
    Outcome first;
    Outcome second;

    Fixture fixture;

    const auto stale = fixture.client.request(spec(), first.callback());
    fixture.respond(fixture.reply_header(), ConstBytes{ok_payload()});
    REQUIRE(first.calls == 1);

    static_cast<void>(fixture.client.request(spec(), second.callback()));

    fixture.client.cancel(stale); // inert
    fixture.client.poll(fixture.clock.now());

    REQUIRE(second.calls == 0);
    REQUIRE(fixture.client.in_flight() == 1);
}

TEST_CASE("cancelling a default-constructed handle is a no-op", "[client][cancel]")
{
    Fixture fixture;
    fixture.client.cancel(RequestHandle{});
    fixture.client.poll(fixture.clock.now());
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Failures that prevent the request being issued at all
// ---------------------------------------------------------------------------

TEST_CASE("a request on a dropped link fails without being sent", "[client]")
{
    Outcome outcome;

    Fixture fixture;

    fixture.transport.disconnect();

    const auto handle = fixture.client.request(spec(), outcome.callback());

    REQUIRE_FALSE(handle.valid());
    REQUIRE(outcome.calls == 0); // deferred, not synchronous
    REQUIRE(fixture.transport.send_count() == 0);

    fixture.client.poll(fixture.clock.now());
    REQUIRE(outcome.code == ErrorCode::Disconnected);
}

TEST_CASE("TransportBusy is surfaced to the caller", "[client]")
{
    // The core does not queue on the transport's behalf: whoever knows what the
    // message was for decides whether to retry.
    Outcome outcome;

    Fixture fixture;

    fixture.transport.set_busy(true);
    const auto handle = fixture.client.request(spec(), outcome.callback());

    REQUIRE_FALSE(handle.valid());
    fixture.client.poll(fixture.clock.now());
    REQUIRE(outcome.code == ErrorCode::TransportBusy);
    REQUIRE(fixture.client.in_flight() == 0);
}

TEST_CASE("a send failure is reported and leaves nothing pending", "[client]")
{
    Outcome outcome;

    Fixture fixture;

    fixture.transport.fail_next_send(smply::Error{ErrorCode::TransportError, "injected"});
    static_cast<void>(fixture.client.request(spec(), outcome.callback()));

    fixture.client.poll(fixture.clock.now());
    REQUIRE(outcome.code == ErrorCode::TransportError);
    REQUIRE(fixture.client.in_flight() == 0);
}

TEST_CASE("an oversized payload is refused", "[client]")
{
    SmpClientConfig config;
    config.max_smp_payload = 32;
    Outcome outcome;

    Fixture fixture{config};

    const auto body = smply::test::filler(33);
    RequestSpec request_spec = spec(Operation::Write);
    request_spec.payload = ConstBytes{body};

    static_cast<void>(fixture.client.request(request_spec, outcome.callback()));
    fixture.client.poll(fixture.clock.now());

    REQUIRE(outcome.code == ErrorCode::MessageTooLarge);
    REQUIRE(fixture.transport.send_count() == 0);
}

TEST_CASE("exceeding max_in_flight is refused rather than queued", "[client]")
{
    SmpClientConfig config;
    config.max_in_flight = 1;
    Outcome first;
    Outcome second;

    Fixture fixture{config};

    const auto handle = fixture.client.request(spec(), first.callback());
    REQUIRE(handle.valid());

    const auto rejected = fixture.client.request(spec(), second.callback());
    REQUIRE_FALSE(rejected.valid());

    fixture.client.poll(fixture.clock.now());
    REQUIRE(second.code == ErrorCode::InvalidState);
    REQUIRE(first.calls == 0);
}

TEST_CASE("more than one request may be in flight when configured", "[client]")
{
    SmpClientConfig config;
    config.max_in_flight = 4;
    Outcome first;
    Outcome second;

    Fixture fixture{config};

    static_cast<void>(
        fixture.client.request(spec(Operation::Read, Group::Image, 0), first.callback()));
    const std::uint8_t first_seq = fixture.sent_seq();

    static_cast<void>(
        fixture.client.request(spec(Operation::Read, Group::Os, 1), second.callback()));
    const std::uint8_t second_seq = fixture.sent_seq();

    REQUIRE(fixture.client.in_flight() == 2);
    REQUIRE(first_seq != second_seq);

    // Answered out of order, which is legal.
    Header reply_second = fixture.reply_header(Operation::ReadResponse, Group::Os, 1);
    reply_second.seq = second_seq;
    fixture.respond(reply_second, ConstBytes{ok_payload()});
    REQUIRE(second.calls == 1);
    REQUIRE(first.calls == 0);

    Header reply_first = fixture.reply_header(Operation::ReadResponse, Group::Image, 0);
    reply_first.seq = first_seq;
    fixture.respond(reply_first, ConstBytes{ok_payload()});
    REQUIRE(first.calls == 1);
}

// ---------------------------------------------------------------------------
// Link loss
// ---------------------------------------------------------------------------

TEST_CASE("a disconnect fails every pending request", "[client][disconnect]")
{
    SmpClientConfig config;
    config.max_in_flight = 3;
    Outcome first;
    Outcome second;

    Fixture fixture{config};

    static_cast<void>(fixture.client.request(spec(), first.callback()));
    static_cast<void>(fixture.client.request(spec(), second.callback()));

    fixture.transport.disconnect();

    // Inline, not deferred: this is a transport callback, not something the
    // application asked for, and it should learn at once.
    REQUIRE(first.code == ErrorCode::Disconnected);
    REQUIRE(second.code == ErrorCode::Disconnected);
    REQUIRE(fixture.client.in_flight() == 0);
    REQUIRE_FALSE(fixture.client.connected());
}

TEST_CASE("a recoverable transport error leaves requests pending", "[client][disconnect]")
{
    // The link is still up, so the request either gets its answer or times out.
    Outcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(), outcome.callback()));
    fixture.transport.raise_transport_error(smply::Error{ErrorCode::TransportError, "glitch"});

    REQUIRE(outcome.calls == 0);
    REQUIRE(fixture.client.in_flight() == 1);
    REQUIRE(fixture.client.connected());
}

TEST_CASE("rebinding restores service after a reconnect", "[client][disconnect]")
{
    // Declared before the fixture, so it outlives the client that will be
    // bound to it: ~SmpClient detaches from its transport.
    FakeTransport reconnected;

    Outcome first;
    Outcome second;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(), first.callback()));
    fixture.transport.disconnect();
    REQUIRE(first.code == ErrorCode::Disconnected);

    fixture.client.rebind_transport(reconnected);

    REQUIRE(fixture.client.connected());

    const auto handle = fixture.client.request(spec(), second.callback());
    REQUIRE(handle.valid());
    REQUIRE(reconnected.send_count() == 1);

    const auto decoded = smply::decode_header(reconnected.last_sent());
    REQUIRE(decoded.has_value());
    const Header reply{.op = Operation::ReadResponse,
                       .version = Version::V1,
                       .flags = 0,
                       .length = 0,
                       .group = Group::Image,
                       .seq = decoded->seq,
                       .command = 0};
    const auto message = make_message(reply, ConstBytes{ok_payload()});
    reconnected.deliver(ConstBytes{message});

    REQUIRE(second.calls == 1);
}

TEST_CASE("a partial message does not survive a rebind", "[client][disconnect]")
{
    // Half a response arrives, the link drops, and a new session begins. The
    // stale bytes must not be parsed against the new session's.
    // Declared before the fixture, so it outlives the client that will be
    // bound to it: ~SmpClient detaches from its transport.
    FakeTransport reconnected;

    Outcome outcome;
    Outcome second;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(), outcome.callback()));

    const auto message = make_message(fixture.reply_header(), ConstBytes{ok_payload()});
    fixture.transport.deliver(ConstBytes{message}.first(4)); // half a header
    fixture.transport.disconnect();

    fixture.client.rebind_transport(reconnected);

    static_cast<void>(fixture.client.request(spec(), second.callback()));

    const auto decoded = smply::decode_header(reconnected.last_sent());
    REQUIRE(decoded.has_value());
    const Header reply{.op = Operation::ReadResponse,
                       .version = Version::V1,
                       .flags = 0,
                       .length = 0,
                       .group = Group::Image,
                       .seq = decoded->seq,
                       .command = 0};
    const auto fresh = make_message(reply, ConstBytes{ok_payload()});
    reconnected.deliver(ConstBytes{fresh});

    // Would have failed had the four stale bytes still been buffered.
    REQUIRE(second.calls == 1);
    REQUIRE_FALSE(second.code.has_value());
}

// ---------------------------------------------------------------------------
// Malformed framing
// ---------------------------------------------------------------------------

TEST_CASE("malformed framing fails pending requests", "[client]")
{
    // SMP has no sync word, so once framing is violated nothing further can be
    // correlated. The link itself is the application's to drop.
    Outcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(), outcome.callback()));

    const auto garbage = bytes_of({0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    fixture.transport.deliver(ConstBytes{garbage});

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::MalformedMessage);
    REQUIRE(fixture.client.stats().malformed == 1);
}

TEST_CASE("a response with an undecodable payload fails its request", "[client]")
{
    Outcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(), outcome.callback()));

    // Well-framed, but the payload is not a CBOR map.
    const auto payload = bytes_of({0x01});
    fixture.respond(fixture.reply_header(), ConstBytes{payload});

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("responses split across arbitrary fragments still correlate", "[client]")
{
    Outcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(), outcome.callback()));

    const auto message = make_message(fixture.reply_header(), ConstBytes{ok_payload()});
    fixture.transport.deliver_byte_by_byte(ConstBytes{message});

    REQUIRE(outcome.calls == 1);
    REQUIRE_FALSE(outcome.code.has_value());
}

TEST_CASE("two responses in one delivery are both handled", "[client]")
{
    SmpClientConfig config;
    config.max_in_flight = 2;
    Outcome first;
    Outcome second;

    Fixture fixture{config};

    static_cast<void>(
        fixture.client.request(spec(Operation::Read, Group::Image, 0), first.callback()));
    const std::uint8_t first_seq = fixture.sent_seq();
    static_cast<void>(
        fixture.client.request(spec(Operation::Read, Group::Os, 1), second.callback()));
    const std::uint8_t second_seq = fixture.sent_seq();

    const Header reply_a{.op = Operation::ReadResponse,
                         .version = Version::V1,
                         .flags = 0,
                         .length = 0,
                         .group = Group::Image,
                         .seq = first_seq,
                         .command = 0};
    const Header reply_b{.op = Operation::ReadResponse,
                         .version = Version::V1,
                         .flags = 0,
                         .length = 0,
                         .group = Group::Os,
                         .seq = second_seq,
                         .command = 1};

    auto stream = make_message(reply_a, ConstBytes{ok_payload()});
    const auto tail = make_message(reply_b, ConstBytes{ok_payload()});
    stream.insert(stream.end(), tail.begin(), tail.end());

    fixture.transport.deliver(ConstBytes{stream});

    REQUIRE(first.calls == 1);
    REQUIRE(second.calls == 1);
}

// ---------------------------------------------------------------------------
// Re-entrancy and lifetime
// ---------------------------------------------------------------------------

TEST_CASE("a callback may issue another request", "[client][reentrancy]")
{
    // The DFU state machine does exactly this on every step.
    int first_calls = 0;
    Outcome second;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(), [&](const Result<RawResponse>&) {
        ++first_calls;
        static_cast<void>(fixture.client.request(spec(), second.callback()));
    }));

    fixture.respond(fixture.reply_header(), ConstBytes{ok_payload()});

    REQUIRE(first_calls == 1);
    REQUIRE(fixture.client.in_flight() == 1);
    REQUIRE(fixture.transport.send_count() == 2);

    fixture.respond(fixture.reply_header(), ConstBytes{ok_payload()});
    REQUIRE(second.calls == 1);
}

TEST_CASE("a callback may cancel another request", "[client][reentrancy]")
{
    SmpClientConfig config;
    config.max_in_flight = 2;
    Outcome other;
    int calls = 0;

    Fixture fixture{config};

    const auto other_handle =
        fixture.client.request(spec(Operation::Read, Group::Os, 1), other.callback());

    static_cast<void>(fixture.client.request(spec(Operation::Read, Group::Image, 0),
                                             [&](const Result<RawResponse>&) {
                                                 ++calls;
                                                 fixture.client.cancel(other_handle);
                                             }));
    const std::uint8_t seq = fixture.sent_seq();

    const Header reply{.op = Operation::ReadResponse,
                       .version = Version::V1,
                       .flags = 0,
                       .length = 0,
                       .group = Group::Image,
                       .seq = seq,
                       .command = 0};
    fixture.respond(reply, ConstBytes{ok_payload()});

    REQUIRE(calls == 1);
    fixture.client.poll(fixture.clock.now());
    REQUIRE(other.code == ErrorCode::Cancelled);
}

TEST_CASE("a timeout callback may issue a request without looping", "[client][reentrancy]")
{
    // A new request's deadline is in the future, so it must not be swept by the
    // same poll() that timed out its predecessor.
    int calls = 0;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(), [&](const Result<RawResponse>&) {
        ++calls;
        if (calls < 3) {
            static_cast<void>(
                fixture.client.request(spec(), [&](const Result<RawResponse>&) { ++calls; }));
        }
    }));

    fixture.clock.advance(std::chrono::seconds{10});
    fixture.client.poll(fixture.clock.now());

    REQUIRE(calls == 1);
    REQUIRE(fixture.client.in_flight() == 1);
}

TEST_CASE("destruction completes outstanding requests", "[client][lifetime]")
{
    // The one place a callback runs outside poll() or a transport callback:
    // there is no later poll() to defer to.
    FakeTransport transport;
    const ManualClock clock;
    Outcome outcome;

    {
        SmpClient client{transport, clock};
        static_cast<void>(client.request(RequestSpec{.op = Operation::Read,
                                                     .group = Group::Image,
                                                     .command = 0,
                                                     .payload = {},
                                                     .timeout = {}},
                                         outcome.callback()));
        REQUIRE(outcome.calls == 0);
    }

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::Cancelled);
}

TEST_CASE("destruction delivers deferred completions too", "[client][lifetime]")
{
    FakeTransport transport;
    const ManualClock clock;
    Outcome outcome;

    {
        SmpClient client{transport, clock};
        transport.disconnect();
        static_cast<void>(client.request(RequestSpec{.op = Operation::Read,
                                                     .group = Group::Image,
                                                     .command = 0,
                                                     .payload = {},
                                                     .timeout = {}},
                                         outcome.callback()));
        REQUIRE(outcome.calls == 0);
    }

    // Nothing is silently dropped on the way out.
    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::Disconnected);
}

TEST_CASE("the client detaches itself from the transport on destruction", "[client][lifetime]")
{
    FakeTransport transport;
    const ManualClock clock;

    {
        const SmpClient client{transport, clock};
        static_cast<void>(client);
    }

    // Delivering now must not reach a destroyed listener.
    const auto message = bytes_of({0x01, 0x02, 0x03});
    transport.deliver(ConstBytes{message});
    REQUIRE(transport.suppressed_deliveries() == 1);
}

TEST_CASE("statistics count what happened", "[client]")
{
    Outcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(), outcome.callback()));
    fixture.respond(fixture.reply_header(), ConstBytes{ok_payload()});

    REQUIRE(fixture.client.stats().sent == 1);
    REQUIRE(fixture.client.stats().received == 1);
    REQUIRE(fixture.client.stats().unmatched == 0);
    REQUIRE(fixture.client.stats().mismatched == 0);
    REQUIRE(fixture.client.stats().timeouts == 0);
}

// ---------------------------------------------------------------------------
// Paths the main suite leaves untouched. These were found by measuring branch
// coverage rather than by reading the code, which is the point of measuring it.
// ---------------------------------------------------------------------------

TEST_CASE("a null callback is accepted and simply not invoked", "[client]")
{
    // Fire-and-forget is legitimate: an OS reset whose answer nobody waits for.
    Fixture fixture;

    const auto handle = fixture.client.request(spec(), nullptr);
    REQUIRE(handle.valid());

    fixture.respond(fixture.reply_header(), ConstBytes{ok_payload()});
    REQUIRE(fixture.client.in_flight() == 0);

    // The same on the failure paths, which must not dereference it either.
    static_cast<void>(fixture.client.request(spec(), nullptr));
    fixture.clock.advance(std::chrono::seconds{10});
    fixture.client.poll(fixture.clock.now());

    fixture.transport.disconnect();
    static_cast<void>(fixture.client.request(spec(), nullptr));
    fixture.client.poll(fixture.clock.now());
    SUCCEED();
}

TEST_CASE("exhausting the sequence space is reported rather than colliding",
          "[client][correlation]")
{
    // 255 numbers retired plus one in flight leaves nothing to allocate. The
    // allocator must say so rather than reissue a number it promised not to.
    SmpClientConfig config;
    config.max_in_flight = 2;
    config.max_retired_seqs = 255;
    Outcome held;
    Outcome refused;

    Fixture fixture{config};

    for (int i = 0; i < 255; ++i) {
        Outcome outcome;
        static_cast<void>(fixture.client.request(spec(), outcome.callback()));
        fixture.respond(fixture.reply_header(), ConstBytes{ok_payload()});
        REQUIRE(outcome.calls == 1);
    }

    const auto handle = fixture.client.request(spec(), held.callback());
    REQUIRE(handle.valid()); // the one remaining number

    const auto rejected = fixture.client.request(spec(), refused.callback());
    REQUIRE_FALSE(rejected.valid());

    fixture.client.poll(fixture.clock.now());
    REQUIRE(refused.code == ErrorCode::InvalidState);
}

TEST_CASE("retirement can be switched off", "[client][correlation]")
{
    // With no memory of completed requests, a duplicate response is
    // indistinguishable from an unsolicited one. That is the documented cost.
    SmpClientConfig config;
    config.max_retired_seqs = 0;
    Outcome outcome;

    Fixture fixture{config};

    static_cast<void>(fixture.client.request(spec(), outcome.callback()));
    const auto header = fixture.reply_header();
    fixture.respond(header, ConstBytes{ok_payload()});
    fixture.respond(header, ConstBytes{ok_payload()});

    REQUIRE(outcome.calls == 1);
    REQUIRE(fixture.client.stats().late == 0);
    REQUIRE(fixture.client.stats().unmatched == 1);
}

TEST_CASE("next_deadline reports the earliest of several", "[client][timeout]")
{
    SmpClientConfig config;
    config.max_in_flight = 3;
    config.default_timeout = std::chrono::seconds{30};
    Outcome slow;
    Outcome quick;

    Fixture fixture{config};

    RequestSpec long_spec = spec(Operation::Read, Group::Image, 0);
    long_spec.timeout = std::chrono::seconds{60};
    static_cast<void>(fixture.client.request(long_spec, slow.callback()));

    const auto start = fixture.clock.now();
    RequestSpec short_spec = spec(Operation::Read, Group::Os, 1);
    short_spec.timeout = std::chrono::seconds{5};
    static_cast<void>(fixture.client.request(short_spec, quick.callback()));

    const auto deadline = fixture.client.next_deadline();
    REQUIRE(deadline.has_value());
    REQUIRE(*deadline == start + std::chrono::seconds{5});
}

TEST_CASE("a timeout callback may cancel another expiring request", "[client][reentrancy]")
{
    // Both are due in the same poll(). The first callback removes the second,
    // which the sweep must notice rather than completing a freed entry.
    SmpClientConfig config;
    config.max_in_flight = 2;
    RequestHandle victim;
    int first_calls = 0;
    Outcome second;

    Fixture fixture{config};

    // Registration order matters: poll() sweeps expired requests in slot order,
    // so the canceller has to be registered first to run first.
    static_cast<void>(fixture.client.request(spec(Operation::Read, Group::Image, 0),
                                             [&](const Result<RawResponse>&) {
                                                 ++first_calls;
                                                 fixture.client.cancel(victim);
                                             }));

    victim = fixture.client.request(spec(Operation::Read, Group::Os, 1), second.callback());

    fixture.clock.advance(std::chrono::seconds{10});
    fixture.client.poll(fixture.clock.now());

    REQUIRE(first_calls == 1);
    REQUIRE(second.calls == 1);
    REQUIRE(second.code == ErrorCode::Cancelled); // cancelled, not timed out
}

TEST_CASE("rebinding while requests are still pending fails them", "[client][disconnect]")
{
    // The application may rebind without a disconnect ever being reported --
    // it noticed the link was gone by other means. Nothing may be left dangling.
    // Declared before the fixture, so it outlives the client that will be
    // bound to it: ~SmpClient detaches from its transport.
    FakeTransport replacement;

    Outcome outcome;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(), outcome.callback()));
    REQUIRE(fixture.client.in_flight() == 1);

    fixture.client.rebind_transport(replacement);

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::Disconnected);
    REQUIRE(fixture.client.in_flight() == 0);
}

TEST_CASE("a user-defined group's payload is passed through undecoded", "[client]")
{
    // Groups at 64 and above may define their own encoding, so there is no
    // CBOR error map to look for and none must be demanded.
    Outcome outcome;

    Fixture fixture;

    const auto vendor = static_cast<Group>(100);
    static_cast<void>(fixture.client.request(spec(Operation::Read, vendor, 7), outcome.callback()));

    // Deliberately not CBOR at all.
    const auto payload = bytes_of({0xFF, 0x00, 0xFF});
    const Header reply = fixture.reply_header(Operation::ReadResponse, vendor, 7);
    fixture.respond(reply, ConstBytes{payload});

    REQUIRE(outcome.calls == 1);
    REQUIRE_FALSE(outcome.code.has_value());
    REQUIRE(outcome.payload == payload);
}

TEST_CASE("a device reason is attached to the protocol error", "[client]")
{
    std::optional<smply::Error> captured;

    Fixture fixture;

    static_cast<void>(fixture.client.request(spec(), [&](const Result<RawResponse>& result) {
        if (!result.has_value()) {
            captured = result.error();
        }
    }));

    // {"rc": 6, "rsn": "busy"}
    const auto payload = bytes_of(
        {0xA2, 0x62, 0x72, 0x63, 0x06, 0x63, 0x72, 0x73, 0x6E, 0x64, 0x62, 0x75, 0x73, 0x79});
    fixture.respond(fixture.reply_header(), ConstBytes{payload});

    REQUIRE(captured.has_value());
    REQUIRE(captured->code() == ErrorCode::ProtocolError);
    REQUIRE(captured->reason() == "busy");
    REQUIRE(captured->mgmt().has_value());
}
