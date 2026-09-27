// SPDX-License-Identifier: Apache-2.0
//
// The fuzz target fuzz_cbor_statistics reaches both decoders here through a
// real client.

#include "smply/groups/statistics.hpp"

#include "cbor/cbor.hpp"
#include "groups/common.hpp"
#include "smply/error.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace smply {
namespace {

using groups::command_id;
using groups::reject;

/// Statistics-group command IDs (docs/protocol-notes.md section 10).
enum class StatisticsCommand : std::uint8_t
{
    Show = 0,
    List = 1,
};

/// The largest request is a group read: one map header, the key "name" (five
/// bytes), a text-string header of at most two bytes for a name this short,
/// and the name itself. The list request is the empty map.
constexpr std::size_t kLargestRequestEnvelope = 1 + 5 + 2;
constexpr std::size_t kRequestBufferSize = limits::kMaxStatisticsNameLength + 16;

static_assert(limits::kMaxStatisticsNameLength <= 0xFF,
              "the envelope above assumes a one-byte text length");
static_assert(kRequestBufferSize >= limits::kMaxStatisticsNameLength + kLargestRequestEnvelope,
              "the request buffer must fit the longest legal group name");

constexpr const char* kBufferTooSmall = "stat: request buffer too small";

/// Decodes a list-groups response.
[[nodiscard]] Result<std::vector<std::string>> decode_list(cbor::Reader& reader)
{
    std::vector<std::string> names;
    const Result<bool> present = reader.for_each_text_in_array(
        "stat_list", limits::kMaxStatisticsGroups, [&names](std::string_view name) -> Result<void> {
            if (name.size() > limits::kMaxStatisticsNameLength) {
                // Bounded before the copy.
                return fail(ErrorCode::CborDecode, "stat: group name too long");
            }
            // The view points into the assembler's buffer, valid only for this
            // callback. The copy is what the caller keeps.
            names.emplace_back(name);
            return {};
        });
    static_cast<void>(reader.leave_map());

    if (const auto status = reader.status(); !status.has_value()) {
        return fail(status.error());
    }
    if (!present.has_value()) {
        return fail(present.error());
    }
    if (!*present) {
        // The server always writes the list, empty or not; its absence means
        // this is not an answer to the question.
        return fail(ErrorCode::CborDecode, "stat: list has no stat_list");
    }
    return names;
}

/// Decodes a group-data response.
[[nodiscard]] Result<StatisticsGroup> decode_group(cbor::Reader& reader)
{
    StatisticsGroup group;
    const std::optional<std::string_view> name = reader.text("name");
    const Result<bool> present = reader.for_each_uint_in_map(
        "fields", limits::kMaxStatisticsFields,
        [&group](std::string_view field, std::uint64_t value) -> Result<void> {
            if (field.size() > limits::kMaxStatisticsNameLength) {
                return fail(ErrorCode::CborDecode, "stat: field name too long");
            }
            if (group.find(field) != nullptr) {
                // A repeated name would make find() answer with whichever came
                // first. The server's names are C identifiers of one struct, so
                // a repeat is not something a real device sends.
                return fail(ErrorCode::CborDecode, "stat: duplicate field name");
            }
            group.fields.push_back(StatisticsField{.name = std::string{field}, .value = value});
            return {};
        });
    static_cast<void>(reader.leave_map());

    // Checked before anything decoded is trusted: a wrong-typed field poisons
    // the reader and leaves the rest looking merely absent.
    if (const auto status = reader.status(); !status.has_value()) {
        return fail(status.error());
    }
    if (!present.has_value()) {
        return fail(present.error());
    }
    if (!name.has_value() || !*present) {
        return fail(ErrorCode::CborDecode, "stat: group data incomplete");
    }
    if (name->size() > limits::kMaxStatisticsNameLength) {
        return fail(ErrorCode::CborDecode, "stat: group name too long");
    }
    group.name = std::string{*name};
    return group;
}

} // namespace

const StatisticsField* StatisticsGroup::find(std::string_view field_name) const noexcept
{
    for (const StatisticsField& field : fields) {
        if (field.name == field_name) {
            return &field;
        }
    }
    return nullptr;
}

std::optional<StatisticsError> statistics_error(const Error& error) noexcept
{
    const std::optional<MgmtError>& mgmt = error.mgmt();
    if (!mgmt.has_value() || !mgmt->group_scoped || mgmt->group != Group::Stat) {
        return std::nullopt;
    }
    return static_cast<StatisticsError>(mgmt->rc);
}

StatisticsManagement::StatisticsManagement(SmpClient& client) noexcept : client_{&client} {}

RequestHandle StatisticsManagement::list_groups(Callback<std::vector<std::string>> on_done)
{
    std::array<std::byte, kRequestBufferSize> buffer{};
    return groups::send(*client_,
                        RequestSpec{.op = Operation::Read,
                                    .group = Group::Stat,
                                    .command = command_id(StatisticsCommand::List),
                                    .payload = {},
                                    .timeout = {}},
                        groups::encode_empty(MutBytes{buffer}), std::move(on_done), decode_list,
                        kBufferTooSmall);
}

RequestHandle StatisticsManagement::read_group(std::string_view name,
                                               Callback<StatisticsGroup> on_done)
{
    if (!groups::is_valid_name(name, limits::kMaxStatisticsNameLength)) {
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::InvalidArgument, "stat: invalid group name"});
    }

    std::array<std::byte, kRequestBufferSize> buffer{};
    cbor::Writer writer{MutBytes{buffer}};
    return groups::send(*client_,
                        RequestSpec{.op = Operation::Read,
                                    .group = Group::Stat,
                                    .command = command_id(StatisticsCommand::Show),
                                    .payload = {},
                                    .timeout = {}},
                        writer.open_map().put_text("name", name).close_map().finish(),
                        std::move(on_done), decode_group, kBufferTooSmall);
}

} // namespace smply
