// SPDX-License-Identifier: Apache-2.0

#include "smply/groups/settings.hpp"

#include "cbor/cbor.hpp"
#include "detail/narrow.hpp"
#include "groups/common.hpp"
#include "smply/error.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace smply {
namespace {

using groups::command_id;
using groups::reject;

/// Settings-group command IDs (docs/protocol-notes.md section 11). Two
/// commands share an ID each and are told apart by the operation: 0 is read
/// or write, 3 is load (read) or save (write).
enum class SettingsCommand : std::uint8_t
{
    ReadWrite = 0,
    Delete = 1,
    Commit = 2,
    LoadSave = 3,
};

/// The largest request is a write: one map header, the key "name" (five bytes)
/// and a text header of at most two bytes, the key "val" (four bytes) and a
/// byte-string header of at most three bytes, plus the name and value
/// themselves. A read's "max_size" key and number are smaller than the value
/// they replace.
constexpr std::size_t kLargestRequestEnvelope = 1 + 5 + 2 + 4 + 3;
constexpr std::size_t kRequestBufferSize =
    limits::kMaxSettingNameLength + limits::kMaxSettingValueLength + 32;

static_assert(limits::kMaxSettingNameLength <= 0xFF,
              "the envelope above assumes a one-byte text length");
static_assert(limits::kMaxSettingValueLength <= 0xFFFF,
              "the envelope above assumes a two-byte byte-string length");
static_assert(kRequestBufferSize >= limits::kMaxSettingNameLength + limits::kMaxSettingValueLength +
                                        kLargestRequestEnvelope,
              "the request buffer must fit the longest legal name and value");

constexpr const char* kBufferTooSmall = "settings: request buffer too small";
constexpr const char* kInvalidName = "settings: invalid setting name";

[[nodiscard]] bool valid_name(std::string_view name) noexcept
{
    return groups::is_valid_name(name, limits::kMaxSettingNameLength);
}

/// Decodes a read response, refusing a value longer than \p limit: the
/// `max_size` the read asked for, or smply's own bound when it asked for none.
[[nodiscard]] Result<SettingValue> decode_value(cbor::Reader& reader, std::size_t limit)
{
    const std::optional<ConstBytes> value = reader.bytes("val");
    const std::optional<std::uint64_t> max_size = reader.uint("max_size");
    static_cast<void>(reader.leave_map());

    if (const auto status = reader.status(); !status.has_value()) {
        return fail(status.error());
    }
    if (!value.has_value()) {
        return fail(ErrorCode::CborDecode, "settings: read reply has no value");
    }
    if (value->size() > limit) {
        // Bounded before the copy. A device returning more than was asked for
        // is not answering the question.
        return fail(ErrorCode::CborDecode, "settings: value too long");
    }
    SettingValue result;
    if (max_size.has_value()) {
        // The server decodes and reports the size as 32 bits.
        const std::optional<std::uint32_t> narrowed =
            detail::checked_narrow<std::uint32_t>(*max_size);
        if (!narrowed.has_value()) {
            return fail(ErrorCode::CborDecode, "settings: max_size out of range");
        }
        result.max_size = narrowed;
    }
    // The view points into the assembler's buffer, valid only for this
    // callback. The copy is what the caller keeps.
    result.value.assign(value->begin(), value->end());
    return result;
}

} // namespace

std::optional<SettingsError> settings_error(const Error& error) noexcept
{
    const std::optional<MgmtError>& mgmt = error.mgmt();
    if (!mgmt.has_value() || !mgmt->group_scoped || mgmt->group != Group::Settings) {
        return std::nullopt;
    }
    return static_cast<SettingsError>(mgmt->rc);
}

SettingsManagement::SettingsManagement(SmpClient& client) noexcept : client_{&client} {}

RequestHandle SettingsManagement::read(std::string_view name, Callback<SettingValue> on_done)
{
    if (!valid_name(name)) {
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::InvalidArgument, kInvalidName});
    }

    std::array<std::byte, kRequestBufferSize> buffer{};
    cbor::Writer writer{MutBytes{buffer}};
    return groups::send(
        *client_,
        RequestSpec{.op = Operation::Read,
                    .group = Group::Settings,
                    .command = command_id(SettingsCommand::ReadWrite),
                    .payload = {},
                    .timeout = {}},
        writer.open_map().put_text("name", name).close_map().finish(), std::move(on_done),
        [](cbor::Reader& reader) { return decode_value(reader, limits::kMaxSettingValueLength); },
        kBufferTooSmall);
}

