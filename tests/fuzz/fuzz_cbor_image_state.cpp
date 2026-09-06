// SPDX-License-Identifier: Apache-2.0
//
// The image-state decoder, reached the way a device reaches it.
//
// `decode_state` has no public entry point, and going through a real client is
// the better target anyway: it fuzzes the whole path a device drives -- framing,
// correlation, error extraction and the group decode -- rather than a function
// lifted out of it.
//
// The property is bounded work. An image-state response carries a list of
// slots, each with a version string and a hash, and every one of those lengths
// comes from the device. `limits::kMaxImages`, `kMaxSlotsPerImage`,
// `kMaxVersionStringLength` and `kMaxImageHashLength` are what stand between a
// hostile response and an allocation sized by an attacker; ASan's allocator
// limit is what notices if one of them stops holding.

#include "fuzz_support.hpp"

#include "smply/groups/image.hpp"
#include "smply/limits.hpp"
#include "smply/result.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > smply::fuzz::kMaxUsefulInput) {
        return 0;
    }

    smply::fuzz::ClientHarness harness;
    smply::ImageManagement image{harness.client};

    std::optional<smply::ImageState> state;
    static_cast<void>(image.get_state([&state](smply::Result<smply::ImageState> result) {
        if (result.has_value()) {
            state = std::move(*result);
        }
    }));
    if (harness.transport.sent().empty()) {
        return 0;
    }

    const smply::Result<smply::Header> request =
        smply::decode_header(smply::ConstBytes{harness.transport.last_sent()});
    assert(request.has_value());

    const std::vector<std::byte> message =
        smply::fuzz::response_to(*request, smply::fuzz::view(data, size));
    harness.transport.deliver(smply::ConstBytes{message});
    harness.client.poll(harness.clock.now());

    if (!state.has_value()) {
        return 0;
    }

    // Anything that decoded must be within the bounds the limits promise.
    assert(state->slots.size() <= smply::limits::kMaxImages);
    for (const smply::ImageSlot& slot : state->slots) {
        assert(slot.version.size() <= smply::limits::kMaxVersionStringLength);
        if (slot.hash.has_value()) {
            assert(slot.hash->size() <= smply::limits::kMaxImageHashLength);
        }
    }
    return 0;
}
