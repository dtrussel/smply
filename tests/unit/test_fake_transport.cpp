// SPDX-License-Identifier: Apache-2.0
//
// P4's acceptance criterion is that the double can express every scenario the
// later phases need, so there is one test per scenario even where it is nearly
// trivial. Anything not expressible here becomes a blocker in P6 through P12,
// when there is far more code in flight and far less attention to spare.

#include "fake_transport.hpp"

#include "message_builder.hpp"
#include "smp/assembler.hpp"
#include "smply/transport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

using smply::ConstBytes;
using smply::Error;
using smply::ErrorCode;
using smply::Group;
using smply::Header;
using smply::Operation;
using smply::TransportListener;
using smply::Version;
using smply::test::FakeTransport;
using smply::test::filler;
using smply::test::make_message;

namespace {

/// Records callbacks, copying each buffer during the call so a lifetime mistake
/// shows up as corrupted content -- and, under ASan, as a use-after-free at the
/// point of the copy.
class Listener final : public TransportListener
{
public:
    std::vector<std::vector<std::byte>> chunks;
    std::vector<Error> transport_errors;
    std::vector<Error> disconnects;

    void on_bytes(ConstBytes bytes) override
    {
        chunks.emplace_back(bytes.begin(), bytes.end());
    }

    void on_transport_error(Error error) override
    {
        transport_errors.push_back(std::move(error));
    }

    void on_disconnected(Error error) override
    {
        disconnects.push_back(std::move(error));
    }

    /// Everything received, in order, with the chunk boundaries removed.
    [[nodiscard]] std::vector<std::byte> joined() const
    {
        std::vector<std::byte> out;
        for (const auto& chunk : chunks) {
            out.insert(out.end(), chunk.begin(), chunk.end());
        }
        return out;
    }
};

Header header_for(std::uint8_t seq)
{
    return Header{.op = Operation::WriteResponse,
                  .version = Version::V1,
                  .flags = 0,
                  .length = 0,
                  .group = Group::Image,
                  .seq = seq,
                  .command = 1};
}

struct Fixture
{
    FakeTransport transport;
    Listener listener;

