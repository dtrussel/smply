// SPDX-License-Identifier: Apache-2.0
//
// One case per configured bound in docs/architecture.md section 9.
//
// The point is traceability, not duplication. Several bounds are already
// exercised somewhere in the suite -- but with a literal, so moving the
// constant would leave the test passing while the bound it was written for no
// longer existed. Every case here drives the real API with a value derived
// from the **named constant**, so the test's meaning changes when the constant
// does. A limit that is documented and not enforced fails here.
//
// P13's audit found two: see "an oversized echo request" and the note on
// kMaxRetiredSeqs.

#include "cbor/cbor.hpp"
#include "smp/assembler.hpp"

#include "fake_image_source.hpp"
#include "fake_transport.hpp"
#include "image_builder.hpp"
#include "manual_clock.hpp"
#include "message_builder.hpp"
#include "test_cbor.hpp"

#include "smply/error.hpp"
#include "smply/groups/image.hpp"
#include "smply/groups/os.hpp"
#include "smply/image_source.hpp"
#include "smply/limits.hpp"
#include "smply/mcuboot_image.hpp"
#include "smply/result.hpp"
#include "smply/smp_client.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <array>
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
using smply::ImageState;
using smply::MutBytes;
using smply::Operation;
using smply::RequestSpec;
using smply::Result;
using smply::SmpClient;
using smply::SmpClientConfig;
using smply::Version;
using smply::cbor::Reader;
using smply::test::FakeTransport;
using smply::test::make_message;
using smply::test::make_raw_message;
using smply::test::ManualClock;
namespace limits = smply::limits;
namespace tcbor = smply::test::tcbor;

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

/// Client, transport and clock, in the order the library requires.
struct Fixture
{
    explicit Fixture(SmpClientConfig config = {}) : client{transport, clock, config} {}

    FakeTransport transport;
    ManualClock clock;
    SmpClient client;

    /// The header of the most recent request.
    [[nodiscard]] Header last_request() const
    {
        const Result<Header> header = smply::decode_header(ConstBytes{transport.last_sent()});
        REQUIRE(header.has_value());
        return *header;
    }

    /// Answers the outstanding request with \p payload.
    void answer(ConstBytes payload)
    {
        const Header request = last_request();
        const Header reply{.op = smply::response_to(request.op),
                           .version = request.version,
                           .flags = 0,
                           .length = 0,
                           .group = request.group,
                           .seq = request.seq,
                           .command = request.command};
        const std::vector<std::byte> message = make_message(reply, payload);
        transport.deliver(ConstBytes{message});
        client.poll(clock.now());
    }
};

/// A do-nothing sink, for feeding the assembler.
class NullSink final : public smply::MessageSink
{
public:
    void on_message(const Header& /*header*/, ConstBytes /*payload*/) override
    {
        ++messages_;
    }

    [[nodiscard]] std::size_t messages() const noexcept
    {
        return messages_;
    }

private:
    std::size_t messages_ = 0;
};

} // namespace

// --- SMP framing ------------------------------------------------------------

TEST_CASE("kMaxSmpPayload bounds a declared message length", "[limits]")
{
    // The length field is the first number a device controls, and it is checked
    // before a single byte is buffered on its behalf.
    NullSink sink;
    smply::MessageAssembler assembler;

    const Header oversized{.op = Operation::ReadResponse,
                           .version = Version::V1,
                           .flags = 0,
                           .length = static_cast<std::uint16_t>(limits::kMaxSmpPayload + 1),
                           .group = Group::Image,
                           .seq = 0,
                           .command = 0};
    const std::vector<std::byte> message = make_raw_message(oversized, ConstBytes{});

    const Result<void> fed = assembler.feed(ConstBytes{message}, sink);
    REQUIRE_FALSE(fed.has_value());
    CHECK(fed.error().code() == ErrorCode::MessageTooLarge);
    CHECK(sink.messages() == 0);
}

TEST_CASE("kMaxAssemblyBuffer bounds what a partial message may hold", "[limits]")
{
    // A device that sends a large header and then goes quiet must not be able
    // to make the client hold memory indefinitely.
    NullSink sink;
    smply::MessageAssembler assembler;

    const Header header{.op = Operation::ReadResponse,
                        .version = Version::V1,
                        .flags = 0,
                        .length = limits::kMaxSmpPayload,
                        .group = Group::Image,
                        .seq = 0,
                        .command = 0};
    const std::vector<std::byte> prefix = make_raw_message(header, ConstBytes{});
    static_cast<void>(assembler.feed(ConstBytes{prefix}, sink));

    const std::vector<std::byte> filler = smply::test::filler(4096);
    for (int i = 0; i < 8; ++i) {
        static_cast<void>(assembler.feed(ConstBytes{filler}, sink));
        CHECK(assembler.buffered() <= limits::kMaxAssemblyBuffer);
        CHECK(assembler.peak_buffered() <= limits::kMaxAssemblyBuffer);
    }
}

