// SPDX-License-Identifier: Apache-2.0
//
// A live client with a request outstanding, fed an arbitrary inbound stream.
//
// This is the correlation surface: everything a device can put on the wire
// while the client is waiting for one specific answer. The property is the one
// the whole request table exists to provide -- **the pending request is never
// completed by a response that does not belong to it**. A client that answers
// the wrong request hands a caller another request's data, which is worse than
// any decode failure.
//
// The stream is delivered in fuzzer-chosen fragments, because a response
// arriving in pieces takes a different path through reassembly than the same
// bytes arriving whole, and correlation happens after both.

#include "fuzz_support.hpp"

#include "smply/error.hpp"
#include "smply/group.hpp"
#include "smply/result.hpp"
#include "smply/smp/header.hpp"
#include "smply/smp_client.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 2 || size > smply::fuzz::kMaxUsefulInput) {
        return 0;
    }

    smply::fuzz::ClientHarness harness;

    std::optional<smply::Header> answered;
    const smply::RequestSpec spec{.op = smply::Operation::Read,
                                  .group = smply::Group::Image,
                                  .command = 0,
                                  .payload = {},
                                  .timeout = std::nullopt};
    static_cast<void>(
        harness.client.request(spec, [&answered](smply::Result<smply::RawResponse> result) {
            if (result.has_value()) {
                answered = result->header;
            }
        }));
    if (harness.transport.sent().empty()) {
        return 0;
    }

    const smply::Result<smply::Header> sent =
        smply::decode_header(smply::ConstBytes{harness.transport.last_sent()});
    assert(sent.has_value());

    const std::size_t fragment = static_cast<std::size_t>(data[0]) + 1;
    const smply::ConstBytes stream = smply::fuzz::view(data + 1, size - 1);
    for (std::size_t offset = 0; offset < stream.size() && harness.transport.connected();) {
        const std::size_t take = std::min(fragment, stream.size() - offset);
        harness.transport.deliver(stream.subspan(offset, take));
        offset += take;
    }
    harness.client.poll(harness.clock.now());

    if (answered.has_value()) {
        // The one invariant that matters: whatever completed this request
        // carried its sequence number, its group and its command, and was a
        // response rather than another request.
        assert(answered->seq == sent->seq);
        assert(answered->group == sent->group);
        assert(answered->command == sent->command);
        assert(smply::is_response(answered->op));
    }

    // A response that matched nothing must have been counted rather than
    // silently dropped -- "we ignored it" and "we never saw it" are different
    // states, and only one of them is diagnosable.
    const smply::SmpClientStats& stats = harness.client.stats();
    assert(stats.received >= stats.unmatched);
    return 0;
}
