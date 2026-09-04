// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_BYTES_HPP
#define SMPLY_BYTES_HPP

/// \file
/// Byte-buffer vocabulary types.
///
/// smply never takes ownership of a caller's buffer. Spans passed across an
/// smply API boundary -- in either direction -- are borrowed for the duration
/// of the call only; anything that outlives the call must be copied. See
/// docs/design.md section 9.

#include <array>
#include <cstddef>
#include <span>

namespace smply {

/// A borrowed, read-only view of bytes.
using ConstBytes = std::span<const std::byte>;

/// A borrowed, writable view of bytes.
using MutBytes = std::span<std::byte>;

/// A SHA-256 digest.
///
/// Two distinct SHA-256 values appear in MCUmgr and must not be confused
/// (docs/protocol-notes.md section 7): the image-upload `sha`, over the whole
/// firmware file, and the image-state `hash`, which is MCUboot's
/// IMAGE_TLV_SHA256 over the header and body only.
using Hash = std::array<std::byte, 32>;

} // namespace smply

#endif // SMPLY_BYTES_HPP
