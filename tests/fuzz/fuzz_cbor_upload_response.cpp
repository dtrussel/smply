// SPDX-License-Identifier: Apache-2.0
//
// The upload-response decoder, reached through a live upload.
//
// Small, but it is device-supplied and it feeds a state machine with budgets:
// the offset it carries is authoritative and drives what the client sends next
// (docs/protocol-notes.md section 6, rule 5). A response that could push the
// session past the image size, or drive the retry budgets somewhere they cannot
// come back from, would be a real defect rather than a decode bug.

#include "fuzz_support.hpp"

#include "smply/groups/image.hpp"
#include "smply/image_source.hpp"
#include "smply/limits.hpp"
#include "smply/result.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

/// A minimal well-formed image: the client only reads bytes from it, so the
/// content matters far less than that the size is fixed and small.
constexpr std::size_t kImageSize = 256;

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > smply::fuzz::kMaxUsefulInput) {
        return 0;
    }

    std::array<std::byte, kImageSize> image{};
    smply::MemoryImageSource source{smply::ConstBytes{image}};

    smply::fuzz::ClientHarness harness;
    smply::ImageManagement management{harness.client};

    std::optional<smply::UploadResult> result;
    smply::UploadOptions options;
    options.sha = smply::Hash{};
    const smply::UploadHandle handle = management.upload(
        source, options, [](smply::UploadProgress) {},
        [&result](smply::Result<smply::UploadResult> outcome) {
            if (outcome.has_value()) {
                result = *outcome;
            }
        });
    harness.client.poll(harness.clock.now());
    if (!handle.valid() || harness.transport.sent().empty()) {
        return 0;
    }

    const smply::Result<smply::Header> request =
        smply::decode_header(smply::ConstBytes{harness.transport.last_sent()});
    assert(request.has_value());

    const std::vector<std::byte> message =
        smply::fuzz::response_to(*request, smply::fuzz::view(data, size));
    harness.transport.deliver(smply::ConstBytes{message});
    harness.client.poll(harness.clock.now());

    // However the response decoded, the client cannot believe it transferred
    // more than the image holds.
    assert(management.transferred(handle) <= kImageSize);
    if (result.has_value()) {
        assert(result->transferred <= kImageSize);
    }
    return 0;
}
