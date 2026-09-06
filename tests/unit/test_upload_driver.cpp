// SPDX-License-Identifier: Apache-2.0
//
// The I/O half: what actually goes on the wire, when the callbacks fire, and
// that they fire exactly once. The decisions themselves are tested without a
// transport in test_upload_session.cpp -- if a case here is really about a
// rule, it belongs there.
//
// The first-packet vector below is hand-derived from the CBOR grammar and the
// field names in docs/protocol-notes.md section 6, not captured from the
// writer: an encoder checked against its own output proves only that it is
// deterministic.

#include "smply/groups/image.hpp"

#include "fake_image_source.hpp"
#include "fake_transport.hpp"
#include "manual_clock.hpp"
#include "message_builder.hpp"
#include "smply/image_source.hpp"
#include "smply/limits.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using smply::ConstBytes;
using smply::ErrorCode;
using smply::Group;
using smply::Hash;
using smply::Header;
using smply::ImageManagement;
using smply::MemoryImageSource;
using smply::Operation;
using smply::Result;
using smply::SmpClient;
using smply::UploadHandle;
using smply::UploadOptions;
using smply::UploadProgress;
using smply::UploadResult;
using smply::Version;
using smply::test::bytes_of;
using smply::test::FailingImageSource;
using smply::test::FakeTransport;
using smply::test::make_message;
using smply::test::ManualClock;
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

constexpr std::uint32_t kChunk = 40;
constexpr std::size_t kImageSize = 100;

std::vector<std::byte> image_bytes(std::size_t count = kImageSize)
{
    std::vector<std::byte> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(static_cast<std::byte>(i & 0xFFU));
    }
    return out;
}

/// What an upload reported, and how many times.
struct Outcome
{
    int calls = 0;
    std::optional<ErrorCode> code;
    std::optional<UploadResult> value;
    std::vector<UploadProgress> progress;

    [[nodiscard]] auto on_done()
    {
        return [this](Result<UploadResult> result) {
            ++calls;
            if (result.has_value()) {
                value = *result;
            } else {
                code = result.error().code();
            }
        };
    }

    [[nodiscard]] auto on_progress()
    {
        return [this](UploadProgress step) { progress.push_back(step); };
    }
};

/// Transport, clock, client and group.
///
/// Declaration order matters: everything a callback touches must outlive the
/// client, whose destructor completes outstanding requests.
struct Fixture
{
    std::vector<std::byte> image = image_bytes();
    Outcome outcome;
    FakeTransport transport;
    ManualClock clock;
    SmpClient client;
    MemoryImageSource source{ConstBytes{image}};
    ImageManagement management;

    Fixture() : client{transport, clock}, management{client} {}

    [[nodiscard]] static UploadOptions options()
    {
        UploadOptions out;
        out.chunk_size = kChunk;
        out.sha = Hash{};
        return out;
    }

    [[nodiscard]] UploadHandle start()
    {
        return management.upload(source, options(), outcome.on_progress(), outcome.on_done());
    }

    [[nodiscard]] std::vector<std::byte> payload_of(std::size_t index) const
    {
        const auto& message = transport.sent().at(index);
        REQUIRE(message.size() >= smply::kHeaderSize);
        return {message.begin() + smply::kHeaderSize, message.end()};
    }

    [[nodiscard]] Header header_of(std::size_t index) const
    {
        const auto decoded = smply::decode_header(ConstBytes{transport.sent().at(index)});
        REQUIRE(decoded.has_value());
        return *decoded;
    }

