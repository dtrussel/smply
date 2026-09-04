// SPDX-License-Identifier: Apache-2.0

#include "smp/assembler.hpp"

#include "../support/message_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

using smply::AssemblerLimits;
using smply::ConstBytes;
using smply::ErrorCode;
using smply::Group;
using smply::Header;
using smply::kHeaderSize;
using smply::MessageAssembler;
using smply::MessageSink;
using smply::Operation;
using smply::Version;
using smply::test::filler;
using smply::test::make_message;
using smply::test::make_raw_message;

namespace {

/// Records everything the assembler emits, copying each payload so a mistake in
/// the assembler's buffer lifetime shows up as a content mismatch (and, under
/// ASan, as a use-after-free at the point of the copy).
class Recorder final : public MessageSink
{
public:
    struct Message
    {
        Header header;
        std::vector<std::byte> payload;

        friend bool operator==(const Message&, const Message&) = default;
    };

    void on_message(const Header& header, ConstBytes payload) override
    {
        messages.push_back(Message{header, {payload.begin(), payload.end()}});
    }

    std::vector<Message> messages;
};

Header header_for(std::uint8_t seq, std::uint16_t payload_size)
{
    return Header{.op = Operation::WriteResponse,
                  .version = Version::V1,
                  .flags = 0,
                  .length = payload_size,
                  .group = Group::Image,
                  .seq = seq,
                  .command = 1};
}

/// A stream of three messages of deliberately awkward sizes: empty, one byte,
/// and larger than any plausible fragment.
std::vector<std::byte> three_message_stream()
{
    std::vector<std::byte> stream;
    for (const auto& message :
         {make_message(header_for(1, 0)), make_message(header_for(2, 0), filler(1, 0x10)),
          make_message(header_for(3, 0), filler(200, 0x20))}) {
        stream.insert(stream.end(), message.begin(), message.end());
    }
    return stream;
}

/// Feeds `stream` to a fresh assembler in fragments of `size`, returning what
/// came out.
std::vector<Recorder::Message> feed_in_fragments(ConstBytes stream, std::size_t size)
{
    MessageAssembler assembler;
    Recorder recorder;
    for (std::size_t offset = 0; offset < stream.size(); offset += size) {
        const std::size_t take = std::min(size, stream.size() - offset);
        const auto result = assembler.feed(stream.subspan(offset, take), recorder);
        REQUIRE(result.has_value());
    }
    return recorder.messages;
}

} // namespace

// ---------------------------------------------------------------------------
// The fragmentation invariant. This is the property the whole component exists
// to provide: how the bytes were chopped up must not be observable.
// ---------------------------------------------------------------------------

TEST_CASE("fragmentation is not observable", "[assembler][invariant]")
{
    const auto stream = three_message_stream();

    // The reference: everything in one call.
    MessageAssembler assembler;
    Recorder whole;
    REQUIRE(assembler.feed(ConstBytes{stream}, whole).has_value());
    REQUIRE(whole.messages.size() == 3);

    SECTION("every fixed fragment size from 1 to 64 agrees")
    {
        const std::size_t size = GENERATE(range(std::size_t{1}, std::size_t{65}));
        INFO("fragment size " << size);
        REQUIRE(feed_in_fragments(ConstBytes{stream}, size) == whole.messages);
    }

    SECTION("a fragment larger than the stream agrees")
    {
        REQUIRE(feed_in_fragments(ConstBytes{stream}, stream.size() * 2) == whole.messages);
    }

    SECTION("randomised cut points agree")
    {
        const unsigned seed = GENERATE(1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U);
        INFO("seed " << seed);

        std::mt19937 rng{seed};
        std::uniform_int_distribution<std::size_t> cut{1, 40};

        MessageAssembler chopped;
        Recorder recorder;
        std::size_t offset = 0;
        while (offset < stream.size()) {
            const std::size_t take = std::min(cut(rng), stream.size() - offset);
            REQUIRE(chopped.feed(ConstBytes{stream}.subspan(offset, take), recorder).has_value());
            offset += take;
        }

        REQUIRE(recorder.messages == whole.messages);
    }
}

