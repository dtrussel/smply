// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_LIMITS_HPP
#define SMPLY_LIMITS_HPP

/// \file
/// Defensive bounds (docs/architecture.md section 9).
///
/// Everything received from the device is untrusted. Every length, offset,
/// array size and nesting depth taken from a response is checked against one of
/// these before it is used to size, index or allocate anything -- so a hostile
/// or broken peer can cause a bounded error, never unbounded growth
/// (docs/security.md, T1-T4).
///
/// These are defaults. `SmpClientConfig` and `UploadOptions` override them per
/// instance; they are gathered here so the whole defensive surface can be
/// reviewed in one place.

#include "smply/clock.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace smply::limits {

/// Largest SMP payload accepted from the device, excluding the 8-byte header.
/// A larger declared length is rejected before any buffering happens.
inline constexpr std::uint16_t kMaxSmpPayload = 8192;

/// Cap on bytes held while waiting for a message to complete. Bounds the cost
/// of a peer that sends a header and then stops.
inline constexpr std::size_t kMaxAssemblyBuffer = std::size_t{16} * 1024;

/// Maximum CBOR nesting depth accepted, bounding decoder recursion.
inline constexpr unsigned kMaxCborNesting = 16;

/// Requests outstanding at once. One by default: Zephyr's server processes
/// packets sequentially, and a single in-flight request is what makes upload
/// retransmission unambiguous (ADR-0010).
inline constexpr std::uint8_t kMaxInFlight = 1;

/// Recently completed sequence numbers remembered, so a late response cannot be
/// mis-attributed to a newer request that reused the 8-bit number
/// (docs/security.md, T5).
inline constexpr std::uint8_t kMaxRetiredSeqs = 64;

/// Largest number of image entries accepted in an image-state response.
inline constexpr std::size_t kMaxImages = 16;

/// Largest number of slot entries accepted per image in a slot-info response.
inline constexpr std::size_t kMaxSlotsPerImage = 8;

/// Largest accepted image-state version string, in bytes.
inline constexpr std::size_t kMaxVersionStringLength = 32;

/// Largest accepted device-supplied `rsn` string, in bytes.
inline constexpr std::size_t kMaxReasonLength = 128;

/// Default per-request deadline.
inline constexpr Duration kDefaultTimeout = std::chrono::seconds{5};

/// Deadline for the first upload chunk, which may trigger an implicit slot
/// erase of unbounded duration (docs/protocol-notes.md section 9, A7).
inline constexpr Duration kFirstChunkTimeout = std::chrono::seconds{30};

/// Deadline for the synchronous image-erase command (protocol-notes A12).
inline constexpr Duration kEraseTimeout = std::chrono::seconds{60};

/// Upper bound on an upload chunk before the device's buf_size is known.
inline constexpr std::uint32_t kUploadChunkMax = 512;

/// Smallest workable upload chunk. The server rejects a first chunk that does
/// not carry the whole 32-byte MCUboot header (protocol-notes section 6, rule 2).
inline constexpr std::uint32_t kUploadChunkMin = 32;

/// Assumed whole-SMP-message budget when the device does not implement the
/// MCUmgr parameters command (protocol-notes section 9, A8).
inline constexpr std::uint32_t kDefaultSmpMessageBudget = 256;

/// Sanity bound on a firmware image offered for upload.
inline constexpr std::uint64_t kMaxImageSize = 16ULL * 1024ULL * 1024ULL;

} // namespace smply::limits

#endif // SMPLY_LIMITS_HPP