    /// Answers the last request with `{"off": off}` plus an optional `match`.
    void respond(std::uint64_t off, std::optional<bool> match = std::nullopt)
    {
        std::vector<std::byte> payload = match.has_value() ? bytes_of({0xA2}) : bytes_of({0xA1});
        const auto key = bytes_of({0x63, 'o', 'f', 'f'});
        payload.insert(payload.end(), key.begin(), key.end());
        if (off < 24) {
            payload.push_back(static_cast<std::byte>(off));
        } else if (off <= 0xFF) {
            payload.push_back(std::byte{0x18});
            payload.push_back(static_cast<std::byte>(off));
        } else {
            payload.push_back(std::byte{0x19});
            payload.push_back(static_cast<std::byte>((off >> 8U) & 0xFFU));
            payload.push_back(static_cast<std::byte>(off & 0xFFU));
        }
        if (match.has_value()) {
            const auto match_key = bytes_of({0x65, 'm', 'a', 't', 'c', 'h'});
            payload.insert(payload.end(), match_key.begin(), match_key.end());
            payload.push_back(*match ? std::byte{0xF5} : std::byte{0xF4});
        }

        const Header request = header_of(transport.send_count() - 1);
        const Header reply{.op = smply::response_to(request.op),
                           .version = Version::V1,
                           .flags = 0,
                           .length = 0,
                           .group = request.group,
                           .seq = request.seq,
                           .command = request.command};
        const auto message = make_message(reply, ConstBytes{payload});
        transport.deliver(ConstBytes{message});
    }

    /// Answers with a flat SMP error code.
    void respond_error(std::uint8_t rc)
    {
        const auto payload = bytes_of({0xA1, 0x62, 'r', 'c', rc});
        const Header request = header_of(transport.send_count() - 1);
        const Header reply{.op = smply::response_to(request.op),
                           .version = Version::V1,
                           .flags = 0,
                           .length = 0,
                           .group = request.group,
                           .seq = request.seq,
                           .command = request.command};
        const auto message = make_message(reply, ConstBytes{payload});
        transport.deliver(ConstBytes{message});
    }
};

} // namespace

// ---------------------------------------------------------------------------
// What goes on the wire
// ---------------------------------------------------------------------------

TEST_CASE("the first packet carries len, sha, image and off", "[upload][driver][encoding]")
{
    // {"image": 0, "len": 100, "off": 0, "sha": h'00*32', "data": h'<40 bytes>'}
    Fixture fixture;

    const UploadHandle handle = fixture.start();
    REQUIRE(handle.valid());
    REQUIRE(fixture.transport.send_count() == 1);

    const Header header = fixture.header_of(0);
    REQUIRE(header.op == Operation::Write);
    REQUIRE(header.group == Group::Image);
    REQUIRE(header.command == 1);

    auto expected = bytes_of({0xA5,                                  // map(5)
                              0x65, 'i', 'm', 'a', 'g',  'e',  0x00, // "image": 0
                              0x63, 'l', 'e', 'n', 0x18, 0x64,       // "len": 100
                              0x63, 'o', 'f', 'f', 0x00,             // "off": 0
                              0x63, 's', 'h', 'a', 0x58, 0x20});     // "sha": bstr(32)
    expected.insert(expected.end(), 32, std::byte{0});
    const auto data_key = bytes_of({0x64, 'd', 'a', 't', 'a', 0x58, 0x28}); // "data": bstr(40)
    expected.insert(expected.end(), data_key.begin(), data_key.end());
    for (std::size_t i = 0; i < kChunk; ++i) {
        expected.push_back(static_cast<std::byte>(i));
    }

    REQUIRE(fixture.payload_of(0) == expected);
}

TEST_CASE("later packets carry only off and data", "[upload][driver][encoding]")
{
    // Rule 7's fields belong to a first packet; repeating them every time would
    // waste payload the chunk could have used.
    Fixture fixture;

    static_cast<void>(fixture.start());
    fixture.respond(kChunk);

    REQUIRE(fixture.transport.send_count() == 2);
    auto expected = bytes_of({0xA2,                            // map(2)
                              0x63, 'o', 'f', 'f', 0x18, 0x28, // "off": 40
                              0x64, 'd', 'a', 't', 'a', 0x58, 0x28});
    for (std::size_t i = 0; i < kChunk; ++i) {
        expected.push_back(static_cast<std::byte>(kChunk + i));
    }

    REQUIRE(fixture.payload_of(1) == expected);
}