TEST_CASE("a single feed can carry many messages", "[assembler]")
{
    const auto stream = three_message_stream();

    MessageAssembler assembler;
    Recorder recorder;
    REQUIRE(assembler.feed(ConstBytes{stream}, recorder).has_value());

    REQUIRE(recorder.messages.size() == 3);
    REQUIRE(recorder.messages[0].header.seq == 1);
    REQUIRE(recorder.messages[1].header.seq == 2);
    REQUIRE(recorder.messages[2].header.seq == 3);
    REQUIRE(recorder.messages[2].payload == filler(200, 0x20));
    REQUIRE(assembler.buffered() == 0);
}

TEST_CASE("payload content survives reassembly exactly", "[assembler]")
{
    const auto payload = filler(1000, 0x37);
    const auto message = make_message(header_for(9, 0), payload);

    // One byte at a time: the worst case for a buffer-management mistake.
    const auto received = feed_in_fragments(ConstBytes{message}, 1);

    REQUIRE(received.size() == 1);
    REQUIRE(received[0].payload == payload);
}

// ---------------------------------------------------------------------------
// Partial messages
// ---------------------------------------------------------------------------

TEST_CASE("a truncated message is buffered until it completes", "[assembler]")
{
    const auto message = make_message(header_for(4, 0), filler(32, 0x40));

    MessageAssembler assembler;
    Recorder recorder;

    SECTION("split inside the header")
    {
        REQUIRE(assembler.feed(ConstBytes{message}.first(3), recorder).has_value());
        REQUIRE(recorder.messages.empty());
        REQUIRE(assembler.buffered() == 3);

        REQUIRE(assembler.feed(ConstBytes{message}.subspan(3), recorder).has_value());
        REQUIRE(recorder.messages.size() == 1);
        REQUIRE(assembler.buffered() == 0);
    }

    SECTION("split exactly at the header boundary")
    {
        REQUIRE(assembler.feed(ConstBytes{message}.first(kHeaderSize), recorder).has_value());
        REQUIRE(recorder.messages.empty());

        REQUIRE(assembler.feed(ConstBytes{message}.subspan(kHeaderSize), recorder).has_value());
        REQUIRE(recorder.messages.size() == 1);
    }

    SECTION("split inside the payload")
    {
        REQUIRE(assembler.feed(ConstBytes{message}.first(kHeaderSize + 10), recorder).has_value());
        REQUIRE(recorder.messages.empty());

        REQUIRE(
            assembler.feed(ConstBytes{message}.subspan(kHeaderSize + 10), recorder).has_value());
        REQUIRE(recorder.messages.size() == 1);
        REQUIRE(recorder.messages[0].payload == filler(32, 0x40));
    }
}

TEST_CASE("a trailing partial message stays buffered", "[assembler]")
{
    auto stream = make_message(header_for(1, 0), filler(4, 0x50));
    const auto partial = make_message(header_for(2, 0), filler(100, 0x60));
    stream.insert(stream.end(), partial.begin(), partial.begin() + 20);

    MessageAssembler assembler;
    Recorder recorder;
    REQUIRE(assembler.feed(ConstBytes{stream}, recorder).has_value());

    REQUIRE(recorder.messages.size() == 1);
    REQUIRE(assembler.buffered() == 20);
}

TEST_CASE("an empty feed is a no-op", "[assembler]")
{
    MessageAssembler assembler;
    Recorder recorder;

    REQUIRE(assembler.feed(ConstBytes{}, recorder).has_value());
    REQUIRE(recorder.messages.empty());
    REQUIRE(assembler.buffered() == 0);

    // Also mid-message: an empty feed must not disturb buffered state.
    const auto message = make_message(header_for(1, 0), filler(8));
    REQUIRE(assembler.feed(ConstBytes{message}.first(5), recorder).has_value());
    REQUIRE(assembler.feed(ConstBytes{}, recorder).has_value());
    REQUIRE(assembler.buffered() == 5);
}

// ---------------------------------------------------------------------------
// Bounds. Everything here is untrusted input.
// ---------------------------------------------------------------------------

