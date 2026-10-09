// SPDX-License-Identifier: Apache-2.0

#include "smply/groups/os.hpp"

#include "cbor/cbor.hpp"
#include "detail/narrow.hpp"
#include "groups/common.hpp"
#include "smply/error.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace smply {
namespace {

using groups::command_id;
using groups::reject;

/// OS-group command IDs (docs/protocol-notes.md section 5, S7).
enum class OsCommand : std::uint8_t
{
    Echo = 0,
    Reset = 5,
    McumgrParameters = 6,
    BootloaderInfo = 8,
};

/// Every request in this group is a small, flat map. The largest is echo's:
/// one map header, the one-character key "d", a text-string header of at most
/// three bytes, and `kMaxEchoLength` bytes of text.
constexpr std::size_t kLargestRequestEnvelope = 1 + 2 + 3;
constexpr std::size_t kRequestBufferSize = limits::kMaxEchoLength + 16;

// The buffer is sized from the same constant that bounds echo's input, and the
// other two requests are smaller still ({} is one byte, {"force": true} is
// eight). Encoding therefore cannot run out of room -- which is why
// groups::send() reports a failure to encode as Internal.
static_assert(kRequestBufferSize >= limits::kMaxEchoLength + kLargestRequestEnvelope,
              "the request buffer must fit the largest legal echo string");

constexpr const char* kBufferTooSmall = "os: request buffer too small";

/// Decodes an mcumgr-parameters response.
[[nodiscard]] Result<McumgrParameters> decode_parameters(cbor::Reader& reader)
{
    const std::optional<std::uint64_t> buf_size = reader.uint("buf_size");
    const std::optional<std::uint64_t> buf_count = reader.uint("buf_count");
    static_cast<void>(reader.leave_map());

    // Checked before the values are trusted: a wrong-typed field poisons the
    // reader and leaves both looking merely absent.
    if (const auto status = reader.status(); !status.has_value()) {
        return fail(status.error());
    }
    if (!buf_size.has_value() || !buf_count.has_value()) {
        return fail(ErrorCode::CborDecode, "os: parameters incomplete");
    }
    const std::optional<std::uint32_t> size = detail::checked_narrow<std::uint32_t>(*buf_size);
    const std::optional<std::uint32_t> count = detail::checked_narrow<std::uint32_t>(*buf_count);
    if (!size.has_value() || !count.has_value()) {
        return fail(ErrorCode::CborDecode, "os: parameters out of range");
    }
    return McumgrParameters{.buf_size = *size, .buf_count = *count};
}

/// Decodes an echo response.
[[nodiscard]] Result<std::string> decode_echo(cbor::Reader& reader)
{
    const std::optional<std::string_view> echoed = reader.text("r");
    static_cast<void>(reader.leave_map());

    if (const auto status = reader.status(); !status.has_value()) {
        return fail(status.error());
    }
    if (!echoed.has_value()) {
        return fail(ErrorCode::CborDecode, "os: echo reply has no text");
    }
    if (echoed->size() > limits::kMaxEchoLength) {
        // Bounded before the copy. A device cannot make smply allocate on a
        // size it chose, and a reply longer than the request is not an answer
        // to it.
        return fail(ErrorCode::CborDecode, "os: echo reply too long");
    }
    // The view points into the assembler's buffer, which is valid only for
    // this callback. The copy is what the caller keeps.
    return std::string{*echoed};
}

/// Decodes a bootloader-information response to the empty query.
[[nodiscard]] Result<std::string> decode_bootloader_name(cbor::Reader& reader)
{
    const std::optional<std::string_view> name = reader.text("bootloader");
    static_cast<void>(reader.leave_map());

    if (const auto status = reader.status(); !status.has_value()) {
        return fail(status.error());
    }
    if (!name.has_value()) {
        return fail(ErrorCode::CborDecode, "os: bootloader info has no name");
    }
    if (name->size() > limits::kMaxBootloaderNameLength) {
        // Bounded before the copy, as echo's reply is.
        return fail(ErrorCode::CborDecode, "os: bootloader name too long");
    }
    return std::string{*name};
}

/// The mode for a number the device sent: one of MCUboot's ten, or `Unknown`.
[[nodiscard]] McubootMode mode_from(std::int64_t raw) noexcept
{
    constexpr std::int64_t kFirst = 0;
    constexpr auto kLast = static_cast<std::int64_t>(McubootMode::SwapUsingOffset);
    if (raw < kFirst || raw > kLast) {
        return McubootMode::Unknown;
    }
    return static_cast<McubootMode>(raw);
}

/// Decodes a bootloader-information response to the `mode` query.
[[nodiscard]] Result<BootloaderMode> decode_bootloader_mode(cbor::Reader& reader)
{
    const std::optional<std::int64_t> mode = reader.integer("mode");
    const std::optional<bool> no_downgrade = reader.boolean("no-downgrade");
    static_cast<void>(reader.leave_map());

    if (const auto status = reader.status(); !status.has_value()) {
        return fail(status.error());
    }
    if (!mode.has_value()) {
        return fail(ErrorCode::CborDecode, "os: bootloader info has no mode");
    }
    return BootloaderMode{
        .mode = mode_from(*mode), .raw_mode = *mode, .no_downgrade = no_downgrade.value_or(false)};
}

} // namespace