TEST_CASE("the final chunk is short and the upload completes", "[upload][driver]")
{
    Fixture fixture;

    static_cast<void>(fixture.start());
    fixture.respond(40);
    fixture.respond(80);
    // 100 - 80 = 20 bytes left.
    REQUIRE(fixture.payload_of(2).back() == static_cast<std::byte>(99));
    fixture.respond(100, true);

    REQUIRE(fixture.outcome.calls == 1);
    REQUIRE(fixture.outcome.value.has_value());
    REQUIRE(fixture.outcome.value->transferred == 100);
    REQUIRE(fixture.outcome.value->match == true);
}

TEST_CASE("the upgrade flag is sent only when asked for", "[upload][driver][encoding]")
{
    // A11: whether the version comparison includes the build number is a
    // Kconfig option, so this is never set by default.
    Fixture fixture;

    UploadOptions options = Fixture::options();
    options.upgrade_only = true;
    static_cast<void>(fixture.management.upload(
        fixture.source, options, fixture.outcome.on_progress(), fixture.outcome.on_done()));

    const auto payload = fixture.payload_of(0);
    REQUIRE(payload.front() == std::byte{0xA6}); // one more key than the default
    const auto needle = bytes_of({0x67, 'u', 'p', 'g', 'r', 'a', 'd', 'e', 0xF5});
    REQUIRE(std::search(payload.begin(), payload.end(), needle.begin(), needle.end()) !=
            payload.end());
}

// ---------------------------------------------------------------------------
// Timeouts and retransmission
// ---------------------------------------------------------------------------

TEST_CASE("the first chunk gets the long timeout and the rest the short one",
          "[upload][driver][timeout]")
{
    // A7: the first chunk may trigger an implicit slot erase of unbounded
    // duration, so it cannot share the ordinary deadline.
    Fixture fixture;

    static_cast<void>(fixture.start());
    const auto first = fixture.client.next_deadline();
    REQUIRE(first.has_value());
    REQUIRE(*first == fixture.clock.now() + smply::limits::kFirstChunkTimeout);

    fixture.respond(kChunk);
    const auto second = fixture.client.next_deadline();
    REQUIRE(second.has_value());
    REQUIRE(*second == fixture.clock.now() + smply::limits::kDefaultTimeout);
}

TEST_CASE("a timeout retransmits an identical payload", "[upload][driver][timeout]")
{
    // The payload has to be identical, not merely equivalent: the server may
    // have seen the first copy, and two different requests at the same offset
    // would be two different sessions to it.
    //
    // The SMP header is *not* identical, and must not be: the timeout retired
    // the old sequence number, so a reply carrying it would be discarded as
    // late (docs/protocol-notes.md section 4). A retransmission is a new
    // request carrying the same bytes.
    Fixture fixture;

    static_cast<void>(fixture.start());
    fixture.respond(kChunk);
    REQUIRE(fixture.transport.send_count() == 2);

    fixture.clock.advance(smply::limits::kDefaultTimeout);
    fixture.client.poll(fixture.clock.now());

    REQUIRE(fixture.transport.send_count() == 3);
    REQUIRE(fixture.payload_of(1) == fixture.payload_of(2));
    REQUIRE(fixture.header_of(2).seq != fixture.header_of(1).seq);
    REQUIRE(fixture.outcome.calls == 0);
}

TEST_CASE("retransmissions are bounded and then the upload fails", "[upload][driver][timeout]")
{
    Fixture fixture;

    static_cast<void>(fixture.start());
    fixture.respond(kChunk);

    for (std::uint32_t i = 0; i < smply::limits::kMaxChunkRetries + 1; ++i) {
        fixture.clock.advance(smply::limits::kDefaultTimeout);
        fixture.client.poll(fixture.clock.now());
    }

    REQUIRE(fixture.outcome.calls == 1);
    REQUIRE(fixture.outcome.code == ErrorCode::Timeout);
}