TEST_CASE("a payload beyond the limit is rejected without buffering it", "[assembler][bounds]")
{
    const AssemblerLimits limits{.max_payload = 64, .max_buffer = 1024};
    MessageAssembler assembler{limits};
    Recorder recorder;

    // Declares far more than it sends: the classic hostile frame.
    const auto message = make_raw_message(header_for(1, 65), filler(4));

    const auto result = assembler.feed(ConstBytes{message}, recorder);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code() == ErrorCode::MessageTooLarge);
    REQUIRE(recorder.messages.empty());
    // Nothing was retained on the device's behalf.
    REQUIRE(assembler.buffered() == 0);
    REQUIRE(assembler.peak_buffered() == 0);
}

TEST_CASE("a message beyond the buffer limit fails immediately, not eventually",
          "[assembler][bounds]")
{
    // max_payload permits it but max_buffer does not. The check must happen
    // before waiting for the remaining bytes, otherwise the stream stalls
    // forever waiting for data that would be refused on arrival.
    const AssemblerLimits limits{.max_payload = 8192, .max_buffer = 100};
    MessageAssembler assembler{limits};
    Recorder recorder;

    const auto message = make_raw_message(header_for(1, 200), filler(4));
    const auto result = assembler.feed(ConstBytes{message}, recorder);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code() == ErrorCode::MessageTooLarge);
    REQUIRE(assembler.buffered() == 0);
}

TEST_CASE("the buffer never exceeds the configured bound under hostile input",
          "[assembler][bounds]")
{
    const AssemblerLimits limits{.max_payload = 256, .max_buffer = 512};
    MessageAssembler assembler{limits};
    Recorder recorder;

    // A device that sends a header claiming the maximum and then dribbles.
    const auto message = make_raw_message(header_for(1, 256), filler(256));

    for (std::size_t offset = 0; offset < message.size(); ++offset) {
        REQUIRE(assembler.feed(ConstBytes{message}.subspan(offset, 1), recorder).has_value());
        REQUIRE(assembler.buffered() <= limits.max_buffer);
    }

    REQUIRE(recorder.messages.size() == 1);
    REQUIRE(assembler.peak_buffered() <= limits.max_buffer);
    // The bound is real, not merely observed: capacity is bounded too, so a
    // hostile peer cannot induce unbounded allocation.
    REQUIRE(assembler.capacity() <= limits.max_buffer);
}

TEST_CASE("a huge declared length allocates nothing", "[assembler][bounds]")
{
    MessageAssembler assembler;
    Recorder recorder;

    const auto capacity_before = assembler.capacity();
    // The largest the 16-bit field can express, far above the default bounds.
    const auto message = make_raw_message(header_for(1, 0xFFFF), filler(4));

    const auto result = assembler.feed(ConstBytes{message}, recorder);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code() == ErrorCode::MessageTooLarge);
    REQUIRE(assembler.capacity() == capacity_before);
}

TEST_CASE("a message exactly at the limits is accepted", "[assembler][bounds]")
{
    // Off-by-one guard on the bounds themselves.
    const std::uint16_t payload_size = 64;
    const AssemblerLimits limits{.max_payload = payload_size,
                                 .max_buffer = kHeaderSize + payload_size};
    MessageAssembler assembler{limits};
    Recorder recorder;

    const auto message = make_message(header_for(1, 0), filler(payload_size));
    REQUIRE(assembler.feed(ConstBytes{message}, recorder).has_value());
    REQUIRE(recorder.messages.size() == 1);
}

// ---------------------------------------------------------------------------
// Malformed framing
// ---------------------------------------------------------------------------