// --- CBOR -------------------------------------------------------------------

TEST_CASE("kMaxCborNesting bounds decoder recursion", "[limits]")
{
    // Built to exceed the default cap by one, so the constant is what the test
    // is measuring against rather than a number chosen to match it.
    tcbor::Writer document;
    for (unsigned i = 0; i <= limits::kMaxCborNesting; ++i) {
        document.map(1).text("n");
    }
    document.uint(0);

    Reader reader{document.view()};
    REQUIRE(reader.enter_map().has_value());

    // Descend until the reader stops us. The bound is on the reader's own
    // depth, so the loop is capped only to keep a regression from hanging.
    unsigned entered = 1;
    while (entered < limits::kMaxCborNesting * 4 && reader.enter_map("n").has_value()) {
        ++entered;
    }

    CHECK(entered == limits::kMaxCborNesting);
    CHECK_FALSE(reader.ok());
    CHECK(reader.status().error().code() == ErrorCode::CborDecode);
}

// --- Request lifecycle ------------------------------------------------------

TEST_CASE("kMaxInFlight bounds the pending-request table", "[limits]")
{
    // Declared before the fixture, so it outlives the client: ~SmpClient fails
    // every still-pending request, which runs this callback. Getting the order
    // wrong is a use-after-free that only ASan reports.
    std::vector<ErrorCode> refused;
    Fixture fixture;

    const RequestSpec spec{.op = Operation::Read,
                           .group = Group::Image,
                           .command = 0,
                           .payload = {},
                           .timeout = std::nullopt};

    std::size_t accepted = 0;
    for (std::size_t i = 0; i < limits::kMaxInFlight + 1; ++i) {
        const smply::RequestHandle handle =
            fixture.client.request(spec, [&refused](Result<smply::RawResponse> result) {
                if (!result.has_value()) {
                    refused.push_back(result.error().code());
                }
            });
        if (handle.valid()) {
            ++accepted;
        }
    }
    fixture.client.poll(fixture.clock.now());

    CHECK(accepted == limits::kMaxInFlight);
    CHECK(fixture.client.in_flight() == limits::kMaxInFlight);
    REQUIRE(refused.size() == 1);
    CHECK(refused.front() == ErrorCode::InvalidState);
}

TEST_CASE("kMaxRetiredSeqs bounds how long a completed sequence is remembered", "[limits]")
{
    // The sequence counter is eight bits, so it wraps. Remembering every
    // completed sequence for ever would be unbounded memory; remembering none
    // would let a late response be mis-attributed after a wrap. The bound is
    // the compromise, and this pins that it is finite and that exceeding it
    // drops responses rather than mis-delivering them (docs/security.md, T5).
    int completions = 0; // before the fixture; see kMaxInFlight
    Fixture fixture;
    const RequestSpec spec{.op = Operation::Read,
                           .group = Group::Image,
                           .command = 0,
                           .payload = {},
                           .timeout = std::nullopt};

    // Complete one request, and keep its header for a late reply.
    static_cast<void>(fixture.client.request(
        spec, [&completions](const Result<smply::RawResponse>&) { ++completions; }));
    const Header first = fixture.last_request();
    fixture.answer(ConstBytes{});
    REQUIRE(completions == 1);

    // Fill the retired set past its bound.
    for (std::size_t i = 0; i < limits::kMaxRetiredSeqs + 1; ++i) {
        static_cast<void>(fixture.client.request(
            spec, [&completions](const Result<smply::RawResponse>&) { ++completions; }));
        fixture.answer(ConstBytes{});
    }

    // The first sequence has now been forgotten, so a late copy of its response
    // is unmatched -- counted, and delivered to nobody.
    const std::uint64_t before = fixture.client.stats().unmatched;
    const Header reply{.op = Operation::ReadResponse,
                       .version = first.version,
                       .flags = 0,
                       .length = 0,
                       .group = first.group,
                       .seq = first.seq,
                       .command = first.command};
    const std::vector<std::byte> late = make_message(reply, ConstBytes{});
    const int completed_before = completions;
    fixture.transport.deliver(ConstBytes{late});
    fixture.client.poll(fixture.clock.now());

    CHECK(completions == completed_before);
    CHECK(fixture.client.stats().unmatched == before + 1);
}