TEST_CASE("a busy device is retried rather than abandoned", "[upload][driver]")
{
    Fixture fixture;

    static_cast<void>(fixture.start());
    fixture.respond_error(10); // MGMT_ERR_EBUSY

    REQUIRE(fixture.outcome.calls == 0);
    REQUIRE(fixture.transport.send_count() == 2);
    REQUIRE(fixture.payload_of(0) == fixture.payload_of(1));
}

TEST_CASE("an ordinary device error stops the upload", "[upload][driver]")
{
    Fixture fixture;

    static_cast<void>(fixture.start());
    fixture.respond_error(3); // MGMT_ERR_EINVAL

    REQUIRE(fixture.outcome.calls == 1);
    REQUIRE(fixture.outcome.code == ErrorCode::ProtocolError);
}

TEST_CASE("a wrong-typed offset is a decode failure, not an absent one",
          "[upload][driver][hostile]")
{
    // A wrong type poisons the reader and makes every field look absent, so
    // without the status() check this would read as a success with no offset --
    // a different error, and a misleading one.
    Fixture fixture;

    static_cast<void>(fixture.start());

    // {"off": "forty"}
    const auto payload = bytes_of({0xA1, 0x63, 'o', 'f', 'f', 0x65, 'f', 'o', 'r', 't', 'y'});
    const Header request = fixture.header_of(0);
    const Header reply{.op = smply::response_to(request.op),
                       .version = Version::V1,
                       .flags = 0,
                       .length = 0,
                       .group = request.group,
                       .seq = request.seq,
                       .command = request.command};
    const auto message = make_message(reply, ConstBytes{payload});
    fixture.transport.deliver(ConstBytes{message});

    REQUIRE(fixture.outcome.calls == 1);
    REQUIRE(fixture.outcome.code == ErrorCode::CborDecode);
}

TEST_CASE("cancelling a finished upload is a no-op", "[upload][driver]")
{
    Fixture fixture;

    const UploadHandle handle = fixture.start();
    fixture.respond(100, true);
    REQUIRE(fixture.outcome.calls == 1);

    fixture.management.cancel(handle);
    fixture.client.poll(fixture.clock.now());

    REQUIRE(fixture.outcome.calls == 1);
}

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------

TEST_CASE("progress is reported only on confirmed advance", "[upload][driver]")
{
    // Never on send: progress read from what was put on the wire would
    // overstate what the device stored.
    Fixture fixture;

    static_cast<void>(fixture.start());
    REQUIRE(fixture.outcome.progress.empty());

    fixture.respond(40);
    REQUIRE(fixture.outcome.progress.size() == 1);
    REQUIRE(fixture.outcome.progress.back().transferred == 40);
    REQUIRE(fixture.outcome.progress.back().total == 100);

    fixture.respond(80);
    fixture.respond(100);

    REQUIRE(fixture.outcome.progress.size() == 3);
    REQUIRE(fixture.outcome.progress.back().transferred == 100);
}

TEST_CASE("a repeated offset reports no progress", "[upload][driver]")
{
    Fixture fixture;

    static_cast<void>(fixture.start());
    fixture.respond(40);
    fixture.respond(40);

    REQUIRE(fixture.outcome.progress.size() == 1);
}

TEST_CASE("a device that already holds the image completes at once", "[upload][driver]")
{
    // The first packet is answered with the whole size: nothing was
    // transferred, and progress still ends at 100 %.
    Fixture fixture;

    static_cast<void>(fixture.start());
    fixture.respond(100, true);

    REQUIRE(fixture.transport.send_count() == 1);
    REQUIRE(fixture.outcome.calls == 1);
    REQUIRE(fixture.outcome.value.has_value());
    REQUIRE(fixture.outcome.value->transferred == 100);
    REQUIRE(fixture.outcome.progress.size() == 1);
    REQUIRE(fixture.outcome.progress.back().transferred == 100);
}

TEST_CASE("a mismatching device fails the upload", "[upload][driver]")
{
    Fixture fixture;

    static_cast<void>(fixture.start());
    fixture.respond(40);
    fixture.respond(80);
    fixture.respond(100, false);

    REQUIRE(fixture.outcome.calls == 1);
    REQUIRE(fixture.outcome.code == ErrorCode::ImageMismatch);
}