TEST_CASE("a malformed header aborts the stream", "[assembler]")
{
    MessageAssembler assembler;
    Recorder recorder;

    // Reserved bits set.
    const auto raw = smply::test::bytes_of({0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    const auto result = assembler.feed(ConstBytes{raw}, recorder);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code() == ErrorCode::MalformedMessage);
    // Framing is broken and there is no sentinel to resynchronise on, so the
    // buffer is discarded rather than left to misparse the next bytes.
    REQUIRE(assembler.buffered() == 0);
}

TEST_CASE("messages before a malformed one are still delivered", "[assembler]")
{
    auto stream = make_message(header_for(1, 0), filler(4));
    const auto bad = smply::test::bytes_of({0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    stream.insert(stream.end(), bad.begin(), bad.end());

    MessageAssembler assembler;
    Recorder recorder;
    const auto result = assembler.feed(ConstBytes{stream}, recorder);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(recorder.messages.size() == 1);
}

TEST_CASE("a reserved protocol version aborts the stream with its own code", "[assembler]")
{
    MessageAssembler assembler;
    Recorder recorder;

    const auto raw = smply::test::bytes_of({0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    const auto result = assembler.feed(ConstBytes{raw}, recorder);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code() == ErrorCode::UnsupportedSmpVersion);
}

TEST_CASE("a malformed header split across feeds is still caught", "[assembler]")
{
    MessageAssembler assembler;
    Recorder recorder;

    const auto raw = smply::test::bytes_of({0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    REQUIRE(assembler.feed(ConstBytes{raw}.first(4), recorder).has_value());

    const auto result = assembler.feed(ConstBytes{raw}.subspan(4), recorder);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code() == ErrorCode::MalformedMessage);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("reset discards a partial message", "[assembler]")
{
    const auto message = make_message(header_for(1, 0), filler(64));

    MessageAssembler assembler;
    Recorder recorder;
    REQUIRE(assembler.feed(ConstBytes{message}.first(20), recorder).has_value());
    REQUIRE(assembler.buffered() == 20);

    assembler.reset();
    REQUIRE(assembler.buffered() == 0);

    // The tail of the discarded message must not be parsed as a new one; a
    // fresh message after the reset comes through cleanly.
    REQUIRE(assembler.feed(ConstBytes{message}, recorder).has_value());
    REQUIRE(recorder.messages.size() == 1);
}

TEST_CASE("the assembler is reusable after an error", "[assembler]")
{
    MessageAssembler assembler;
    Recorder recorder;

    const auto bad = smply::test::bytes_of({0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    REQUIRE_FALSE(assembler.feed(ConstBytes{bad}, recorder).has_value());

    // The caller is expected to drop the connection, but the object itself must
    // not be left in a state that misbehaves.
    const auto good = make_message(header_for(7, 0), filler(4));
    REQUIRE(assembler.feed(ConstBytes{good}, recorder).has_value());
    REQUIRE(recorder.messages.size() == 1);
    REQUIRE(recorder.messages[0].header.seq == 7);
}

TEST_CASE("a re-entrant sink is refused rather than corrupting the buffer", "[assembler]")
{
    // Re-entering would mutate the buffer the payload span points into. A
    // diagnosable error beats a use-after-free.
    class ReentrantSink final : public MessageSink
    {
    public:
        MessageAssembler* assembler = nullptr;
        ErrorCode inner_code = ErrorCode::Ok;

        void on_message(const Header& /*header*/, ConstBytes /*payload*/) override
        {
            const auto result = assembler->feed(ConstBytes{}, *this);
            inner_code = result.has_value() ? ErrorCode::Ok : result.error().code();
        }
    };

    MessageAssembler assembler;
    ReentrantSink sink;
    sink.assembler = &assembler;

    const auto message = make_message(header_for(1, 0), filler(4));
    REQUIRE(assembler.feed(ConstBytes{message}, sink).has_value());
    REQUIRE(sink.inner_code == ErrorCode::InvalidState);
}

TEST_CASE("headers with unusual but legal fields pass through", "[assembler]")
{
    // The assembler must not develop opinions the codec does not have: unknown
    // groups and flags are legal and belong to layers above.
    Header header = header_for(200, 0);
    header.group = static_cast<Group>(9000);
    header.flags = 0xFF;
    header.command = 0xFE;

    const auto message = make_message(header, filler(3));

    MessageAssembler assembler;
    Recorder recorder;
    REQUIRE(assembler.feed(ConstBytes{message}, recorder).has_value());

    REQUIRE(recorder.messages.size() == 1);
    REQUIRE(smply::to_underlying(recorder.messages[0].header.group) == 9000);
    REQUIRE(recorder.messages[0].header.flags == 0xFF);
}