RequestHandle SettingsManagement::read(std::string_view name, std::uint32_t max_size,
                                       Callback<SettingValue> on_done)
{
    if (!valid_name(name)) {
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::InvalidArgument, kInvalidName});
    }
    if (max_size == 0 || max_size > limits::kMaxSettingValueLength) {
        // Zero can return nothing, and a device with heap buffers may fail to
        // allocate it; above smply's own bound a reply could not be accepted.
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::InvalidArgument, "settings: max_size out of range"});
    }

    std::array<std::byte, kRequestBufferSize> buffer{};
    cbor::Writer writer{MutBytes{buffer}};
    return groups::send(
        *client_,
        RequestSpec{.op = Operation::Read,
                    .group = Group::Settings,
                    .command = command_id(SettingsCommand::ReadWrite),
                    .payload = {},
                    .timeout = {}},
        writer.open_map()
            .put_text("name", name)
            .put_uint("max_size", max_size)
            .close_map()
            .finish(),
        std::move(on_done),
        [max_size](cbor::Reader& reader) { return decode_value(reader, max_size); },
        kBufferTooSmall);
}

RequestHandle SettingsManagement::write(std::string_view name, ConstBytes value,
                                        Callback<void> on_done)
{
    if (!valid_name(name)) {
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::InvalidArgument, kInvalidName});
    }
    if (value.size() > limits::kMaxSettingValueLength) {
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::InvalidArgument, "settings: value too long"});
    }

    std::array<std::byte, kRequestBufferSize> buffer{};
    cbor::Writer writer{MutBytes{buffer}};
    return groups::send(
        *client_,
        RequestSpec{.op = Operation::Write,
                    .group = Group::Settings,
                    .command = command_id(SettingsCommand::ReadWrite),
                    .payload = {},
                    .timeout = {}},
        writer.open_map().put_text("name", name).put_bytes("val", value).close_map().finish(),
        std::move(on_done), groups::decode_nothing, kBufferTooSmall);
}

RequestHandle SettingsManagement::erase(std::string_view name, Callback<void> on_done)
{
    if (!valid_name(name)) {
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::InvalidArgument, kInvalidName});
    }

    std::array<std::byte, kRequestBufferSize> buffer{};
    cbor::Writer writer{MutBytes{buffer}};
    return groups::send(*client_,
                        RequestSpec{.op = Operation::Write,
                                    .group = Group::Settings,
                                    .command = command_id(SettingsCommand::Delete),
                                    .payload = {},
                                    .timeout = {}},
                        writer.open_map().put_text("name", name).close_map().finish(),
                        std::move(on_done), groups::decode_nothing, kBufferTooSmall);
}

RequestHandle SettingsManagement::commit(Callback<void> on_done)
{
    std::array<std::byte, kRequestBufferSize> buffer{};
    return groups::send(*client_,
                        RequestSpec{.op = Operation::Write,
                                    .group = Group::Settings,
                                    .command = command_id(SettingsCommand::Commit),
                                    .payload = {},
                                    .timeout = {}},
                        groups::encode_empty(MutBytes{buffer}), std::move(on_done),
                        groups::decode_nothing, kBufferTooSmall);
}

RequestHandle SettingsManagement::load(Callback<void> on_done)
{
    std::array<std::byte, kRequestBufferSize> buffer{};
    return groups::send(*client_,
                        RequestSpec{.op = Operation::Read,
                                    .group = Group::Settings,
                                    .command = command_id(SettingsCommand::LoadSave),
                                    .payload = {},
                                    .timeout = {}},
                        groups::encode_empty(MutBytes{buffer}), std::move(on_done),
                        groups::decode_nothing, kBufferTooSmall);
}

RequestHandle SettingsManagement::save(const SaveOptions& options, Callback<void> on_done)
{
    // No "name" at all is what asks for everything; an empty name would be
    // refused by the device (docs/protocol-notes.md section 11).
    std::array<std::byte, kRequestBufferSize> buffer{};
    return groups::send(*client_,
                        RequestSpec{.op = Operation::Write,
                                    .group = Group::Settings,
                                    .command = command_id(SettingsCommand::LoadSave),
                                    .payload = {},
                                    .timeout = options.timeout},
                        groups::encode_empty(MutBytes{buffer}), std::move(on_done),
                        groups::decode_nothing, kBufferTooSmall);
}

RequestHandle SettingsManagement::save(std::string_view name, const SaveOptions& options,
                                       Callback<void> on_done)
{
    if (!valid_name(name)) {
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::InvalidArgument, kInvalidName});
    }

    std::array<std::byte, kRequestBufferSize> buffer{};
    cbor::Writer writer{MutBytes{buffer}};
    return groups::send(*client_,
                        RequestSpec{.op = Operation::Write,
                                    .group = Group::Settings,
                                    .command = command_id(SettingsCommand::LoadSave),
                                    .payload = {},
                                    .timeout = options.timeout},
                        writer.open_map().put_text("name", name).close_map().finish(),
                        std::move(on_done), groups::decode_nothing, kBufferTooSmall);
}

RequestHandle SettingsManagement::save(Callback<void> on_done)
{
    return save(SaveOptions{}, std::move(on_done));
}

RequestHandle SettingsManagement::save(std::string_view name, Callback<void> on_done)
{
    return save(name, SaveOptions{}, std::move(on_done));
}

} // namespace smply