    Fixture()
    {
        transport.set_listener(&listener);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

TEST_CASE("sent messages are recorded whole and in order", "[fake-transport]")
{
    Fixture fixture;

    const auto first = make_message(header_for(1), filler(4, 0x10));
    const auto second = make_message(header_for(2), filler(8, 0x20));

    REQUIRE(fixture.transport.send(ConstBytes{first}).has_value());
    REQUIRE(fixture.transport.send(ConstBytes{second}).has_value());

    REQUIRE(fixture.transport.send_count() == 2);
    REQUIRE(fixture.transport.sent()[0] == first);
    REQUIRE(fixture.transport.sent()[1] == second);

    const auto last = fixture.transport.last_sent();
    REQUIRE(std::vector<std::byte>(last.begin(), last.end()) == second);
}

TEST_CASE("send copies rather than retaining the caller's buffer", "[fake-transport]")
{
    // The contract says the buffer is borrowed for the call only. Sending from
    // a buffer that is then destroyed proves the transport took its own copy;
    // under ASan a retained span would be caught on the comparison below.
    Fixture fixture;
    std::vector<std::byte> expected;

    {
        const auto temporary = make_message(header_for(3), filler(16, 0x30));
        expected = temporary;
        REQUIRE(fixture.transport.send(ConstBytes{temporary}).has_value());
    }

    REQUIRE(fixture.transport.sent()[0] == expected);
}

TEST_CASE("max_message_size is configurable and defaults to unknown", "[fake-transport]")
{
    FakeTransport transport;

    // 0 means "no opinion"; the core falls back to its configured default.
    REQUIRE(transport.max_message_size() == 0);

    transport.set_max_message_size(252);
    REQUIRE(transport.max_message_size() == 252);
}

// ---------------------------------------------------------------------------
// Delivery patterns. These are what make the reassembler and, later, the
// client testable against arbitrary fragmentation.
// ---------------------------------------------------------------------------

TEST_CASE("deliver hands everything over in one call", "[fake-transport]")
{
    Fixture fixture;
    const auto message = make_message(header_for(1), filler(20));

    fixture.transport.deliver(ConstBytes{message});

    REQUIRE(fixture.listener.chunks.size() == 1);
    REQUIRE(fixture.listener.chunks[0] == message);
}

TEST_CASE("deliver_fragmented splits into fixed-size chunks", "[fake-transport]")
{
    Fixture fixture;
    const auto message = make_message(header_for(1), filler(20)); // 28 bytes

    fixture.transport.deliver_fragmented(ConstBytes{message}, 8);

    // 28 bytes in eights: 8, 8, 8, 4.
    REQUIRE(fixture.listener.chunks.size() == 4);
    REQUIRE(fixture.listener.chunks[0].size() == 8);
    REQUIRE(fixture.listener.chunks[3].size() == 4);
    REQUIRE(fixture.listener.joined() == message);
}

TEST_CASE("a fragment larger than the buffer yields a single chunk", "[fake-transport]")
{
    Fixture fixture;
    const auto message = make_message(header_for(1), filler(4));

    fixture.transport.deliver_fragmented(ConstBytes{message}, 1000);

    REQUIRE(fixture.listener.chunks.size() == 1);
    REQUIRE(fixture.listener.joined() == message);
}

TEST_CASE("deliver_byte_by_byte issues one call per byte", "[fake-transport]")
{
    Fixture fixture;
    const auto message = make_message(header_for(1), filler(5));

    fixture.transport.deliver_byte_by_byte(ConstBytes{message});

    REQUIRE(fixture.listener.chunks.size() == message.size());
    for (const auto& chunk : fixture.listener.chunks) {
        REQUIRE(chunk.size() == 1);
    }
    REQUIRE(fixture.listener.joined() == message);
}

TEST_CASE("deliver_concatenated carries several messages in one call", "[fake-transport]")
{
    // The device answered twice and both responses arrived together -- a real
    // and easily mishandled case.
    Fixture fixture;
    const auto first = make_message(header_for(1), filler(4, 0x10));
    const auto second = make_message(header_for(2), filler(4, 0x20));

    const std::vector<ConstBytes> buffers{ConstBytes{first}, ConstBytes{second}};
    fixture.transport.deliver_concatenated(buffers);

    REQUIRE(fixture.listener.chunks.size() == 1);
    REQUIRE(fixture.listener.chunks[0].size() == first.size() + second.size());

    std::vector<std::byte> expected = first;
    expected.insert(expected.end(), second.begin(), second.end());
    REQUIRE(fixture.listener.chunks[0] == expected);
}

TEST_CASE("deliver_split_at places boundaries exactly where asked", "[fake-transport]")
{
    Fixture fixture;
    const auto message = make_message(header_for(1), filler(12)); // 20 bytes

    // Mid-header, at the header boundary, and one byte before the end.
    const std::vector<std::size_t> cuts{3, 8, 19};
    fixture.transport.deliver_split_at(ConstBytes{message}, cuts);

    REQUIRE(fixture.listener.chunks.size() == 4);
    REQUIRE(fixture.listener.chunks[0].size() == 3);
    REQUIRE(fixture.listener.chunks[1].size() == 5);
    REQUIRE(fixture.listener.chunks[2].size() == 11);
    REQUIRE(fixture.listener.chunks[3].size() == 1);
    REQUIRE(fixture.listener.joined() == message);
}

TEST_CASE("deliver_split_at ignores unusable cut offsets", "[fake-transport]")
{
    // So a fixed cut list can be reused across inputs of differing length
    // without every call site having to sanitise it.
    Fixture fixture;
    const auto message = make_message(header_for(1), filler(2)); // 10 bytes

    const std::vector<std::size_t> cuts{0, 4, 4, 10, 99};
    fixture.transport.deliver_split_at(ConstBytes{message}, cuts);

    REQUIRE(fixture.listener.joined() == message);
    REQUIRE(fixture.listener.chunks.size() == 2); // only the cut at 4 applies
}

TEST_CASE("delivery borrows the buffer for the call only", "[fake-transport]")
{
    // Under ASan this is the test that catches a listener span outliving its
    // backing store.
    Fixture fixture;
    std::vector<std::byte> expected;

    {
        const auto temporary = make_message(header_for(1), filler(32, 0x77));
        expected = temporary;
        fixture.transport.deliver_fragmented(ConstBytes{temporary}, 7);
    }

    REQUIRE(fixture.listener.joined() == expected);
}

TEST_CASE("a delayed response is just a later delivery", "[fake-transport]")
{
    // "No response yet" needs no API: the test simply does not deliver. This
    // is how timeout paths are driven, together with ManualClock.
    Fixture fixture;
    const auto message = make_message(header_for(1), filler(4));

    REQUIRE(fixture.transport.send(ConstBytes{message}).has_value());
    REQUIRE(fixture.listener.chunks.empty()); // nothing came back

    fixture.transport.deliver(ConstBytes{message});
    REQUIRE(fixture.listener.chunks.size() == 1);
}

TEST_CASE("a duplicate response is delivered twice", "[fake-transport]")
{
    Fixture fixture;
    const auto message = make_message(header_for(1), filler(4));

    fixture.transport.deliver(ConstBytes{message});
    fixture.transport.deliver(ConstBytes{message});

    REQUIRE(fixture.listener.chunks.size() == 2);
    REQUIRE(fixture.listener.chunks[0] == fixture.listener.chunks[1]);
}

TEST_CASE("arbitrary and malformed bytes can be injected", "[fake-transport]")
{
    // Malformed headers and malformed CBOR are ordinary byte buffers as far as
    // the transport is concerned; it has no opinion about content.
    Fixture fixture;
    const auto garbage = smply::test::bytes_of({0xE0, 0xFF, 0xFF, 0xFF, 0xDE, 0xAD, 0xBE, 0xEF});

    fixture.transport.deliver(ConstBytes{garbage});

    REQUIRE(fixture.listener.chunks.size() == 1);
    REQUIRE(fixture.listener.chunks[0] == garbage);
}

// ---------------------------------------------------------------------------
// Faults
// ---------------------------------------------------------------------------

TEST_CASE("fail_next_send fails exactly one send", "[fake-transport]")
{
    Fixture fixture;
    const auto message = make_message(header_for(1), filler(4));

    fixture.transport.fail_next_send(Error{ErrorCode::TransportError, "injected"});

    const auto failed = fixture.transport.send(ConstBytes{message});
    REQUIRE_FALSE(failed.has_value());
    REQUIRE(failed.error().code() == ErrorCode::TransportError);
    REQUIRE(fixture.transport.send_count() == 0); // not recorded as sent

    REQUIRE(fixture.transport.send(ConstBytes{message}).has_value());
    REQUIRE(fixture.transport.send_count() == 1);
}

TEST_CASE("a busy transport reports TransportBusy until cleared", "[fake-transport]")
{
    Fixture fixture;
    const auto message = make_message(header_for(1), filler(4));

    fixture.transport.set_busy(true);

    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto result = fixture.transport.send(ConstBytes{message});
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code() == ErrorCode::TransportBusy);
    }
    REQUIRE(fixture.transport.send_count() == 0);