TEST_CASE("kDefaultTimeout is the deadline a request gets when it names none", "[limits]")
{
    std::optional<ErrorCode> outcome; // before the fixture; see kMaxInFlight
    Fixture fixture;
    const RequestSpec spec{.op = Operation::Read,
                           .group = Group::Image,
                           .command = 0,
                           .payload = {},
                           .timeout = std::nullopt};
    static_cast<void>(fixture.client.request(spec, [&outcome](Result<smply::RawResponse> result) {
        if (!result.has_value()) {
            outcome = result.error().code();
        }
    }));

    // One tick short of the limit: still waiting.
    fixture.clock.advance(limits::kDefaultTimeout - std::chrono::milliseconds{1});
    fixture.client.poll(fixture.clock.now());
    CHECK(outcome == std::nullopt);

    fixture.clock.advance(std::chrono::milliseconds{1});
    fixture.client.poll(fixture.clock.now());
    CHECK(outcome == ErrorCode::Timeout);
}

// --- Group responses --------------------------------------------------------

namespace {

/// An image-state payload with \p count slot entries.
[[nodiscard]] std::vector<std::byte> image_state_with(std::size_t count)
{
    tcbor::Writer out;
    out.map(1).text("images").array(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.map(2).text("slot").uint(i % 2).text("version").text("1.0.0");
    }
    return out.bytes();
}

} // namespace

TEST_CASE("kMaxImages bounds the slot table a device can report", "[limits]")
{
    std::optional<ErrorCode> failure; // before the fixture; see kMaxInFlight
    std::optional<ImageState> state;
    Fixture fixture;
    smply::ImageManagement image{fixture.client};

    static_cast<void>(image.get_state([&](Result<ImageState> result) {
        if (result.has_value()) {
            state = *result;
        } else {
            failure = result.error().code();
        }
    }));

    const std::vector<std::byte> payload = image_state_with(limits::kMaxImages + 1);
    fixture.answer(ConstBytes{payload});

    CHECK_FALSE(state.has_value());
    CHECK(failure == ErrorCode::CborDecode);
}

TEST_CASE("an image list exactly at kMaxImages is accepted", "[limits]")
{
    // The boundary in the other direction. P8 found a real off-by-one here: the
    // cap was tested before entering an element, so a list of exactly the
    // maximum was rejected.
    std::optional<ImageState> state; // before the fixture; see kMaxInFlight
    Fixture fixture;
    smply::ImageManagement image{fixture.client};

    static_cast<void>(image.get_state([&state](Result<ImageState> result) {
        if (result.has_value()) {
            state = *result;
        }
    }));

    const std::vector<std::byte> payload = image_state_with(limits::kMaxImages);
    fixture.answer(ConstBytes{payload});

    REQUIRE(state.has_value());
    CHECK(state->slots.size() == limits::kMaxImages);
}

TEST_CASE("kMaxVersionStringLength bounds a device-reported version", "[limits]")
{
    std::optional<ErrorCode> failure; // before the fixture; see kMaxInFlight
    Fixture fixture;
    smply::ImageManagement image{fixture.client};

    static_cast<void>(image.get_state([&failure](Result<ImageState> result) {
        if (!result.has_value()) {
            failure = result.error().code();
        }
    }));

    tcbor::Writer out;
    out.map(1).text("images").array(1);
    out.map(2).text("slot").uint(0).text("version").text(
        std::string(limits::kMaxVersionStringLength + 1, 'v'));
    fixture.answer(out.view());

    CHECK(failure == ErrorCode::CborDecode);
}

TEST_CASE("kMaxImageHashLength bounds a device-reported hash", "[limits]")
{
    std::optional<ErrorCode> failure; // before the fixture; see kMaxInFlight
    Fixture fixture;
    smply::ImageManagement image{fixture.client};

    static_cast<void>(image.get_state([&failure](Result<ImageState> result) {
        if (!result.has_value()) {
            failure = result.error().code();
        }
    }));

    const std::vector<std::byte> oversized(limits::kMaxImageHashLength + 1, std::byte{0xAB});
    tcbor::Writer out;
    out.map(1).text("images").array(1);
    out.map(3).text("slot").uint(0).text("version").text("1.0.0").text("hash").blob(
        ConstBytes{oversized});
    fixture.answer(out.view());

    CHECK(failure == ErrorCode::CborDecode);
}

