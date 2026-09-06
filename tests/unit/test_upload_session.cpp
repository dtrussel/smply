// SPDX-License-Identifier: Apache-2.0
//
// The response table from docs/design.md section 6, row by row. No client, no
// transport, no clock -- which is the entire point of ADR-0008: every rule in
// the protocol's most intricate operation is a value-in, value-out assertion.
//
// The rule the suite exists to protect: the server's `off` is authoritative.
// It may come back larger than what was sent, smaller, or zero, and nothing
// here may compute an offset arithmetically.

#include "groups/image/upload_session.hpp"

#include "smply/limits.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_tostring.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

using smply::ConstBytes;
using smply::Error;
using smply::ErrorCode;
using smply::Group;
using smply::Hash;
using smply::MgmtError;
using smply::SmpError;
using smply::image::Action;
using smply::image::ChunkBudget;
using smply::image::compute_chunk_size;
using smply::image::first_packet_overhead;
using smply::image::FirstPacketFields;
using smply::image::on_response;
using smply::image::Phase;
using smply::image::plan_next;
using smply::image::record_sent;
using smply::image::Step;
using smply::image::UploadConfig;
using smply::image::UploadRequest;
using smply::image::UploadResponse;
using smply::image::UploadState;

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

constexpr std::uint64_t kImageSize = 1000;
constexpr std::uint32_t kChunk = 100;

/// A config with round numbers, so an offset in a failure message is readable.
UploadConfig config()
{
    UploadConfig out;
    out.image_size = kImageSize;
    out.chunk_size = kChunk;
    out.sha = Hash{};
    return out;
}

/// State as it is just after a first packet was sent and acknowledged at
/// \p confirmed -- the ordinary mid-upload situation.
UploadState mid_upload(std::uint64_t confirmed)
{
    UploadState state;
    state.confirmed_off = confirmed;
    state.first_packet_pending = false;
    state.phase = Phase::Sending;
    state.in_flight_off = confirmed;
    state.in_flight_len = kChunk;
    state.in_flight_first_packet = false;
    return state;
}

UploadResponse offset(std::uint64_t value)
{
    UploadResponse out;
    out.off = value;
    return out;
}

