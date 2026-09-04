// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TESTS_MESSAGE_BUILDER_HPP
#define SMPLY_TESTS_MESSAGE_BUILDER_HPP

/// \file
/// Builders for raw SMP messages, used to feed decoders and (from P3) the
/// reassembler.
///
/// Two flavours deliberately: `make_message` builds a well-formed message and
/// keeps the length field consistent with the payload, while `make_raw_message`
/// lets a test state the length field independently. The second is what makes
/// hostile input expressible -- a device claiming 60 KiB and sending four bytes
/// is exactly the case the bounds exist for.

#include "smply/bytes.hpp"
#include "smply/smp/header.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace smply::test {

/// Raw bytes, spelled without casts at every call site.
inline std::vector<std::byte> bytes_of(std::initializer_list<std::uint8_t> values)
{
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const std::uint8_t value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

/// A well-formed message: the header's length field is set from `payload`.
inline std::vector<std::byte> make_message(Header header, ConstBytes payload = {})
{
    header.length = static_cast<std::uint16_t>(payload.size());
    const auto encoded = encode(header);

    std::vector<std::byte> out;
    out.reserve(encoded.size() + payload.size());
    out.insert(out.end(), encoded.begin(), encoded.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/// A message whose declared length is whatever `header.length` says, regardless
/// of how many payload bytes actually follow. For malformed and hostile input.
inline std::vector<std::byte> make_raw_message(const Header& header, ConstBytes payload = {})
{
    const auto encoded = encode(header);

    std::vector<std::byte> out;
    out.reserve(encoded.size() + payload.size());
    out.insert(out.end(), encoded.begin(), encoded.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/// `count` bytes of filler, distinguishable by position so a misordered or
/// truncated copy is visible in a failure message.
inline std::vector<std::byte> filler(std::size_t count, std::uint8_t seed = 0)
{
    std::vector<std::byte> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(static_cast<std::byte>((i + seed) & 0xFFU));
    }
    return out;
}

} // namespace smply::test

#endif // SMPLY_TESTS_MESSAGE_BUILDER_HPP
