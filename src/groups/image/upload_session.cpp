// SPDX-License-Identifier: Apache-2.0

#include "groups/image/upload_session.hpp"

#include "cbor/cbor.hpp"
#include "smply/error.hpp"
#include "smply/limits.hpp"
#include "smply/smp/header.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace smply::image {
namespace {

/// Room for the largest first-packet envelope, so the probe encode below can
/// never be the thing that fails.
constexpr std::size_t kProbeBufferSize = 256;

/// The byte-string header CBOR needs for `length` bytes of `data`.
[[nodiscard]] constexpr std::size_t bstr_header_size(std::uint64_t length) noexcept
{
    if (length < 24) {
        return 1;
    }
    if (length <= 0xFF) {
        return 2;
    }
    if (length <= 0xFFFF) {
        return 3;
    }
    return 5;
}

/// The header a full-sized chunk needs. Reserved for **every** first packet,
/// including the ones whose chunk turns out shorter: over-reserving by a byte
/// or two is what guarantees the largest packet still fits.
constexpr std::size_t kChunkDataHeaderSize = bstr_header_size(limits::kUploadChunkMax);

[[nodiscard]] Step fail_with(UploadState& state, Error error)
{
    state.phase = Phase::Failed;
    return Step{.action = Action::Fail, .request = {}, .error = std::move(error), .match = {}};
}

/// The request that follows from the current state.
[[nodiscard]] Step send_from(const UploadState& state, const UploadConfig& config)
{
    UploadRequest request;
    // A first packet always goes at offset zero: the server runs its resume and
    // already-present checks only there (docs/protocol-notes.md section 6).
    request.first_packet = state.first_packet_pending;
    request.off = request.first_packet ? 0 : state.confirmed_off;

    const std::uint64_t remaining = config.image_size - request.off;
    request.length =
        remaining < config.chunk_size ? static_cast<std::uint32_t>(remaining) : config.chunk_size;

    return Step{.action = Action::SendChunk, .request = request, .error = {}, .match = {}};
}

/// True for the two device errors that mean "ask again shortly".
[[nodiscard]] bool is_transient(const Error& error) noexcept
{
    if (error.code() == ErrorCode::Timeout) {
        return true;
    }
    if (error.code() != ErrorCode::ProtocolError) {
        return false;
    }
    const std::optional<SmpError> smp = smp_error(error);
    return smp == SmpError::Busy || smp == SmpError::NoMemory;
}

} // namespace

std::size_t first_packet_overhead(const FirstPacketFields& fields)
{
    std::array<std::byte, kProbeBufferSize> buffer{};
    cbor::Writer writer{MutBytes{buffer}};
    writer.open_map();
    // Every field a first packet carries, with its real value, because the
    // encoded width of `len` and `off` depends on the number.
    writer.put_uint("image", fields.image);
    writer.put_uint("len", fields.image_size);
    writer.put_uint("off", 0);
    if (fields.sha.has_value()) {
        writer.put_bytes("sha", ConstBytes{*fields.sha});
    }
    if (fields.upgrade_only) {
        writer.put_bool("upgrade", true);
    }
    // A zero-length data string, so what the probe measures is the envelope.
    writer.put_bytes("data", ConstBytes{});
    const auto probe = writer.close_map().finish();
    // LCOV_EXCL_START -- unreachable guard, and the whole block is: marking
    // only the `if` leaves its body counted against the branch denominator,
    // which is what docs/quality-gates.md section 6 excludes it for.
    if (!probe.has_value()) {
        // Unreachable: kProbeBufferSize is far larger than the largest legal
        // envelope. Reported as an overhead nothing can fit rather than a
        // silent under-estimate, so chunk sizing fails loudly.
        return kProbeBufferSize;
    }
    // LCOV_EXCL_STOP

    // The probe already contains a one-byte empty-string header; the real chunk
    // needs one sized for its own length.
    return (probe->size() - 1) + kChunkDataHeaderSize;
}

Result<std::uint32_t> compute_chunk_size(const ChunkBudget& budget, const FirstPacketFields& fields)
{
    // The three limits are separate and must not be conflated
    // (docs/protocol-notes.md section 8). Each is ignored when it has no
    // opinion, so a caller that knows nothing still gets the safe default.
    std::uint64_t message_budget = limits::kDefaultSmpMessageBudget;
    if (budget.server_buf_size.has_value() && *budget.server_buf_size > 0) {
        message_budget = *budget.server_buf_size;
    }
    if (budget.transport_max_message_size > 0) {
        message_budget = std::min<std::uint64_t>(message_budget, budget.transport_max_message_size);
    }

    const std::uint64_t overhead = kHeaderSize + first_packet_overhead(fields);
    if (message_budget <= overhead) {
        return fail(Error{ErrorCode::MessageTooLarge, "upload: no room for a chunk"});
    }

    const std::uint64_t available = message_budget - overhead;
    const std::uint64_t capped = std::min<std::uint64_t>(available, budget.configured_max);
    if (capped < limits::kUploadChunkMin) {
        // The server rejects a first chunk that does not carry the whole
        // 32-byte MCUboot header, so a smaller chunk can never succeed.
        return fail(Error{ErrorCode::MessageTooLarge, "upload: chunk below the 32-byte minimum"});
    }
    return static_cast<std::uint32_t>(capped);
}