TEST_CASE("kMaxReasonLength bounds a device-supplied rsn string", "[limits]")
{
    // Attacker-controlled text that ends up in a log (docs/security.md, T12).
    std::optional<smply::Error> failure; // before the fixture; see kMaxInFlight
    Fixture fixture;
    smply::ImageManagement image{fixture.client};

    static_cast<void>(image.get_state([&failure](Result<ImageState> result) {
        if (!result.has_value()) {
            failure = result.error();
        }
    }));

    tcbor::Writer out;
    out.map(2).text("rc").uint(3).text("rsn").text(std::string(limits::kMaxReasonLength * 4, 'r'));
    fixture.answer(out.view());

    REQUIRE(failure.has_value());
    CHECK(failure->reason().size() <= limits::kMaxReasonLength);
}

TEST_CASE("kMaxEchoLength bounds echo in both directions", "[limits]")
{
    // Outbound: rejected as caller misuse rather than truncated. Declared
    // before the fixture; see kMaxInFlight.
    std::optional<ErrorCode> refused;
    Fixture fixture;
    smply::OsManagement os{fixture.client};

    static_cast<void>(os.echo(std::string(limits::kMaxEchoLength + 1, 'e'),
                              [&refused](Result<std::string> result) {
                                  if (!result.has_value()) {
                                      refused = result.error().code();
                                  }
                              }));
    fixture.client.poll(fixture.clock.now());
    CHECK(refused == ErrorCode::InvalidArgument);
    CHECK(fixture.transport.sent().empty());

    // Inbound: a device echoing back more than it was sent is not answering the
    // question that was asked.
    std::optional<ErrorCode> rejected;
    static_cast<void>(os.echo("hello", [&rejected](Result<std::string> result) {
        if (!result.has_value()) {
            rejected = result.error().code();
        }
    }));

    tcbor::Writer out;
    out.map(1).text("r").text(std::string(limits::kMaxEchoLength + 1, 'e'));
    fixture.answer(out.view());
    CHECK(rejected == ErrorCode::CborDecode);
}

// --- Image files ------------------------------------------------------------

TEST_CASE("kMaxImageSize bounds both numbers that can claim a size", "[limits]")
{
    // Two enforcement sites, because the size arrives two ways.
    //
    // From the *file*: the MCUboot header's own header_size + image_size, which
    // every later offset is derived from.
    smply::test::ImageBuilder builder;
    builder.version(1, 0, 0, 0).body(64);
    std::vector<std::byte> image = builder.build();
    // Overwrite ih_img_size (little-endian, offset 12) with something absurd.
    for (std::size_t i = 0; i < 4; ++i) {
        image[12 + i] = std::byte{0xFF};
    }
    const Result<smply::McubootImageInfo> info =
        smply::parse_mcuboot_header(ConstBytes{image}.first(smply::kMcubootHeaderSize));
    REQUIRE_FALSE(info.has_value());
    CHECK(info.error().code() == ErrorCode::InvalidArgument);

    // From the *source*: an upload refuses a file larger than the bound before
    // a byte goes on the wire.
    FakeTransport transport;
    const ManualClock clock;
    SmpClient client{transport, clock};
    smply::ImageManagement management{client};
    smply::test::FailingImageSource enormous{limits::kMaxImageSize + 1};

    std::optional<ErrorCode> refused;
    smply::UploadOptions options;
    options.sha = smply::Hash{};
    static_cast<void>(management.upload(
        enormous, options, [](const smply::UploadProgress&) {},
        [&refused](Result<smply::UploadResult> result) {
            if (!result.has_value()) {
                refused = result.error().code();
            }
        }));
    client.poll(clock.now());
    CHECK(refused == ErrorCode::InvalidArgument);
    CHECK(transport.sent().empty());

    // `sha256()` deliberately does **not** apply the bound: its source is
    // supplied by the application, not by a device or a file, and it streams in
    // fixed-size chunks so a large one costs time rather than memory. The bound
    // is defensive against untrusted numbers, and there is no untrusted number
    // here. P13's audit checked this rather than assuming it.
}

TEST_CASE("kMaxImageTlvs bounds a trailer scan", "[limits]")
{
    // Every advance is at least a four-byte entry header, so the scan cannot
    // spin; the cap bounds the *work*, not the termination.
    smply::test::ImageBuilder builder;
    builder.version(1, 0, 0, 0).body(16);
    for (std::size_t i = 0; i < limits::kMaxImageTlvs + 1; ++i) {
        builder.tlv(0x20, std::vector<std::byte>(2));
    }
    const std::vector<std::byte> image = builder.build();
    smply::MemoryImageSource source{ConstBytes{image}};

    const Result<smply::McubootImageInfo> info =
        smply::parse_mcuboot_header(ConstBytes{image}.first(smply::kMcubootHeaderSize));
    REQUIRE(info.has_value());

    const Result<std::optional<smply::ImageHash>> found = smply::find_image_tlv_hash(source, *info);
    REQUIRE_FALSE(found.has_value());
    CHECK(found.error().code() == ErrorCode::MalformedMessage);
}

