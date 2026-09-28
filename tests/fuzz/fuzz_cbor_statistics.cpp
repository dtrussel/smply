// SPDX-License-Identifier: Apache-2.0
//
// The statistics decoders, reached the way a device reaches them.
//
// The first input byte chooses the command -- even lists groups, odd reads one
// -- and the rest is delivered as the correlated response, through a real
// client, like fuzz_cbor_image_state.
//
// This group is the first whose responses carry containers sized *and keyed*
// by the device: a list of names, and a map whose keys are data rather than
// field names, read by two façade visitors that sit beside QCBOR's
// consecutive-break defect (protocol-notes section 9, A18). The property is
// bounded work and bounded storage: `kMaxStatisticsGroups`,
// `kMaxStatisticsFields` and `kMaxStatisticsNameLength` all hold on anything
// that decodes, and field names stay unique so `find()` means one thing.

#include "fuzz_support.hpp"

#include "smply/groups/statistics.hpp"
#include "smply/limits.hpp"
#include "smply/result.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size < 1 || size > smply::fuzz::kMaxUsefulInput) {
        return 0;
    }
    const bool read_group = (data[0] & 1U) != 0;

    smply::fuzz::ClientHarness harness;
    smply::StatisticsManagement stats{harness.client};

    std::optional<std::vector<std::string>> names;
    std::optional<smply::StatisticsGroup> group;
    if (read_group) {
        static_cast<void>(
            stats.read_group("net", [&group](smply::Result<smply::StatisticsGroup> result) {
                if (result.has_value()) {
                    group = std::move(*result);
                }
            }));
    } else {
        static_cast<void>(
            stats.list_groups([&names](smply::Result<std::vector<std::string>> result) {
                if (result.has_value()) {
                    names = std::move(*result);
                }
            }));
    }
    if (harness.transport.sent().empty()) {
        return 0;
    }

    const smply::Result<smply::Header> request =
        smply::decode_header(smply::ConstBytes{harness.transport.last_sent()});
    assert(request.has_value());

    const std::vector<std::byte> message =
        smply::fuzz::response_to(*request, smply::fuzz::view(data + 1, size - 1));
    harness.transport.deliver(smply::ConstBytes{message});
    harness.client.poll(harness.clock.now());

    if (names.has_value()) {
        assert(names->size() <= smply::limits::kMaxStatisticsGroups);
        for (const std::string& name : *names) {
            assert(name.size() <= smply::limits::kMaxStatisticsNameLength);
        }
    }
    if (group.has_value()) {
        assert(group->name.size() <= smply::limits::kMaxStatisticsNameLength);
        assert(group->fields.size() <= smply::limits::kMaxStatisticsFields);
        for (const smply::StatisticsField& field : group->fields) {
            assert(field.name.size() <= smply::limits::kMaxStatisticsNameLength);
            // Unique: the first field of this name is this one.
            assert(group->find(field.name) == &field);
        }
    }
    return 0;
}
