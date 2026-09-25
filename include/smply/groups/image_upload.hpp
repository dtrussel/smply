// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_GROUPS_IMAGE_UPLOAD_HPP
#define SMPLY_GROUPS_IMAGE_UPLOAD_HPP

/// \file
/// The values an image upload takes and returns: its options, its progress,
/// its result and its handle (docs/design.md section 6).
///
/// Separate from `smply/groups/image.hpp`, which includes this header and
/// declares `ImageManagement::upload()` itself, so that code which only builds
/// options or reads results -- a plan, a report, a user interface -- does not
/// need the SMP client.

#include "smply/bytes.hpp"
#include "smply/clock.hpp"
#include "smply/limits.hpp"

#include <cstdint>
#include <functional>
#include <optional>

namespace smply {

/// What to ask for when uploading an image.
struct UploadOptions
{
    /// Which image to write: 0 is the running application, and a multi-image
    /// device numbers the rest from 1. Sent on first packets only, where the
    /// device uses it to choose the slot (docs/protocol-notes.md section 6).
    std::uint32_t image = 0;

    /// Ask the server to refuse a version that is not newer than the running
    /// one.
    ///
    /// Off by default: whether the comparison includes the build number is a
    /// Kconfig option, so the same image can be accepted by one device and
    /// refused by another (docs/protocol-notes.md section 9, A11).
    bool upgrade_only = false;

    /// SHA-256 of the whole file, as `sha256(ImageSource&)` computes it.
    ///
    /// Computed from the source when absent, which costs one extra pass. It is
    /// worth sending: the full 32-byte value is what lets the device resume an
    /// interrupted upload, skip an image it already holds, and verify what it
    /// flashed. Omitting it disables all three.
    std::optional<Hash> sha;

    /// Payload bytes per chunk. Zero negotiates one from the budget below.
    ///
    /// An explicit value above `limits::kUploadChunkMax` or below
    /// `limits::kUploadChunkMin` is rejected with `ErrorCode::InvalidArgument`.
    std::uint32_t chunk_size = 0;

    /// The device's SMP buffer size, from `OsManagement::mcumgr_parameters()`.
    ///
    /// The caller fetches it, because it belongs to the OS group and because
    /// `SmpError::NotSupported` from that command is a normal answer to fall
    /// back from, not an upload failure (A8). Absent means
    /// `limits::kDefaultSmpMessageBudget`.
    std::optional<std::uint32_t> server_buf_size;

    std::uint32_t max_chunk_retries = limits::kMaxChunkRetries;
    std::uint32_t max_restarts = limits::kMaxUploadRestarts;
    std::uint32_t max_no_progress = limits::kMaxNoProgress;

    /// Deadline for the first chunk, which may trigger an implicit slot erase
    /// of unbounded duration (A7).
    Duration first_chunk_timeout = limits::kFirstChunkTimeout;
    /// Deadline for the final chunk, which a device with the image check
    /// enabled answers only after hashing the whole image out of flash (A19).
    /// Proportional to the image size; see `limits::kFinalChunkTimeout`.
    Duration final_chunk_timeout = limits::kFinalChunkTimeout;
    /// Deadline for every other chunk.
    Duration chunk_timeout = limits::kDefaultTimeout;
};

/// How far an upload has got.
///
/// `transferred` is the offset the **device** has acknowledged, never what was
/// put on the wire, so it cannot overstate what was stored.
struct UploadProgress
{
    std::uint64_t transferred = 0;
    std::uint64_t total = 0;
};

/// What an upload achieved.
struct UploadResult
{
    std::uint64_t transferred = 0;

    /// The device already held this image: it answered a **first** packet with
    /// the image complete **before this session had transferred anything**
    /// (docs/protocol-notes.md section 6, rule 9a).
    ///
    /// The server runs that check itself, on any request at offset zero
    /// carrying a full `sha`, and it is the reason an upload can finish in one
    /// round trip. Without this flag a caller cannot tell that from a transfer
    /// that happened to be one chunk long -- `transferred` reads as the whole
    /// image either way, because the device acknowledged the whole image.
    ///
    /// The second clause matters on a real link. When the response to the
    /// final chunk is lost or late, the retransmitted chunk is answered
    /// `off == 0` (rule 9b) and the session restarts with a first packet, which
    /// the server then completes by that same check -- because it now holds
    /// the image *this session sent*. That is a transfer, not a skip, and it
    /// happens on every update against a device whose final-chunk work exceeds
    /// the deadline (protocol-notes A19). A session that made progress therefore never reports it,
    /// whichever packet finished it.
    bool already_present = false;
    /// The device's own verdict on the flashed bytes, when it has one.
    ///
    /// Absent on a device built without the image check, which is not a failure
    /// (docs/protocol-notes.md section 9, A6) -- a `false` never reaches here,
    /// because it fails the upload with `ErrorCode::ImageMismatch`.
    std::optional<bool> match;
};

/// Identifies one upload.
///
/// Generation-tagged, like `RequestHandle`: once an upload finishes the handle
/// is inert forever, so acting through a stale one is a no-op rather than an
/// attack on whatever upload started since.
///
/// It carries no pointer to its `ImageManagement`, which is why the operations
/// on it are methods there rather than here -- a handle that outlived its group
/// would otherwise be a dangling pointer instead of an inert value.
class UploadHandle
{
public:
    UploadHandle() = default;

    /// False for a default-constructed handle, and for one whose `upload()`
    /// could not start.
    [[nodiscard]] bool valid() const noexcept
    {
        return generation_ != 0;
    }

    explicit operator bool() const noexcept
    {
        return valid();
    }

    [[nodiscard]] friend constexpr bool operator==(const UploadHandle&,
                                                   const UploadHandle&) noexcept = default;

private:
    friend class ImageManagement;

    explicit UploadHandle(std::uint64_t generation) noexcept : generation_{generation} {}

    /// Zero means "never referred to an upload".
    std::uint64_t generation_ = 0;
};

/// How an upload reports progress: on every advance the device confirms, on
/// the client context.
using ProgressCallback = std::function<void(UploadProgress)>;

} // namespace smply

#endif // SMPLY_GROUPS_IMAGE_UPLOAD_HPP