std::string_view to_string(McubootMode mode) noexcept
{
    switch (mode) {
    case McubootMode::Unknown:
        return "unknown";
    case McubootMode::SingleSlot:
        return "single-slot";
    case McubootMode::SwapUsingScratch:
        return "swap-using-scratch";
    case McubootMode::UpgradeOnly:
        return "upgrade-only";
    case McubootMode::SwapUsingMove:
        return "swap-using-move";
    case McubootMode::DirectXip:
        return "direct-xip";
    case McubootMode::DirectXipWithRevert:
        return "direct-xip-with-revert";
    case McubootMode::RamLoad:
        return "ram-load";
    case McubootMode::FirmwareLoader:
        return "firmware-loader";
    case McubootMode::SingleSlotRamLoad:
        return "single-slot-ram-load";
    case McubootMode::SwapUsingOffset:
        return "swap-using-offset";
    }
    return "unknown"; // LCOV_EXCL_LINE -- every enumerator is handled above
}

OsManagement::OsManagement(SmpClient& client) noexcept : client_{&client} {}

RequestHandle OsManagement::reset(const ResetOptions& options, Callback<void> on_done)
{
    std::array<std::byte, kRequestBufferSize> buffer{};
    cbor::Writer writer{MutBytes{buffer}};
    writer.open_map();
    if (options.force) {
        // Omitted entirely when false: that makes the ordinary request the
        // empty map the specification shows, and a boolean is what the server
        // actually decodes (docs/protocol-notes.md section 9, A15).
        writer.put_bool("force", true);
    }

    // Success carries an empty map. There is nothing to decode, and nothing to
    // check: the device accepted the request.
    return groups::send(*client_,
                        RequestSpec{.op = Operation::Write,
                                    .group = Group::Os,
                                    .command = command_id(OsCommand::Reset),
                                    .payload = {},
                                    .timeout = options.timeout},
                        writer.close_map().finish(), std::move(on_done), groups::decode_nothing,
                        kBufferTooSmall);
}

RequestHandle OsManagement::reset(Callback<void> on_done)
{
    return reset(ResetOptions{}, std::move(on_done));
}

RequestHandle OsManagement::mcumgr_parameters(Callback<McumgrParameters> on_done)
{
    std::array<std::byte, kRequestBufferSize> buffer{};
    // SmpError::NotSupported arrives as a failure like any other device error.
    // Recognising it and falling back is the caller's decision, not this
    // layer's (docs/protocol-notes.md section 9, A8).
    return groups::send(*client_,
                        RequestSpec{.op = Operation::Read,
                                    .group = Group::Os,
                                    .command = command_id(OsCommand::McumgrParameters),
                                    .payload = {},
                                    .timeout = {}},
                        groups::encode_empty(MutBytes{buffer}), std::move(on_done),
                        decode_parameters, kBufferTooSmall);
}

RequestHandle OsManagement::echo(std::string_view text, Callback<std::string> on_done)
{
    if (text.size() > limits::kMaxEchoLength) {
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::InvalidArgument, "os: echo string too long"});
    }

    std::array<std::byte, kRequestBufferSize> buffer{};
    cbor::Writer writer{MutBytes{buffer}};
    return groups::send(*client_,
                        RequestSpec{.op = Operation::Write,
                                    .group = Group::Os,
                                    .command = command_id(OsCommand::Echo),
                                    .payload = {},
                                    .timeout = {}},
                        writer.open_map().put_text("d", text).close_map().finish(),
                        std::move(on_done), decode_echo, kBufferTooSmall);
}

RequestHandle OsManagement::bootloader_name(Callback<std::string> on_done)
{
    std::array<std::byte, kRequestBufferSize> buffer{};
    return groups::send(*client_,
                        RequestSpec{.op = Operation::Read,
                                    .group = Group::Os,
                                    .command = command_id(OsCommand::BootloaderInfo),
                                    .payload = {},
                                    .timeout = {}},
                        groups::encode_empty(MutBytes{buffer}), std::move(on_done),
                        decode_bootloader_name, kBufferTooSmall);
}

RequestHandle OsManagement::bootloader_mode(Callback<BootloaderMode> on_done)
{
    std::array<std::byte, kRequestBufferSize> buffer{};
    cbor::Writer writer{MutBytes{buffer}};
    return groups::send(*client_,
                        RequestSpec{.op = Operation::Read,
                                    .group = Group::Os,
                                    .command = command_id(OsCommand::BootloaderInfo),
                                    .payload = {},
                                    .timeout = {}},
                        writer.open_map().put_text("query", "mode").close_map().finish(),
                        std::move(on_done), decode_bootloader_mode, kBufferTooSmall);
}

} // namespace smply