// --- Upload -----------------------------------------------------------------

namespace {

[[nodiscard]] std::vector<std::byte> small_image()
{
    smply::test::ImageBuilder builder;
    builder.version(1, 0, 0, 0).body(64).tlv(0x10, std::vector<std::byte>(32));
    return builder.build();
}

/// Starts an upload with \p chunk_size and returns why it was refused, if it was.
[[nodiscard]] std::optional<ErrorCode> upload_with(std::uint32_t chunk_size)
{
    const std::vector<std::byte> image = small_image();
    std::optional<ErrorCode> refused;
    {
        FakeTransport transport;
        const ManualClock clock;
        SmpClient client{transport, clock};
        smply::ImageManagement management{client};
        smply::MemoryImageSource source{ConstBytes{image}};

        smply::UploadOptions options;
        options.chunk_size = chunk_size;
        options.sha = smply::Hash{};
        static_cast<void>(management.upload(
            source, options, [](const smply::UploadProgress&) {},
            [&refused](Result<smply::UploadResult> result) {
                if (!result.has_value()) {
                    refused = result.error().code();
                }
            }));
        client.poll(clock.now());
    }
    return refused;
}

} // namespace

TEST_CASE("kUploadChunkMin and kUploadChunkMax bound an explicit chunk size", "[limits]")
{
    // Below the minimum the MCUboot header cannot fit in the first chunk, which
    // the server rejects outright (protocol-notes section 6, rule 2); above the
    // maximum the request would not fit a conservative SMP buffer.
    CHECK(upload_with(limits::kUploadChunkMin - 1) == ErrorCode::InvalidArgument);
    CHECK(upload_with(limits::kUploadChunkMax + 1) == ErrorCode::InvalidArgument);

    // The boundary values are not rejected *as arguments*. Either may still
    // fail on the message budget -- the minimum chunk plus a first-packet
    // envelope does not fit the conservative 256-byte default -- and that is a
    // different refusal with a different meaning, which is the distinction this
    // check exists to keep.
    CHECK(upload_with(limits::kUploadChunkMin) != ErrorCode::InvalidArgument);
    CHECK(upload_with(limits::kUploadChunkMax) != ErrorCode::InvalidArgument);
}

TEST_CASE("kDefaultSmpMessageBudget is what an unreported buf_size falls back to", "[limits]")
{
    // A device that does not implement the parameters command is ordinary, not
    // broken (protocol-notes section 9, A8), so the budget must have a safe
    // default rather than the client guessing large.
    const std::vector<std::byte> image = small_image();
    FakeTransport transport;
    const ManualClock clock;
    SmpClient client{transport, clock};
    smply::ImageManagement management{client};
    smply::MemoryImageSource source{ConstBytes{image}};

    smply::UploadOptions options;
    options.sha = smply::Hash{};
    static_cast<void>(management.upload(
        source, options, [](const smply::UploadProgress&) {},
        [](const Result<smply::UploadResult>&) {}));
    client.poll(clock.now());

    REQUIRE_FALSE(transport.sent().empty());
    CHECK(transport.last_sent().size() <= limits::kDefaultSmpMessageBudget);
}

TEST_CASE("kFirstChunkTimeout and kEraseTimeout are longer than the default", "[limits]")
{
    // Both commands can trigger a flash erase of unbounded duration
    // (protocol-notes section 9, A7 and A12). A limit that was *shorter* than
    // the default would silently make those commands fail on real hardware.
    STATIC_REQUIRE(limits::kFirstChunkTimeout > limits::kDefaultTimeout);
    STATIC_REQUIRE(limits::kEraseTimeout > limits::kDefaultTimeout);
}

TEST_CASE("the upload budgets are finite", "[limits]")
{
    // Retries, restarts and no-progress responses are all things a device can
    // cause indefinitely. Each budget being non-zero and finite is what turns
    // "the device is misbehaving" into a terminated upload rather than a hang;
    // the behaviour of each is table-tested in test_upload_session.cpp.
    STATIC_REQUIRE(limits::kMaxChunkRetries > 0);
    STATIC_REQUIRE(limits::kMaxUploadRestarts > 0);
    STATIC_REQUIRE(limits::kMaxNoProgress > 0);
    CHECK(limits::kMaxChunkRetries < 100);
    CHECK(limits::kMaxUploadRestarts < 100);
    CHECK(limits::kMaxNoProgress < 100);
}