// ---------------------------------------------------------------------------
// Exactly once
// ---------------------------------------------------------------------------

TEST_CASE("cancelling completes the upload exactly once", "[upload][driver]")
{
    Fixture fixture;

    const UploadHandle handle = fixture.start();
    fixture.management.cancel(handle);

    REQUIRE(fixture.outcome.calls == 0); // deferred, like every other cancellation
    fixture.client.poll(fixture.clock.now());

    REQUIRE(fixture.outcome.calls == 1);
    REQUIRE(fixture.outcome.code == ErrorCode::Cancelled);
    REQUIRE_FALSE(fixture.management.uploading(handle));
}

TEST_CASE("cancelling twice still completes once", "[upload][driver]")
{
    Fixture fixture;

    const UploadHandle handle = fixture.start();
    fixture.management.cancel(handle);
    fixture.management.cancel(handle);
    fixture.client.poll(fixture.clock.now());

    REQUIRE(fixture.outcome.calls == 1);
}

TEST_CASE("a stale handle cannot cancel a later upload", "[upload][driver]")
{
    // Declared before the fixture: the second upload is still running at the
    // end of the test, so ~ImageManagement completes its callback -- which
    // reaches this object. Anything a callback touches must outlive both the
    // client and the group (smp_client.hpp).
    Outcome second_outcome;
    Fixture fixture;

    const UploadHandle first = fixture.start();
    fixture.management.cancel(first);
    fixture.client.poll(fixture.clock.now());
    REQUIRE(fixture.outcome.calls == 1);

    const UploadHandle second = fixture.management.upload(
        fixture.source, Fixture::options(), second_outcome.on_progress(), second_outcome.on_done());
    REQUIRE(second.valid());
    REQUIRE(second != first);

    fixture.management.cancel(first); // inert
    fixture.client.poll(fixture.clock.now());

    REQUIRE(second_outcome.calls == 0);
    REQUIRE(fixture.management.uploading(second));
}

TEST_CASE("destroying the group completes the upload exactly once", "[upload][driver]")
{
    // There is no later poll() to defer to, so the callback runs inline -- the
    // same exception ~SmpClient makes.
    std::vector<std::byte> image = image_bytes();
    Outcome outcome;
    FakeTransport transport;
    const ManualClock clock;
    SmpClient client{transport, clock};
    MemoryImageSource source{ConstBytes{image}};

    {
        ImageManagement management{client};
        static_cast<void>(management.upload(source, Fixture::options(), outcome.on_progress(),
                                            outcome.on_done()));
        REQUIRE(outcome.calls == 0);
    }

    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::Cancelled);

    // The cancellation the destructor queued in the client must not reach the
    // driver that has since gone away.
    client.poll(clock.now());
    REQUIRE(outcome.calls == 1);
}

TEST_CASE("a second upload while one is running is refused", "[upload][driver]")
{
    Fixture fixture;

    static_cast<void>(fixture.start());

    Outcome second;
    const UploadHandle handle = fixture.management.upload(fixture.source, Fixture::options(),
                                                          second.on_progress(), second.on_done());

    REQUIRE_FALSE(handle.valid());
    REQUIRE(second.calls == 0);
    fixture.client.poll(fixture.clock.now());
    REQUIRE(second.calls == 1);
    REQUIRE(second.code == ErrorCode::InvalidState);
}

// ---------------------------------------------------------------------------
// Disconnect and resume
// ---------------------------------------------------------------------------

TEST_CASE("a disconnect completes once and leaves the session resumable",
          "[upload][driver][resume]")
{
    Fixture fixture;

    const UploadHandle handle = fixture.start();
    fixture.respond(40);
    REQUIRE(fixture.management.transferred(handle) == 40);

    fixture.transport.disconnect();

    REQUIRE(fixture.outcome.calls == 1);
    REQUIRE(fixture.outcome.code == ErrorCode::Disconnected);
    // The offset survives, which is what makes resuming worth doing.
    REQUIRE(fixture.management.transferred(handle) == 40);
}

