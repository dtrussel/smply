// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SRC_GROUPS_IMAGE_UPLOAD_SESSION_HPP
#define SMPLY_SRC_GROUPS_IMAGE_UPLOAD_SESSION_HPP

/// \file
/// The upload decision logic, as pure functions
/// ([ADR-0008](docs/decisions/ADR-0008-upload-state-ownership.md)).
///
/// No client, no transport, no clock, no `ImageSource`: these functions say
/// *what* should happen next and the driver does it. That is what makes every
/// row of the response table in docs/design.md section 6 a table-driven unit
/// test rather than a scenario needing a fake device.
///
/// **The rule the whole file exists to enforce: the server's `off` is
/// authoritative.** It may come back larger than what was sent, smaller, or
/// zero, and every one of those is legal (docs/protocol-notes.md section 6,
/// rule 5). Nothing here computes `next_off = off + sent`; a client that does
/// silently corrupts the flashed image instead of failing.

#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/limits.hpp"
#include "smply/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace smply::image {

/// Everything about one upload that does not change while it runs.
struct UploadConfig
{
    /// Total bytes to transfer. Never zero: an empty file is rejected before a
    /// session exists.
    std::uint64_t image_size = 0;
    /// Negotiated payload per chunk, at least `limits::kUploadChunkMin`.
    std::uint32_t chunk_size = 0;
    /// The image number, sent in every first packet.
    std::uint32_t image = 0;
    /// SHA-256 of the whole file. Absent disables the device's resume and
    /// `match` checks (docs/protocol-notes.md section 6, rule 1).
    std::optional<Hash> sha;
    /// Ask the server to reject a version that is not newer (A11).
    bool upgrade_only = false;

    std::uint32_t max_chunk_retries = limits::kMaxChunkRetries;
    std::uint32_t max_restarts = limits::kMaxUploadRestarts;
    std::uint32_t max_no_progress = limits::kMaxNoProgress;
};

/// Where an upload has got to.
enum class Phase : std::uint8_t
{
    Idle,    ///< Nothing sent yet.
    Sending, ///< A request is out, or the next one is ready.
    Done,    ///< The device holds the whole image.
    Failed,  ///< Terminal.
};

/// The mutable half. Everything a resume needs is in here.
struct UploadState
{
    /// The offset the **server** has acknowledged. The only source of progress,
    /// so progress can never overstate what the device stored.
    std::uint64_t confirmed_off = 0;
    /// What the outstanding request asked for, kept so a timeout can retransmit
    /// byte-identical bytes.
    std::uint64_t in_flight_off = 0;
    std::uint32_t in_flight_len = 0;
    bool in_flight_first_packet = false;

    std::uint32_t consecutive_no_progress = 0;
    std::uint32_t restarts = 0;
    std::uint32_t retries = 0;

    /// The next request must carry `len`, `sha`, `image` and `upgrade` -- set
    /// after a restart and before a resume (docs/protocol-notes.md section 6,
    /// rule 7).
    bool first_packet_pending = true;
    Phase phase = Phase::Idle;
};

/// What to put on the wire. The bytes themselves are the driver's business.
struct UploadRequest
{
    std::uint64_t off = 0;
    std::uint32_t length = 0;
    /// Include `len`, `sha`, `image` and `upgrade`. Always paired with
    /// `off == 0`: the server only runs its resume and already-present checks
    /// on a request at offset zero.
    bool first_packet = false;

    [[nodiscard]] friend constexpr bool operator==(const UploadRequest&,
                                                   const UploadRequest&) noexcept = default;
};

/// What came back, already decoded and stripped of transport detail.
struct UploadResponse
{
    /// The server's offset. Absent on a success is malformed.
    std::optional<std::uint64_t> off;
    /// Only on the final chunk, and only where the device was built with the
    /// image check (A6). Absent is not an error; `false` is.
    std::optional<bool> match;
    /// Set instead of the above when there was no usable response: a timeout, a
    /// device error, a disconnect.
    std::optional<Error> failure;
};

/// What the driver should do next.
enum class Action : std::uint8_t
{
    SendChunk, ///< Send `Step::request`.
    Complete,  ///< The upload succeeded; `Step::match` carries the device's verdict.
    Fail,      ///< Terminal; `Step::error` says why.
};

/// One decision.
///
/// A restart is deliberately **not** its own action. It is "set
/// `first_packet_pending`, zero `confirmed_off`, then send" -- expressing it as
/// a separate action would let a driver handle it and forget to send anything,
/// which is a hang rather than an error. ADR-0008's sketch listed `Restart`;
/// the behaviour here is the same.
struct Step
{
    Action action = Action::Fail;
    UploadRequest request;
    Error error;
    std::optional<bool> match;
};

/// Inputs to the exact first-packet CBOR overhead.
struct FirstPacketFields
{
    std::uint64_t image_size = 0;
    std::uint32_t image = 0;
    std::optional<Hash> sha;
    bool upgrade_only = false;
};

/// The three limits that bound a chunk, kept apart on purpose
/// (docs/protocol-notes.md section 8).
struct ChunkBudget
{
    /// The device's whole-SMP-message buffer, from the OS group's mcumgr
    /// parameters. Absent means it was never asked, or the device does not
    /// implement the command (A8), and the conservative default applies.
    std::optional<std::uint32_t> server_buf_size;
    /// `Transport::max_message_size()`. Zero means "no opinion".
    std::size_t transport_max_message_size = 0;
    /// smply's own cap.
    std::uint32_t configured_max = limits::kUploadChunkMax;
};

/// Bytes a first packet costs before any `data`.
///
/// Computed by encoding a probe map with the real values and a zero-length
/// `data` byte string, then adding the byte-string header for a full chunk --
/// never estimated. The first packet is the largest, and using its overhead for
/// every chunk wastes a handful of bytes on the others and guarantees the first
/// one fits (docs/design.md section 6).
[[nodiscard]] std::size_t first_packet_overhead(const FirstPacketFields& fields);

/// Resolves the chunk size, or explains why no workable one exists.
///
/// Fails with `ErrorCode::MessageTooLarge` when the budget cannot fit
/// `limits::kUploadChunkMin` (32) bytes of payload -- the server rejects a
/// first chunk that does not carry the whole MCUboot header
/// (docs/protocol-notes.md section 6, rule 2), so a smaller chunk could never
/// succeed and looping would be worse than failing.
[[nodiscard]] Result<std::uint32_t> compute_chunk_size(const ChunkBudget& budget,
                                                       const FirstPacketFields& fields);

/// The first request of an upload, or of a resume.
[[nodiscard]] Step plan_next(const UploadState& state, const UploadConfig& config);

/// Records what the driver actually sent, so a retransmission can repeat it.
void record_sent(UploadState& state, const UploadRequest& request);

/// Folds one response into the state and says what to do next.
[[nodiscard]] Step on_response(UploadState& state, const UploadResponse& response,
                               const UploadConfig& config);

} // namespace smply::image

#endif // SMPLY_SRC_GROUPS_IMAGE_UPLOAD_SESSION_HPP