UploadResponse failure(Error error)
{
    UploadResponse out;
    out.failure = std::move(error);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// plan_next
// ---------------------------------------------------------------------------

TEST_CASE("the first request is a full first packet at offset zero", "[upload][session]")
{
    // A first packet only does its job at offset zero: that is where the server
    // runs its resume and already-present checks.
    const UploadState state;
    const Step step = plan_next(state, config());

    REQUIRE(step.action == Action::SendChunk);
    REQUIRE(step.request.off == 0);
    REQUIRE(step.request.length == kChunk);
    REQUIRE(step.request.first_packet);
}

TEST_CASE("a mid-upload request carries only off and data", "[upload][session]")
{
    const Step step = plan_next(mid_upload(300), config());

    REQUIRE(step.action == Action::SendChunk);
    REQUIRE(step.request.off == 300);
    REQUIRE(step.request.length == kChunk);
    REQUIRE_FALSE(step.request.first_packet);
}

TEST_CASE("the final chunk is clipped to what is left", "[upload][session]")
{
    UploadConfig cfg = config();
    cfg.image_size = 250;

    const Step step = plan_next(mid_upload(200), cfg);

    REQUIRE(step.request.length == 50);
}

TEST_CASE("an image smaller than one chunk is a single request", "[upload][session]")
{
    UploadConfig cfg = config();
    cfg.image_size = 40;

    const Step step = plan_next(UploadState{}, cfg);

    REQUIRE(step.request.off == 0);
    REQUIRE(step.request.length == 40);
    REQUIRE(step.request.first_packet);
}

TEST_CASE("a session already at the image size is complete", "[upload][session]")
{
    // Reachable after a resume whose first packet adopted an offset covering
    // the whole image.
    const UploadState state = mid_upload(kImageSize);

    const Step step = plan_next(state, config());

    REQUIRE(step.action == Action::Complete);
}

TEST_CASE("a failed session refuses to plan more work", "[upload][session]")
{
    UploadState state = mid_upload(300);
    state.phase = Phase::Failed;

    const Step step = plan_next(state, config());

    REQUIRE(step.action == Action::Fail);
    REQUIRE(step.error.code() == ErrorCode::InvalidState);
}

TEST_CASE("record_sent remembers exactly what went out", "[upload][session]")
{
    UploadState state;
    const UploadRequest request{.off = 700, .length = 60, .first_packet = false};

    record_sent(state, request);

    REQUIRE(state.in_flight_off == 700);
    REQUIRE(state.in_flight_len == 60);
    REQUIRE_FALSE(state.in_flight_first_packet);
    REQUIRE(state.phase == Phase::Sending);
}

// ---------------------------------------------------------------------------
// The response table
// ---------------------------------------------------------------------------

TEST_CASE("ordinary progress advances to the server's offset", "[upload][session]")
{
    UploadState state = mid_upload(300);

    const Step step = on_response(state, offset(400), config());

    REQUIRE(step.action == Action::SendChunk);
    REQUIRE(state.confirmed_off == 400);
    REQUIRE(step.request.off == 400);
    REQUIRE(state.consecutive_no_progress == 0);
}

TEST_CASE("an offset larger than what was sent is accepted", "[upload][session]")
{
    // Legal and not an error: the server is the authority on how much it has,
    // and a client that insisted on its own arithmetic would desynchronise.
    UploadState state = mid_upload(300);

    const Step step = on_response(state, offset(650), config());

    REQUIRE(step.action == Action::SendChunk);
    REQUIRE(state.confirmed_off == 650);
    REQUIRE(step.request.off == 650);
}

TEST_CASE("a rewind is followed, not argued with", "[upload][session]")
{
    UploadState state = mid_upload(500);

    const Step step = on_response(state, offset(200), config());

    REQUIRE(step.action == Action::SendChunk);
    REQUIRE(state.confirmed_off == 200);
    REQUIRE(step.request.off == 200);
    REQUIRE(state.consecutive_no_progress == 1);
}

TEST_CASE("a server that stops advancing fails after the budget", "[upload][session]")
{
    // Following a rewind forever is a hang; the budget turns it into an error.
    UploadState state = mid_upload(300);
    const UploadConfig cfg = config();

    for (std::uint32_t i = 0; i < cfg.max_no_progress; ++i) {
        const Step step = on_response(state, offset(300), cfg);
        INFO("repetition " << i);
        REQUIRE(step.action == Action::SendChunk);
    }

    const Step step = on_response(state, offset(300), cfg);
    REQUIRE(step.action == Action::Fail);
    REQUIRE(step.error.code() == ErrorCode::UpdateFailed);
    REQUIRE(state.phase == Phase::Failed);
}

TEST_CASE("progress resets the no-progress budget", "[upload][session]")
{
    UploadState state = mid_upload(300);

    static_cast<void>(on_response(state, offset(300), config()));
    REQUIRE(state.consecutive_no_progress == 1);

    static_cast<void>(on_response(state, offset(400), config()));
    REQUIRE(state.consecutive_no_progress == 0);
}

TEST_CASE("offset zero mid-upload restarts with a full first packet", "[upload][session]")
{
    // Rule 7: the client must re-send every field the original first packet
    // carried, or the server cannot rebuild the session.
    UploadState state = mid_upload(600);

    const Step step = on_response(state, offset(0), config());

    REQUIRE(step.action == Action::SendChunk);
    REQUIRE(step.request.off == 0);
    REQUIRE(step.request.first_packet);
    REQUIRE(state.confirmed_off == 0);
    REQUIRE(state.restarts == 1);
}

TEST_CASE("restarts are bounded", "[upload][session]")
{
    UploadState state = mid_upload(600);
    const UploadConfig cfg = config();

    for (std::uint32_t i = 0; i < cfg.max_restarts; ++i) {
        state.in_flight_first_packet = false;
        const Step step = on_response(state, offset(0), cfg);
        INFO("restart " << i);
        REQUIRE(step.action == Action::SendChunk);
    }

    state.in_flight_first_packet = false;
    const Step step = on_response(state, offset(0), cfg);
    REQUIRE(step.action == Action::Fail);
    REQUIRE(step.error.code() == ErrorCode::UpdateFailed);
}

TEST_CASE("the offset the whole image is byte-complete", "[upload][session]")
{
    UploadState state = mid_upload(900);

    const Step step = on_response(state, offset(kImageSize), config());

    REQUIRE(step.action == Action::Complete);
    REQUIRE(state.confirmed_off == kImageSize);
    REQUIRE(state.phase == Phase::Done);
}

TEST_CASE("a device that already holds the image completes the first packet", "[upload][session]")
{
    // Given a full sha, the server checks the target slot before writing
    // anything: if the image is already there it answers with the size and a
    // match. The upload finishes having transferred nothing.
    UploadState state;
    const Step first = plan_next(state, config());
    record_sent(state, first.request);

    UploadResponse response = offset(kImageSize);
    response.match = true;
    const Step step = on_response(state, response, config());

    REQUIRE(step.action == Action::Complete);
    REQUIRE(step.match == true);
    REQUIRE(state.confirmed_off == kImageSize);

    // And it says *which kind* of completion this was. `off` alone cannot
    // distinguish it from a transfer that finished -- both report the whole
    // image -- so the fact is recorded here, where the answer is still known.
    REQUIRE(step.completed_on_first_packet);
}

TEST_CASE("a transfer that finishes normally is not reported as already present",
          "[upload][session]")
{
    // The other side of the flag: the last chunk of a real transfer completes
    // with the same `off == image_size`, and must not look like the device
    // already had the image.
    UploadState state = mid_upload(kImageSize - 40);
    const Step chunk = plan_next(state, config());
    REQUIRE_FALSE(chunk.request.first_packet);
    record_sent(state, chunk.request);

    UploadResponse response = offset(kImageSize);
    response.match = true;
    const Step step = on_response(state, response, config());

    REQUIRE(step.action == Action::Complete);
    CHECK_FALSE(step.completed_on_first_packet);
}

TEST_CASE("an offset beyond the image is malformed", "[upload][session][hostile]")
{
    UploadState state = mid_upload(900);

    const Step step = on_response(state, offset(kImageSize + 1), config());

    REQUIRE(step.action == Action::Fail);
    REQUIRE(step.error.code() == ErrorCode::MalformedMessage);
}

TEST_CASE("a success with no offset is malformed", "[upload][session][hostile]")
{
    // Rule 10: a successful upload response always carries `off`.
    UploadState state = mid_upload(300);

    const Step step = on_response(state, UploadResponse{}, config());

    REQUIRE(step.action == Action::Fail);
    REQUIRE(step.error.code() == ErrorCode::MalformedMessage);
}

// ---------------------------------------------------------------------------
// match
// ---------------------------------------------------------------------------

TEST_CASE("match false fails the upload", "[upload][session]")
{
    // The device hashed what it flashed and it is not the file. Nothing else
    // detects that.
    UploadState state = mid_upload(900);
    UploadResponse response = offset(kImageSize);
    response.match = false;

    const Step step = on_response(state, response, config());

    REQUIRE(step.action == Action::Fail);
    REQUIRE(step.error.code() == ErrorCode::ImageMismatch);
}

TEST_CASE("an absent match is not an error", "[upload][session]")
{
    // A6: whether `match` appears at all depends on a Kconfig option, so its
    // absence says nothing about the upload.
    UploadState state = mid_upload(900);

    const Step step = on_response(state, offset(kImageSize), config());

    REQUIRE(step.action == Action::Complete);
    REQUIRE_FALSE(step.match.has_value());
}

TEST_CASE("match true is carried to the result", "[upload][session]")
{
    UploadState state = mid_upload(900);
    UploadResponse response = offset(kImageSize);
    response.match = true;

    const Step step = on_response(state, response, config());

    REQUIRE(step.action == Action::Complete);
    REQUIRE(step.match == true);
}

// ---------------------------------------------------------------------------
// Failures that are not responses
// ---------------------------------------------------------------------------

TEST_CASE("a timeout retransmits the identical request", "[upload][session]")
{
    // Safe by construction: the offset has not moved, so either the server
    // never saw the request, or it saw it and answers with the offset it holds.
    UploadState state = mid_upload(300);
    state.in_flight_off = 300;
    state.in_flight_len = 100;

    const Step step = on_response(state, failure(Error{ErrorCode::Timeout}), config());

    REQUIRE(step.action == Action::SendChunk);
    REQUIRE(step.request.off == 300);
    REQUIRE(step.request.length == 100);
    REQUIRE_FALSE(step.request.first_packet);
    REQUIRE(state.retries == 1);
}

TEST_CASE("a retransmitted first packet stays a first packet", "[upload][session]")
{
    UploadState state;
    const Step first = plan_next(state, config());
    record_sent(state, first.request);

    const Step step = on_response(state, failure(Error{ErrorCode::Timeout}), config());

    REQUIRE(step.request == first.request);
}

TEST_CASE("retransmissions are bounded", "[upload][session]")
{
    UploadState state = mid_upload(300);
    const UploadConfig cfg = config();

    for (std::uint32_t i = 0; i < cfg.max_chunk_retries; ++i) {
        const Step step = on_response(state, failure(Error{ErrorCode::Timeout}), cfg);
        INFO("retry " << i);
        REQUIRE(step.action == Action::SendChunk);
    }

    const Step step = on_response(state, failure(Error{ErrorCode::Timeout}), cfg);
    REQUIRE(step.action == Action::Fail);
    REQUIRE(step.error.code() == ErrorCode::Timeout);
}

TEST_CASE("a busy or out-of-memory device is retried", "[upload][session]")
{
    // The two device errors that mean "ask again shortly" rather than "stop".
    for (const SmpError code : {SmpError::Busy, SmpError::NoMemory}) {
        UploadState state = mid_upload(300);
        const Error error{ErrorCode::ProtocolError,
                          MgmtError::smp(static_cast<std::uint16_t>(code))};

        const Step step = on_response(state, failure(error), config());

        INFO("smp error " << static_cast<int>(code));
        REQUIRE(step.action == Action::SendChunk);
        REQUIRE(state.retries == 1);
    }
}

TEST_CASE("any other device error stops the upload at once", "[upload][session]")
{
    UploadState state = mid_upload(300);
    const Error error{ErrorCode::ProtocolError, MgmtError::scoped(Group::Image, 31)};

    const Step step = on_response(state, failure(error), config());

    REQUIRE(step.action == Action::Fail);
    REQUIRE(step.error.code() == ErrorCode::ProtocolError);
    REQUIRE(state.retries == 0);
}

TEST_CASE("a disconnect stops the upload without consuming a retry", "[upload][session]")
{
    UploadState state = mid_upload(300);

    const Step step = on_response(state, failure(Error{ErrorCode::Disconnected}), config());

    REQUIRE(step.action == Action::Fail);
    REQUIRE(step.error.code() == ErrorCode::Disconnected);
    // The session is still good; the driver keeps it for resume().
    REQUIRE(state.confirmed_off == 300);
}

TEST_CASE("progress resets the retry budget", "[upload][session]")
{
    UploadState state = mid_upload(300);

    static_cast<void>(on_response(state, failure(Error{ErrorCode::Timeout}), config()));
    REQUIRE(state.retries == 1);

    static_cast<void>(on_response(state, offset(400), config()));
    REQUIRE(state.retries == 0);
}

// ---------------------------------------------------------------------------
// Resume
// ---------------------------------------------------------------------------

TEST_CASE("a resume adopts the device's offset without counting a stall", "[upload][session]")
{
    // The device answering a first packet with the offset it already holds is
    // a successful resume, not a server that has stopped advancing -- so it
    // must not spend the no-progress budget even when it equals what we had.
    UploadState state = mid_upload(500);
    state.first_packet_pending = true;
    const Step planned = plan_next(state, config());
    record_sent(state, planned.request);
    REQUIRE(planned.request.first_packet);
    REQUIRE(planned.request.off == 0);

    const Step step = on_response(state, offset(500), config());

    REQUIRE(step.action == Action::SendChunk);
    REQUIRE(step.request.off == 500);
    REQUIRE_FALSE(step.request.first_packet);
    REQUIRE(state.consecutive_no_progress == 0);
}

TEST_CASE("a resume onto a lower offset is still adopted", "[upload][session]")
{
    UploadState state = mid_upload(800);
    state.first_packet_pending = true;
    const Step planned = plan_next(state, config());
    record_sent(state, planned.request);

    const Step step = on_response(state, offset(200), config());

    REQUIRE(step.action == Action::SendChunk);
    REQUIRE(state.confirmed_off == 200);
    REQUIRE(state.consecutive_no_progress == 0);
}

// ---------------------------------------------------------------------------
// Chunk sizing
// ---------------------------------------------------------------------------

TEST_CASE("the first-packet overhead is the encoded envelope", "[upload][sizing]")
{
    // Hand-derived from the CBOR grammar rather than from the encoder:
    //   A5                          map(5)
    //   65 "image" 00               key + uint 0
    //   63 "len"   19 03 E8         key + uint 1000 (two-byte)
    //   63 "off"   00               key + uint 0
    //   63 "sha"   58 20 <32>       key + bstr(32)
    //   64 "data"  <header>         key + the byte-string header for the chunk
    // = 1 + 6+1 + 4+3 + 4+1 + 4+2+32 + 5 = 63, plus a 2-byte data header for
    // a 512-byte chunk.
    const FirstPacketFields fields{
        .image_size = 1000, .image = 0, .sha = Hash{}, .upgrade_only = false};

    REQUIRE(first_packet_overhead(fields) == 63 + 3);
}

TEST_CASE("the overhead grows with the fields the packet carries", "[upload][sizing]")
{
    const FirstPacketFields bare{
        .image_size = 1000, .image = 0, .sha = std::nullopt, .upgrade_only = false};
    const FirstPacketFields hashed{
        .image_size = 1000, .image = 0, .sha = Hash{}, .upgrade_only = false};
    const FirstPacketFields upgrading{
        .image_size = 1000, .image = 0, .sha = Hash{}, .upgrade_only = true};
    const FirstPacketFields large{
        .image_size = 1000000, .image = 1, .sha = Hash{}, .upgrade_only = true};

    REQUIRE(first_packet_overhead(bare) < first_packet_overhead(hashed));
    REQUIRE(first_packet_overhead(hashed) < first_packet_overhead(upgrading));
    REQUIRE(first_packet_overhead(upgrading) < first_packet_overhead(large));
}

TEST_CASE("with no opinions the budget is the conservative default", "[upload][sizing]")
{
    // A8: a device without the mcumgr-params command is normal, and 256 is the
    // safe assumption (docs/protocol-notes.md section 8).
    const FirstPacketFields fields{
        .image_size = 1000, .image = 0, .sha = Hash{}, .upgrade_only = false};
    const ChunkBudget budget;

    const auto size = compute_chunk_size(budget, fields);

    REQUIRE(size.has_value());
    REQUIRE(*size == 256 - 8 - first_packet_overhead(fields));
}

TEST_CASE("each of the three limits can win", "[upload][sizing]")
{
    const FirstPacketFields fields{
        .image_size = 1000, .image = 0, .sha = Hash{}, .upgrade_only = false};
    const std::size_t overhead = 8 + first_packet_overhead(fields);

    SECTION("the device's buffer")
    {
        ChunkBudget budget;
        budget.server_buf_size = 200;
        budget.transport_max_message_size = 4096;
        const auto size = compute_chunk_size(budget, fields);
        REQUIRE(size.has_value());
        REQUIRE(*size == 200 - overhead);
    }
    SECTION("the transport")
    {
        ChunkBudget budget;
        budget.server_buf_size = 4096;
        budget.transport_max_message_size = 180;
        const auto size = compute_chunk_size(budget, fields);
        REQUIRE(size.has_value());
        REQUIRE(*size == 180 - overhead);
    }
    SECTION("smply's own cap")
    {
        ChunkBudget budget;
        budget.server_buf_size = 4096;
        budget.transport_max_message_size = 4096;
        const auto size = compute_chunk_size(budget, fields);
        REQUIRE(size.has_value());
        REQUIRE(*size == smply::limits::kUploadChunkMax);
    }
}

TEST_CASE("a transport with no opinion does not shrink the budget", "[upload][sizing]")
{
    const FirstPacketFields fields{
        .image_size = 1000, .image = 0, .sha = Hash{}, .upgrade_only = false};
    ChunkBudget budget;
    budget.server_buf_size = 512;
    budget.transport_max_message_size = 0; // "no opinion"

    const auto size = compute_chunk_size(budget, fields);

    REQUIRE(size.has_value());
    REQUIRE(*size == 512 - 8 - first_packet_overhead(fields));
}

TEST_CASE("a budget too small for 32 bytes of payload is refused", "[upload][sizing]")
{
    // Rule 2: the server rejects a first chunk that does not carry the whole
    // 32-byte MCUboot header, so a smaller chunk could never succeed -- failing
    // beats looping.
    const FirstPacketFields fields{
        .image_size = 1000, .image = 0, .sha = Hash{}, .upgrade_only = false};
    ChunkBudget budget;
    budget.server_buf_size = 8 + static_cast<std::uint32_t>(first_packet_overhead(fields)) + 31;

    const auto size = compute_chunk_size(budget, fields);

    REQUIRE_FALSE(size.has_value());
    REQUIRE(size.error().code() == ErrorCode::MessageTooLarge);
}

TEST_CASE("a budget of exactly 32 bytes of payload is accepted", "[upload][sizing]")
{
    const FirstPacketFields fields{
        .image_size = 1000, .image = 0, .sha = Hash{}, .upgrade_only = false};
    ChunkBudget budget;
    budget.server_buf_size = 8 + static_cast<std::uint32_t>(first_packet_overhead(fields)) + 32;

    const auto size = compute_chunk_size(budget, fields);

    REQUIRE(size.has_value());
    REQUIRE(*size == smply::limits::kUploadChunkMin);
}

TEST_CASE("a budget smaller than the overhead is refused", "[upload][sizing]")
{
    const FirstPacketFields fields{
        .image_size = 1000, .image = 0, .sha = Hash{}, .upgrade_only = false};
    ChunkBudget budget;
    budget.server_buf_size = 8;

    const auto size = compute_chunk_size(budget, fields);

    REQUIRE_FALSE(size.has_value());
    REQUIRE(size.error().code() == ErrorCode::MessageTooLarge);
}

// ---------------------------------------------------------------------------
// A whole upload, driven by hand
// ---------------------------------------------------------------------------

TEST_CASE("a clean upload walks to completion", "[upload][session]")
{
    // The happy path, with the server acknowledging exactly what it was sent.
    // Also pins that no offset is ever computed here: every advance comes from
    // the response.
    const UploadConfig cfg = config();
    UploadState state;

    Step step = plan_next(state, cfg);
    std::uint64_t served = 0;
    int requests = 0;

    while (step.action == Action::SendChunk) {
        ++requests;
        REQUIRE(requests < 100);
        record_sent(state, step.request);
        served = step.request.off + step.request.length;
        step = on_response(state, offset(served), cfg);
    }

    REQUIRE(step.action == Action::Complete);
    REQUIRE(state.confirmed_off == kImageSize);
    REQUIRE(requests == 10);
}
