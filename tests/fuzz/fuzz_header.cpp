// SPDX-License-Identifier: Apache-2.0
//
// The SMP header decoder: eight bytes of anything.
//
// Beyond "no crash", the property that matters is that decoding is *faithful*.
// A header that decodes must re-encode to the bytes it came from -- otherwise
// the client and the device disagree about what was on the wire, which is how a
// response gets correlated to the wrong request.

#include "fuzz_support.hpp"

#include "smply/result.hpp"
#include "smply/smp/header.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const smply::ConstBytes input = smply::fuzz::view(data, size);
    const smply::Result<smply::Header> decoded = smply::decode_header(input);
    if (!decoded.has_value()) {
        return 0;
    }

    // Only the reserved version bits may be rejected, so anything accepted must
    // round-trip exactly.
    const auto encoded = smply::encode(*decoded);
    assert(input.size() >= smply::kHeaderSize);
    assert(std::equal(encoded.begin(), encoded.end(), input.begin()));

    // And decoding the re-encoding must be a fixed point.
    const smply::Result<smply::Header> again = smply::decode_header(smply::ConstBytes{encoded});
    assert(again.has_value());
    assert(*again == *decoded);
    return 0;
}