Step plan_next(const UploadState& state, const UploadConfig& config)
{
    if (state.phase == Phase::Failed) {
        return Step{.action = Action::Fail,
                    .request = {},
                    .error = Error{ErrorCode::InvalidState, "upload: session already failed"},
                    .match = {}};
    }
    if (state.confirmed_off >= config.image_size) {
        // Nothing left to send. Reachable after a resume whose first packet
        // adopted an offset that already covers the image.
        return Step{.action = Action::Complete, .request = {}, .error = {}, .match = {}};
    }
    return send_from(state, config);
}

void record_sent(UploadState& state, const UploadRequest& request)
{
    state.in_flight_off = request.off;
    state.in_flight_len = request.length;
    state.in_flight_first_packet = request.first_packet;
    state.phase = Phase::Sending;
}

Step on_response(UploadState& state, const UploadResponse& response, const UploadConfig& config)
{
    if (response.failure.has_value()) {
        if (is_transient(*response.failure) && state.retries < config.max_chunk_retries) {
            ++state.retries;
            // Byte-identical: the offset has not moved, so either the server
            // never saw the request or it will answer with the offset it holds,
            // which the table below handles.
            return Step{.action = Action::SendChunk,
                        .request = UploadRequest{.off = state.in_flight_off,
                                                 .length = state.in_flight_len,
                                                 .first_packet = state.in_flight_first_packet},
                        .error = {},
                        .match = {}};
        }
        return fail_with(state, *response.failure);
    }

    if (!response.off.has_value()) {
        // A successful upload response always carries the offset
        // (docs/protocol-notes.md section 6, rule 10).
        return fail_with(state,
                         Error{ErrorCode::MalformedMessage, "upload: response has no offset"});
    }
    const std::uint64_t reported = *response.off;

    if (reported > config.image_size) {
        return fail_with(state,
                         Error{ErrorCode::MalformedMessage, "upload: offset beyond the image"});
    }

    if (reported == config.image_size) {
        // Byte-complete. Reachable on the very first response: with a full sha
        // the server checks whether the slot already holds this image and jumps
        // straight to the end (docs/protocol-notes.md section 6).
        if (response.match == false) {
            return fail_with(state, Error{ErrorCode::ImageMismatch,
                                          "upload: device hash does not match the file"});
        }
        state.confirmed_off = reported;
        state.phase = Phase::Done;
        return Step{
            .action = Action::Complete, .request = {}, .error = {}, .match = response.match};
    }

    if (reported == 0) {
        // The server restarted the session, and says so by asking for offset
        // zero (rule 7). Also what a device that forgot the session answers,
        // and what a retransmitted final chunk gets after the session was
        // reset -- in which case the first packet below settles it in one round
        // trip, because the already-present check then reports completion.
        if (state.restarts >= config.max_restarts) {
            return fail_with(state, Error{ErrorCode::UpdateFailed, "upload: too many restarts"});
        }
        ++state.restarts;
        state.confirmed_off = 0;
        state.first_packet_pending = true;
        state.consecutive_no_progress = 0;
        state.retries = 0;
        return send_from(state, config);
    }

    if (state.in_flight_first_packet) {
        // Adopting whatever offset a first packet comes back with is the whole
        // point of sending one, so it never counts against the no-progress
        // budget -- a resume that correctly lands back on its old offset would
        // otherwise look like a stalled server.
        state.confirmed_off = reported;
        state.first_packet_pending = false;
        state.consecutive_no_progress = 0;
        state.retries = 0;
        return send_from(state, config);
    }

    if (reported > state.confirmed_off) {
        // Ordinary progress -- possibly further than was sent, which is legal.
        state.confirmed_off = reported;
        state.first_packet_pending = false;
        state.consecutive_no_progress = 0;
        state.retries = 0;
        return send_from(state, config);
    }

    // The server rewound, or repeated an offset. Legal, and the client simply
    // follows -- but bounded, or a server stuck on one offset would loop here
    // forever.
    ++state.consecutive_no_progress;
    if (state.consecutive_no_progress > config.max_no_progress) {
        return fail_with(state, Error{ErrorCode::UpdateFailed, "upload: server stopped advancing"});
    }
    state.confirmed_off = reported;
    // Never a first packet here: `reported == 0` was handled above.
    state.first_packet_pending = false;
    state.retries = 0;
    return send_from(state, config);
}

} // namespace smply::image
