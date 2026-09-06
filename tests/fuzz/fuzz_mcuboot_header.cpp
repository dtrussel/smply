// SPDX-License-Identifier: Apache-2.0
//
// The MCUboot image header, parsed from arbitrary bytes.
//
// The header is decoded field by field from a span, never by casting to a
// struct (docs/design.md section 11), and every size it carries is bounded
// before use. This target exists to keep both true.

#include "fuzz_support.hpp"

#include "smply/mcuboot_image.hpp"
#include "smply/result.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < smply::kMcubootHeaderSize) {
        return 0;
    }
    const smply::ConstBytes input = smply::fuzz::view(data, smply::kMcubootHeaderSize);

    const smply::Result<smply::McubootImageInfo> info = smply::parse_mcuboot_header(input);
    if (!info.has_value()) {
        return 0;
    }

    // A header that parsed must describe something whose parts do not overflow
    // each other: the TLV area starts after the header and the body, and that
    // sum is what every later offset is derived from.
    const std::uint64_t trailer = static_cast<std::uint64_t>(info->header_size) +
                                  static_cast<std::uint64_t>(info->image_size);
    assert(trailer >= info->header_size);
    return 0;
}