    fixture.transport.set_busy(false);
    REQUIRE(fixture.transport.send(ConstBytes{message}).has_value());
    REQUIRE(fixture.transport.send_count() == 1);
}

TEST_CASE("a transport error is reported without taking the link down", "[fake-transport]")
{
    Fixture fixture;

    fixture.transport.raise_transport_error(Error{ErrorCode::TransportError, "glitch"});

    REQUIRE(fixture.listener.transport_errors.size() == 1);
    REQUIRE(fixture.listener.disconnects.empty());
    REQUIRE(fixture.transport.connected());

    // The link is still usable, which is the whole distinction from disconnect.
    const auto message = make_message(header_for(1), filler(4));
    REQUIRE(fixture.transport.send(ConstBytes{message}).has_value());
}

TEST_CASE("disconnect is terminal for callbacks", "[fake-transport]")
{
    Fixture fixture;
    const auto message = make_message(header_for(1), filler(4));

    fixture.transport.disconnect();

    REQUIRE(fixture.listener.disconnects.size() == 1);
    REQUIRE(fixture.listener.disconnects[0].code() == ErrorCode::Disconnected);
    REQUIRE_FALSE(fixture.transport.connected());

    // The contract forbids any further callback. The double enforces it and
    // counts the attempt, so a test relying on one fails loudly.
    fixture.transport.deliver(ConstBytes{message});
    fixture.transport.raise_transport_error(Error{ErrorCode::TransportError});
    fixture.transport.disconnect();

    REQUIRE(fixture.listener.chunks.empty());
    REQUIRE(fixture.listener.disconnects.size() == 1);
    REQUIRE(fixture.transport.suppressed_deliveries() == 3);
}

TEST_CASE("sending after a disconnect fails", "[fake-transport]")
{
    Fixture fixture;
    const auto message = make_message(header_for(1), filler(4));

    fixture.transport.disconnect();

    const auto result = fixture.transport.send(ConstBytes{message});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code() == ErrorCode::Disconnected);
}

