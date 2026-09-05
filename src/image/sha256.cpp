// SPDX-License-Identifier: Apache-2.0

#include "image/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace smply::image {
namespace {

/// The first 32 bits of the fractional parts of the cube roots of the first 64
/// primes (FIPS 180-4 section 4.2.2).
constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U,
    0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU,
    0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU,
    0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU, 0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
    0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU,
    0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
    0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U, 0x19A4C116U,
    0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U,
    0xC67178F2U};

[[nodiscard]] constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned bits) noexcept
{
    // Well defined for the 1..31 range this is used with; a rotate of 0 would
    // shift by 32, which is undefined, and never occurs below.
    return (value >> bits) | (value << (32U - bits));
}

/// Big-endian, as SHA-256 defines its input and output words.
[[nodiscard]] std::uint32_t load_be32(const std::byte* from) noexcept
{
    return (static_cast<std::uint32_t>(from[0]) << 24U) |
           (static_cast<std::uint32_t>(from[1]) << 16U) |
           (static_cast<std::uint32_t>(from[2]) << 8U) | static_cast<std::uint32_t>(from[3]);
}

void store_be32(std::uint32_t value, std::byte* to) noexcept
{
    to[0] = static_cast<std::byte>((value >> 24U) & 0xFFU);
    to[1] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    to[2] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    to[3] = static_cast<std::byte>(value & 0xFFU);
}

} // namespace

void Sha256::compress(const std::byte* block) noexcept
{
    // The message schedule, FIPS 180-4 section 6.2.2 step 1.
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t i = 0; i < 16; ++i) {
        schedule[i] = load_be32(block + (i * 4));
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotate_right(schedule[i - 15], 7) ^
                                 rotate_right(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3U);
        const std::uint32_t s1 = rotate_right(schedule[i - 2], 17) ^
                                 rotate_right(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10U);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t sigma1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + sigma1 + choose + kRoundConstants[i] + schedule[i];
        const std::uint32_t sigma0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = sigma0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(ConstBytes data) noexcept
{
    length_ += data.size();

    // Top up a partial block first, so the block-at-a-time path below always
    // starts on a boundary.
    if (buffered_ != 0) {
        const std::size_t wanted = std::min(kSha256BlockSize - buffered_, data.size());
        std::copy_n(data.begin(), wanted, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
        buffered_ += wanted;
        data = data.subspan(wanted);
        if (buffered_ < kSha256BlockSize) {
            return;
        }
        compress(buffer_.data());
        buffered_ = 0;
    }

    while (data.size() >= kSha256BlockSize) {
        compress(data.data());
        data = data.subspan(kSha256BlockSize);
    }

    std::copy(data.begin(), data.end(), buffer_.begin());
    buffered_ = data.size();
}

std::array<std::byte, kSha256DigestSize> Sha256::finish() noexcept
{
    // Padding, FIPS 180-4 section 5.1.1: a 1 bit, then zeros, then the message
    // length in bits as a 64-bit big-endian value.
    const std::uint64_t bit_length = length_ * 8U;

    buffer_[buffered_] = std::byte{0x80};
    ++buffered_;
    if (buffered_ > kSha256BlockSize - 8) {
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.end(),
                  std::byte{0});
        compress(buffer_.data());
        buffered_ = 0;
    }
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.end() - 8,
              std::byte{0});
    store_be32(static_cast<std::uint32_t>(bit_length >> 32U),
               buffer_.data() + kSha256BlockSize - 8);
    store_be32(static_cast<std::uint32_t>(bit_length & 0xFFFFFFFFU),
               buffer_.data() + kSha256BlockSize - 4);
    compress(buffer_.data());

    std::array<std::byte, kSha256DigestSize> digest{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
        store_be32(state_[i], digest.data() + (i * 4));
    }
    return digest;
}

} // namespace smply::image