TEST_CASE("resuming re-sends a first packet and continues from the device's offset",
          "[upload][driver][resume]")
{
    // Rule 6: the server matches the sha against its in-progress session and
    // answers with the offset it holds.
    // Declared before the fixture on purpose: SmpClient detaches from the
    // transport it currently holds when it is destroyed, so every transport it
    // has ever been bound to must outlive it (smp_client.hpp). Declaring this
    // inside the test body instead aborts on "pure virtual method called" --
    // under GCC; the Clang build happened not to notice.
    FakeTransport reconnected;
    // Before the fixture for the same reason: the resumed upload is still
    // running at the end of the test, and ~ImageManagement completes it.
    Outcome resumed;
    Fixture fixture;

    const UploadHandle handle = fixture.start();
    fixture.respond(40);
    fixture.transport.disconnect();

    fixture.client.rebind_transport(reconnected);

    fixture.management.resume(handle, resumed.on_done());

    REQUIRE(reconnected.send_count() == 1);
    const auto payload = std::vector<std::byte>(
        reconnected.last_sent().begin() + smply::kHeaderSize, reconnected.last_sent().end());
    REQUIRE(payload.front() == std::byte{0xA5}); // a full first packet again

    // The device reports 60, further than we had, and the upload carries on.
    const auto request = smply::decode_header(reconnected.last_sent());
    REQUIRE(request.has_value());
    auto response = bytes_of({0xA1, 0x63, 'o', 'f', 'f', 0x18, 0x3C});
    const Header reply{.op = smply::response_to(request->op),
                       .version = Version::V1,
                       .flags = 0,
                       .length = 0,
                       .group = request->group,
                       .seq = request->seq,
                       .command = request->command};
    const auto message = make_message(reply, ConstBytes{response});
    reconnected.deliver(ConstBytes{message});

    REQUIRE(fixture.management.transferred(handle) == 60);
    REQUIRE(reconnected.send_count() == 2);
    REQUIRE(resumed.calls == 0);
}

TEST_CASE("resuming an upload that never disconnected is refused", "[upload][driver][resume]")
{
    Fixture fixture;

    const UploadHandle handle = fixture.start();

    Outcome resumed;
    fixture.management.resume(handle, resumed.on_done());
    fixture.client.poll(fixture.clock.now());

    REQUIRE(resumed.calls == 1);
    REQUIRE(resumed.code == ErrorCode::InvalidState);
}

TEST_CASE("resuming a stale handle is refused", "[upload][driver][resume]")
{
    Fixture fixture;

    Outcome resumed;
    fixture.management.resume(UploadHandle{}, resumed.on_done());
    fixture.client.poll(fixture.clock.now());

    REQUIRE(resumed.calls == 1);
    REQUIRE(resumed.code == ErrorCode::InvalidState);
}

// ---------------------------------------------------------------------------
// Argument validation
// ---------------------------------------------------------------------------

TEST_CASE("an empty source is refused before anything is sent", "[upload][driver]")
{
    // The server rejects a first chunk that does not carry the 32-byte MCUboot
    // header, so an empty file could never succeed.
    Fixture fixture;

    MemoryImageSource empty{ConstBytes{}};
    Outcome outcome;
    const UploadHandle handle = fixture.management.upload(empty, Fixture::options(),
                                                          outcome.on_progress(), outcome.on_done());

    REQUIRE_FALSE(handle.valid());
    REQUIRE(fixture.transport.send_count() == 0);
    REQUIRE(outcome.calls == 0);
    fixture.client.poll(fixture.clock.now());
    REQUIRE(outcome.code == ErrorCode::InvalidArgument);
}

