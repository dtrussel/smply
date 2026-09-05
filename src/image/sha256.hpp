// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SRC_IMAGE_SHA256_HPP
#define SMPLY_SRC_IMAGE_SHA256_HPP

/// \file
/// SHA-256 (FIPS 180-4), written for smply rather than taken from a crypto
/// library (ADR-0009).
///
/// The protocol needs exactly one hash function, over data smply is already
/// streaming, and a dependency on OpenSSL or BCrypt would cost far more in
/// portability and build surface than 150 lines cost in maintenance. Its
/// correctness is pinned by the NIST vectors in tests/unit/test_sha256.cpp.
///
/// Internal to smply: `smply::sha256(ImageSource&)` is the public surface.
/// Incremental so it can hash a stream without buffering it, and so a test can
/// feed it at every block boundary.

#include "smply/bytes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace smply::image {

/// The digest length, in bytes.
inline constexpr std::size_t kSha256DigestSize = 32;

/// The block size the compression function consumes, in bytes.
inline constexpr std::size_t kSha256BlockSize = 64;

/// An incremental SHA-256.
///
/// Absorbs any number of chunks of any size and produces the same digest as one
/// call over the concatenation. `finish()` consumes the state: a Sha256 is used
/// once.
class Sha256
{
public:
    Sha256() = default;

    /// Absorbs \p data. May be called any number of times, including with an
    /// empty span.
    void update(ConstBytes data) noexcept;

    /// Pads, appends the length, and returns the digest.
    [[nodiscard]] std::array<std::byte, kSha256DigestSize> finish() noexcept;

private:
    void compress(const std::byte* block) noexcept;

    /// H0..H7 from FIPS 180-4 section 5.3.3.
    std::array<std::uint32_t, 8> state_{0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
                                        0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U};
    /// Bytes absorbed so far; the padded length field is this times eight.
    std::uint64_t length_ = 0;
    /// Partial block awaiting a full 64 bytes.
    std::array<std::byte, kSha256BlockSize> buffer_{};
    std::size_t buffered_ = 0;
};

} // namespace smply::image

#endif // SMPLY_SRC_IMAGE_SHA256_HPP
