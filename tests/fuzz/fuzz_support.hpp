// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TESTS_FUZZ_SUPPORT_HPP
#define SMPLY_TESTS_FUZZ_SUPPORT_HPP

/// \file
/// Shared scaffolding for the fuzz targets.
///
/// Three of the targets fuzz a decoder that has no public entry point of its
/// own -- the image-state and upload-response decoders are file-local, and
/// reaching them means going through a real `SmpClient`. That is the better
/// target anyway: it fuzzes the path a device actually drives, framing,
/// correlation and all, rather than a function lifted out of it.
///
/// Everything here is deliberately allocation-light and constructed per input:
/// libFuzzer calls the entry point millions of times, and state left between
/// calls makes a crash unreproducible from the input alone.

#include "fake_transport.hpp"
#include "manual_clock.hpp"

#include "smply/bytes.hpp"
#include "smply/group.hpp"
#include "smply/smp/header.hpp"
#include "smply/smp_client.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace smply::fuzz {

/// A client bound to a fake transport, with nothing else attached.
///
/// Declaration order is the one the library requires: the transport outlives
/// the client, which touches it while being destroyed.
struct ClientHarness
{
    test::FakeTransport transport;
    test::ManualClock clock;
    SmpClient client{transport, clock};
};

/// Wraps \p payload in a well-formed SMP response header addressed to
/// \p request, so a decoder is reached with the fuzzer's bytes as the payload.
///
/// The header is *correct* on purpose. `fuzz_smp_client_rx` fuzzes the framing;
/// these targets fuzz what is inside a frame that already correlated, which is
/// the only way to reach the group decoders at all.
[[nodiscard]] inline std::vector<std::byte> response_to(const Header& request, ConstBytes payload)
{
    const Header reply{.op = smply::response_to(request.op),
                       .version = request.version,
                       .flags = 0,
                       .length = static_cast<std::uint16_t>(payload.size()),
                       .group = request.group,
                       .seq = request.seq,
                       .command = request.command};

    const std::array<std::byte, kHeaderSize> encoded = encode(reply);
    std::vector<std::byte> out;
    out.reserve(encoded.size() + payload.size());
    out.insert(out.end(), encoded.begin(), encoded.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/// A view over the fuzzer's buffer, without copying it.
[[nodiscard]] inline ConstBytes view(const std::uint8_t* data, std::size_t size) noexcept
{
    // reinterpret_cast is confined to this one line, where it converts the
    // driver's buffer to the byte type the library speaks. It is not a cast
    // over device data into a structure, which docs/design.md section 11
    // forbids.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return ConstBytes{reinterpret_cast<const std::byte*>(data), size};
}

/// libFuzzer runs a target millions of times; a payload larger than the
/// protocol allows tells us nothing new and costs time in every one of them.
inline constexpr std::size_t kMaxUsefulInput = 64 * 1024;

} // namespace smply::fuzz

#endif // SMPLY_TESTS_FUZZ_SUPPORT_HPP