TEST_CASE("a chunk size outside the bounds is refused", "[upload][driver]")
{
    Fixture fixture;

    for (const std::uint32_t size :
         {smply::limits::kUploadChunkMin - 1, smply::limits::kUploadChunkMax + 1}) {
        UploadOptions options = Fixture::options();
        options.chunk_size = size;
        Outcome outcome;

        const UploadHandle handle = fixture.management.upload(
            fixture.source, options, outcome.on_progress(), outcome.on_done());

        INFO("chunk size " << size);
        REQUIRE_FALSE(handle.valid());
        fixture.client.poll(fixture.clock.now());
        REQUIRE(outcome.code == ErrorCode::InvalidArgument);
    }
}

TEST_CASE("a source that reads short is refused, not retried", "[upload][driver][hostile]")
{
    Outcome outcome;
    Fixture fixture;

    ShortReadingImageSource source{kImageSize};
    const UploadOptions options = Fixture::options();
    const UploadHandle handle =
        fixture.management.upload(source, options, outcome.on_progress(), outcome.on_done());

    REQUIRE_FALSE(handle.valid());
    REQUIRE(fixture.transport.send_count() == 0);
    // Deferred, not inline: a callback never runs inside the call that started
    // the operation, and a failure on the first chunk is no exception.
    REQUIRE(outcome.calls == 0);
    fixture.client.poll(fixture.clock.now());
    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::InvalidArgument);
}

TEST_CASE("a source that fails a read fails the upload", "[upload][driver][hostile]")
{
    Outcome outcome;
    Fixture fixture;

    FailingImageSource source{kImageSize};
    static_cast<void>(fixture.management.upload(source, Fixture::options(), outcome.on_progress(),
                                                outcome.on_done()));

    REQUIRE(outcome.calls == 0);
    fixture.client.poll(fixture.clock.now());
    REQUIRE(outcome.calls == 1);
    REQUIRE(outcome.code == ErrorCode::TransportError);
}

TEST_CASE("an absent sha is computed from the source", "[upload][driver]")
{
    // Worth the extra pass: without it the device cannot resume, cannot skip an
    // image it already holds, and cannot verify what it flashed.
    Fixture fixture;

    UploadOptions options = Fixture::options();
    options.sha.reset();
    static_cast<void>(fixture.management.upload(
        fixture.source, options, fixture.outcome.on_progress(), fixture.outcome.on_done()));

    const auto payload = fixture.payload_of(0);
    const auto sha_key = bytes_of({0x63, 's', 'h', 'a', 0x58, 0x20});
    const auto at = std::search(payload.begin(), payload.end(), sha_key.begin(), sha_key.end());
    REQUIRE(at != payload.end());
    // Not the all-zero placeholder the other tests pass in.
    const auto first_digest_byte = at + static_cast<std::ptrdiff_t>(sha_key.size());
    REQUIRE(std::any_of(first_digest_byte, first_digest_byte + 32,
                        [](std::byte value) { return value != std::byte{0}; }));
}

TEST_CASE("a device reporting a zero buffer falls back to the default", "[upload][driver][sizing]")
{
    // Zero is not a budget. Treating it as one would compute a negative
    // allowance and refuse an upload that is perfectly possible.
    Fixture fixture;

    UploadOptions options = Fixture::options();
    options.chunk_size = 0;
    options.server_buf_size = 0;
    static_cast<void>(fixture.management.upload(
        fixture.source, options, fixture.outcome.on_progress(), fixture.outcome.on_done()));

    REQUIRE(fixture.transport.send_count() == 1);
    REQUIRE(fixture.transport.sent().at(0).size() <= smply::limits::kDefaultSmpMessageBudget);
}

TEST_CASE("the negotiated chunk fits the device's buffer", "[upload][driver][sizing]")
{
    // The device's buf_size is the authoritative input, and it is the caller's
    // to supply: it belongs to the OS group (A8).
    Fixture fixture;

    UploadOptions options = Fixture::options();
    options.chunk_size = 0;
    options.server_buf_size = 256;
    static_cast<void>(fixture.management.upload(
        fixture.source, options, fixture.outcome.on_progress(), fixture.outcome.on_done()));

    REQUIRE(fixture.transport.send_count() == 1);
    REQUIRE(fixture.transport.sent().at(0).size() <= 256);
}
