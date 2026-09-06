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
///
/// **Must be strictly below QCBOR's own `QCBOR_MAX_ARRAY_NESTING`, which is
/// 15**, so that this bound is the one that binds.
///
/// It was 16 until P13's limits audit, which made it the *documented* bound
/// while QCBOR's was the *effective* one -- and the difference was not
/// academic. A document nested deeper than QCBOR allows makes the reader stop
/// descending while its `status()` stays **clean**, because the QCBOR-error
/// path in `enter_map(key)` is deliberately non-sticky so it can double as a
/// probe for an optional map. A hostile document therefore turned into silently
/// missing fields rather than a decode failure, and a caller following the
/// house rule of "check `status()` at the end" would have seen nothing wrong.
///
/// Fourteen, not fifteen: equal is not enough. Reaching smply's cap needs a
/// document one level deeper than the cap, and at fifteen that document is one
/// QCBOR refuses first. Only a strictly lower value routes the refusal through
/// smply's own sticky `record()`.
///
/// MCUmgr's deepest real structure is four levels (slot info's images then
/// slots), so this is far above anything the protocol uses.
inline constexpr unsigned kMaxCborNesting = 14;

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

/// Largest accepted device-reported image hash, in bytes.
///
/// The device reports `IMAGE_SHA_LEN` bytes, which is 32 for the usual SHA-256
/// bootloader and 64 for one built with `CONFIG_MCUBOOT_BOOTLOADER_USES_SHA512`
/// (docs/protocol-notes.md section 6). The larger of the two is the bound, so
/// the value is copied into a fixed buffer rather than one sized by the device.
inline constexpr std::size_t kMaxImageHashLength = 64;

/// Largest accepted device-supplied `rsn` string, in bytes.
inline constexpr std::size_t kMaxReasonLength = 128;

/// Largest string smply will send to, or accept back from, the echo command.
///
/// Echo exists as an end-to-end smoke test, not as a bulk channel, so the
/// request encodes into a small stack buffer rather than sizing one from the
/// caller's input. A longer string is rejected as `InvalidArgument` rather than
/// silently truncated.
inline constexpr std::size_t kMaxEchoLength = 128;

/// Default per-request deadline.
inline constexpr Duration kDefaultTimeout = std::chrono::seconds{5};

/// Deadline for the first upload chunk, which may trigger an implicit slot
/// erase of unbounded duration (docs/protocol-notes.md section 9, A7).
inline constexpr Duration kFirstChunkTimeout = std::chrono::seconds{30};

/// Deadline for the synchronous image-erase command (protocol-notes A12).
inline constexpr Duration kEraseTimeout = std::chrono::seconds{60};

/// Upper bound on an upload chunk before the device's buf_size is known.
inline constexpr std::uint32_t kUploadChunkMax = 512;

/// Retransmissions of one upload chunk before the upload fails.
///
/// A retransmission is always safe: the offset is unchanged, so either the
/// server never saw the request, or it saw it and answers with the offset it
/// actually holds (docs/design.md section 6).
inline constexpr std::uint32_t kMaxChunkRetries = 3;

/// Times the server may restart an upload from offset zero before it fails.
inline constexpr std::uint32_t kMaxUploadRestarts = 2;

/// Consecutive responses that do not advance the offset before the upload
/// fails. Bounds a server that rewinds or repeats forever.
inline constexpr std::uint32_t kMaxNoProgress = 3;

/// Smallest workable upload chunk. The server rejects a first chunk that does
/// not carry the whole 32-byte MCUboot header (protocol-notes section 6, rule 2).
inline constexpr std::uint32_t kUploadChunkMin = 32;

/// Assumed whole-SMP-message budget when the device does not implement the
/// MCUmgr parameters command (protocol-notes section 9, A8).
inline constexpr std::uint32_t kDefaultSmpMessageBudget = 256;

/// Sanity bound on a firmware image offered for upload.
inline constexpr std::uint64_t kMaxImageSize = 16ULL * 1024ULL * 1024ULL;

/// Largest number of TLV entries scanned in an MCUboot image trailer.
///
/// The scan terminates without this -- every entry consumes at least its own
/// four-byte header, so the offset strictly increases -- so the cap bounds the
/// *work* a crafted file can demand rather than the loop
/// (docs/decisions/ADR-0009-mcuboot-boundary.md).
inline constexpr std::size_t kMaxImageTlvs = 256;

} // namespace smply::limits

#endif // SMPLY_LIMITS_HPP
