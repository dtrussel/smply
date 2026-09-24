// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TRANSPORTS_SERIAL_BASE64_HPP
#define SMPLY_TRANSPORTS_SERIAL_BASE64_HPP

/// \file
/// Base64, as MCUmgr's serial framing needs it and no further.
///
/// This lives in `smply::transport::detail` deliberately. The headers under
/// `transports/` are installed surface and therefore a compatibility promise
/// ([ADR-0016](../../docs/decisions/ADR-0016-installed-package-and-versioning.md)
/// clause 4); a general-purpose base64 is not a promise this library wants to
/// make, and a consumer reaching for one should reach elsewhere. What is
/// promised is `serial_framing.hpp`, which is the only caller.
///
/// **Strict on decode, by rule 6 of CLAUDE.md**: every byte that reaches
/// `base64_decode` came off a wire a device controls. It rejects any character
/// outside the alphabet, any length that is not a multiple of four, and any
/// `=` anywhere but the final quartet -- rather than skipping, tolerating or
/// guessing, each of which turns a corrupt frame into a plausible one.
///
/// Buffers are caller-owned in both directions and nothing here allocates,
/// which is the same rule the CBOR façade follows (docs/design.md section 3).

#include "smply/bytes.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace smply::transport::detail {

/// Encoded length of \p raw bytes: `4 * ceil(raw / 3)`, padding included.
[[nodiscard]] constexpr std::size_t base64_encoded_size(std::size_t raw) noexcept
{
    return ((raw + 2) / 3) * 4;
}

/// The most bytes \p chars of base64 can decode to. Padding makes the true
/// figure up to two smaller; this is the buffer size to reserve.
[[nodiscard]] constexpr std::size_t base64_max_decoded_size(std::size_t chars) noexcept
{
    return (chars / 4) * 3;
}

namespace base64_tables {

inline constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                                    "0123456789+/";

/// Index of \p symbol in the alphabet, or -1. A linear scan over 64 entries,
/// which a serial link's data rate makes free and which cannot disagree with
/// `kAlphabet` the way a hand-written reverse table can.
[[nodiscard]] constexpr int base64_value(std::byte symbol) noexcept
{
    const char wanted = static_cast<char>(symbol);
    for (int i = 0; i < 64; ++i) {
        if (kAlphabet[i] == wanted) {
            return i;
        }
    }
    return -1;
}

} // namespace base64_tables

/// Encodes \p in into \p out.
///
/// \return the number of characters written, or `0` when \p out is too small.
///         Encoding an empty input writes nothing and also returns `0`; that
///         ambiguity is harmless here because the framing never encodes an
///         empty group.
[[nodiscard]] inline std::size_t base64_encode(ConstBytes in, MutBytes out) noexcept
{
    const std::size_t needed = base64_encoded_size(in.size());
    if (needed == 0 || out.size() < needed) {
        return 0;
    }

    std::size_t written = 0;
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const std::size_t have = in.size() - i;
        const auto b0 = static_cast<std::uint32_t>(in[i]);
        const auto b1 = have > 1 ? static_cast<std::uint32_t>(in[i + 1]) : 0U;
        const auto b2 = have > 2 ? static_cast<std::uint32_t>(in[i + 2]) : 0U;
        const std::uint32_t triple = (b0 << 16U) | (b1 << 8U) | b2;

        out[written++] = static_cast<std::byte>(base64_tables::kAlphabet[(triple >> 18U) & 0x3FU]);
        out[written++] = static_cast<std::byte>(base64_tables::kAlphabet[(triple >> 12U) & 0x3FU]);
        out[written++] =
            have > 1 ? static_cast<std::byte>(base64_tables::kAlphabet[(triple >> 6U) & 0x3FU])
                     : std::byte{'='};
        out[written++] = have > 2 ? static_cast<std::byte>(base64_tables::kAlphabet[triple & 0x3FU])
                                  : std::byte{'='};
    }
    return written;
}

/// Decodes \p in into \p out.
///
/// \return the number of bytes written, or `std::nullopt` if \p in is not
///         strictly valid base64 or \p out is too small. An empty input
///         decodes to zero bytes and succeeds -- the caller distinguishes that
///         from a failure by the optional, not by the count.
[[nodiscard]] inline std::optional<std::size_t> base64_decode(ConstBytes in, MutBytes out) noexcept
{
    if ((in.size() % 4) != 0) {
        return std::nullopt;
    }
    if (out.size() < base64_max_decoded_size(in.size())) {
        return std::nullopt;
    }

    std::size_t written = 0;
    for (std::size_t i = 0; i < in.size(); i += 4) {
        const bool final_quartet = (i + 4) == in.size();
        std::uint32_t quad = 0;
        int bytes_here = 3;

        for (std::size_t j = 0; j < 4; ++j) {
            const std::byte symbol = in[i + j];
            if (symbol == std::byte{'='}) {
                // Padding is legal only at the very end, and only as the last
                // one or two characters. `=A==` and `A===` are both rejected.
                if (!final_quartet || j < 2) {
                    return std::nullopt;
                }
                if (j == 2 && in[i + 3] != std::byte{'='}) {
                    return std::nullopt;
                }
                bytes_here = static_cast<int>(j) - 1;
                quad <<= (4 - j) * 6U;
                break;
            }
            const int value = base64_tables::base64_value(symbol);
            if (value < 0) {
                return std::nullopt;
            }
            quad = (quad << 6U) | static_cast<std::uint32_t>(value);
        }

        for (int k = 0; k < bytes_here; ++k) {
            out[written++] =
                static_cast<std::byte>((quad >> (16U - 8U * static_cast<unsigned>(k))) & 0xFFU);
        }
    }
    return written;
}

} // namespace smply::transport::detail

#endif // SMPLY_TRANSPORTS_SERIAL_BASE64_HPP