TEST_CASE("a custom disconnect reason is carried through", "[fake-transport]")
{
    Fixture fixture;

    fixture.transport.disconnect(Error{ErrorCode::TransportError, "radio off"});

    REQUIRE(fixture.listener.disconnects.size() == 1);
    REQUIRE(fixture.listener.disconnects[0].code() == ErrorCode::TransportError);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("close stops callbacks and is idempotent", "[fake-transport]")
{
    Fixture fixture;
    const auto message = make_message(header_for(1), filler(4));

    fixture.transport.close();
    fixture.transport.close(); // idempotent

    REQUIRE(fixture.transport.closed());

    fixture.transport.deliver(ConstBytes{message});
    REQUIRE(fixture.listener.chunks.empty());
    REQUIRE(fixture.transport.suppressed_deliveries() == 1);

    // close() does not report on_disconnected: the caller asked for it.
    REQUIRE(fixture.listener.disconnects.empty());
}

TEST_CASE("delivery without a listener is suppressed, not a crash", "[fake-transport]")
{
    FakeTransport transport;
    const auto message = make_message(header_for(1), filler(4));

    transport.deliver(ConstBytes{message});
    REQUIRE(transport.suppressed_deliveries() == 1);
}

TEST_CASE("clearing the listener stops delivery", "[fake-transport]")
{
    Fixture fixture;
    const auto message = make_message(header_for(1), filler(4));

    fixture.transport.deliver(ConstBytes{message});
    REQUIRE(fixture.listener.chunks.size() == 1);

    fixture.transport.set_listener(nullptr);
    fixture.transport.deliver(ConstBytes{message});

    REQUIRE(fixture.listener.chunks.size() == 1);
    REQUIRE(fixture.transport.suppressed_deliveries() == 1);
}

TEST_CASE("clear_sent resets the recording without disturbing the link", "[fake-transport]")
{
    // Useful when a test wants to assert on the commands issued by one phase of
    // a workflow without counting everything that came before.
    Fixture fixture;
    const auto message = make_message(header_for(1), filler(4));

    REQUIRE(fixture.transport.send(ConstBytes{message}).has_value());
    fixture.transport.clear_sent();

    REQUIRE(fixture.transport.send_count() == 0);
    REQUIRE(fixture.transport.connected());
    REQUIRE(fixture.transport.send(ConstBytes{message}).has_value());
    REQUIRE(fixture.transport.send_count() == 1);
}

TEST_CASE("on_bytes_calls counts deliveries actually made", "[fake-transport]")
{
    Fixture fixture;
    const auto message = make_message(header_for(1), filler(9)); // 17 bytes

    fixture.transport.deliver_fragmented(ConstBytes{message}, 5);
    REQUIRE(fixture.transport.on_bytes_calls() == 4);

    fixture.transport.close();
    fixture.transport.deliver(ConstBytes{message});
    REQUIRE(fixture.transport.on_bytes_calls() == 4); // suppressed, not counted
}

TEST_CASE("FakeTransport satisfies the Transport interface", "[fake-transport]")
{
    // Used through the base class everywhere from P6 onwards.
    Fixture fixture;
    smply::Transport& transport = fixture.transport;

    const auto message = make_message(header_for(1), filler(4));
    REQUIRE(transport.send(ConstBytes{message}).has_value());
    REQUIRE(transport.max_message_size() == 0);
    transport.close();
}

// ---------------------------------------------------------------------------
// Composition with the reassembler. The transport and the assembler are the two
// halves of the inbound path, and every phase from P6 onwards depends on them
// fitting together, so prove it here rather than discovering it later.
// ---------------------------------------------------------------------------

TEST_CASE("transport fragmentation is invisible once reassembled", "[fake-transport][assembler]")
{
    /// Bridges the transport's listener interface to the assembler's sink.
    class Bridge final : public TransportListener, public smply::MessageSink
    {
    public:
        smply::MessageAssembler assembler;
        std::vector<Header> headers;
        std::vector<std::vector<std::byte>> payloads;
        std::vector<Error> errors;

        void on_bytes(ConstBytes bytes) override
        {
            if (auto result = assembler.feed(bytes, *this); !result) {
                errors.push_back(result.error());
            }
        }

        void on_transport_error(Error error) override
        {
            errors.push_back(std::move(error));
        }

        void on_disconnected(Error error) override
        {
            assembler.reset();
            errors.push_back(std::move(error));
        }

        void on_message(const Header& header, ConstBytes payload) override
        {
            headers.push_back(header);
            payloads.emplace_back(payload.begin(), payload.end());
        }
    };

    auto stream = make_message(header_for(1), filler(4, 0x10));
    const auto second = make_message(header_for(2), filler(100, 0x20));
    stream.insert(stream.end(), second.begin(), second.end());

    FakeTransport transport;
    Bridge bridge;
    transport.set_listener(&bridge);

    SECTION("one call")
    {
        transport.deliver(ConstBytes{stream});
    }
    SECTION("byte by byte")
    {
        transport.deliver_byte_by_byte(ConstBytes{stream});
    }
    SECTION("awkward fragments")
    {
        transport.deliver_fragmented(ConstBytes{stream}, 7);
    }
    SECTION("boundaries in the worst places")
    {
        const std::vector<std::size_t> cuts{1, 7, 8, 9, 11, 12, 19, 20};
        transport.deliver_split_at(ConstBytes{stream}, cuts);
    }

    REQUIRE(bridge.errors.empty());
    REQUIRE(bridge.headers.size() == 2);
    REQUIRE(bridge.headers[0].seq == 1);
    REQUIRE(bridge.headers[1].seq == 2);
    REQUIRE(bridge.payloads[0] == filler(4, 0x10));
    REQUIRE(bridge.payloads[1] == filler(100, 0x20));
}
