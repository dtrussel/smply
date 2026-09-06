// SPDX-License-Identifier: Apache-2.0
//
// The MCUboot TLV scanner over an arbitrary file.
//
// The strongest candidate in the library for fuzzing, and P9 said so at the
// time: it walks a trailer using offsets and lengths the *file* supplies, in
// two areas whose sizes must agree with a third number in the header. Every
// bound it applies is one this target can attack.
//
// Two properties beyond "no crash": the scan must terminate -- every advance is
// at least a four-byte entry header, and `kMaxImageTlvs` caps the work -- and a
// hash it returns must be a length the protocol allows.

#include "fuzz_support.hpp"

#include "smply/image_source.hpp"
#include "smply/limits.hpp"
#include "smply/mcuboot_image.hpp"
#include "smply/result.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < smply::kMcubootHeaderSize || size > smply::fuzz::kMaxUsefulInput) {
        return 0;
    }

    const smply::ConstBytes bytes = smply::fuzz::view(data, size);
    smply::MemoryImageSource source{bytes};

    const smply::Result<smply::McubootImageInfo> info =
        smply::parse_mcuboot_header(bytes.first(smply::kMcubootHeaderSize));
    if (!info.has_value()) {
        return 0;
    }

    const smply::Result<std::optional<smply::ImageHash>> hash =
        smply::find_image_tlv_hash(source, *info);
    if (!hash.has_value() || !hash->has_value()) {
        return 0;
    }

    // A hash that came back is 32 or 64 bytes: IMAGE_SHA_LEN is one or the
    // other and nothing else (docs/protocol-notes.md section 6).
    const std::size_t length = (*hash)->size();
    assert(length == 32 || length == 64);
    assert(length <= smply::limits::kMaxImageHashLength);
    return 0;
}
