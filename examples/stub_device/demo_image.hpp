// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_EXAMPLE_DEMO_IMAGE_HPP
#define SMPLY_EXAMPLE_DEMO_IMAGE_HPP

/// \file
/// Builds a valid MCUboot image, so the example runs with no arguments.
///
/// Scaffolding, not a demonstration of anything. It exists because the demo
/// needs two images that differ -- one the stub device is already running, one
/// to install -- and because CI runs this program with no firmware file to hand
/// it. `--image` uses a real file instead.
///
/// The layout is docs/protocol-notes.md section 7, little-endian: a 32-byte
/// header, a filler body, and one unprotected TLV area carrying
/// `IMAGE_TLV_SHA256`. That is the minimum the server-side checks look at (the
/// magic, and a first chunk long enough to hold the header) and the minimum
/// `find_image_tlv_hash()` needs.

#include "smply/bytes.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace smply::example {

/// A version, for both the header and the reported version string.
struct DemoVersion
{
    std::uint8_t major = 1;
    std::uint8_t minor = 0;
    std::uint16_t revision = 0;
    std::uint32_t build = 0;
};

/// Assembles an image whose hash TLV is derived from \p version, so two demo
/// images with different versions have different hashes -- which is what makes
/// the update an update rather than a no-op the device skips.
///
/// The TLV value is a deterministic pattern, **not** a real digest of the
/// content. Nothing in this flow verifies it: `find_image_tlv_hash()` reads it
/// as an opaque value, and the stub device answers no `match` (protocol-notes
/// A6). Computing a genuine SHA-256 here would look more rigorous and prove
/// exactly as much.
[[nodiscard]] std::vector<std::byte> build_demo_image(DemoVersion version,
                                                      std::size_t body_size = 4096);

/// The version string a device would report for \p version: dotted, with the
/// build number appended only when it is non-zero (protocol-notes section 6).
[[nodiscard]] std::string demo_version_string(DemoVersion version);

} // namespace smply::example

#endif // SMPLY_EXAMPLE_DEMO_IMAGE_HPP
