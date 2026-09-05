// SPDX-License-Identifier: Apache-2.0

#include "smply/groups/os.hpp"

#include "cbor/cbor.hpp"
#include "smply/error.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace smply {
namespace {

/// OS-group command IDs (docs/protocol-notes.md section 5, S7).
enum class OsCommand : std::uint8_t
{
    Echo = 0,
    Reset = 5,
    McumgrParameters = 6,
};

/// Every request in this group is a small, flat map. The largest is echo's:
/// one map header, the one-character key "d", a text-string header of at most
/// three bytes, and `kMaxEchoLength` bytes of text.
constexpr std::size_t kLargestRequestEnvelope = 1 + 2 + 3;
constexpr std::size_t kRequestBufferSize = limits::kMaxEchoLength + 16;

// The buffer is sized from the same constant that bounds echo's input, and the
// other two requests are smaller still ({} is one byte, {"force": true} is
// eight). Encoding therefore cannot run out of room -- which is why the
// encode guards below report Internal rather than an ordinary failure.
static_assert(kRequestBufferSize >= limits::kMaxEchoLength + kLargestRequestEnvelope,
              "the request buffer must fit the largest legal echo string");

/// The empty CBOR map, `{}`.
///
/// Sent as the body of every request with no fields. The server does not
/// require a map before dispatching, but each handler that reads anything
/// decodes one, so sending a well-formed empty map is what keeps a
/// no-argument request indistinguishable from any other.
[[nodiscard]] Result<ConstBytes> encode_empty(MutBytes buffer) noexcept
{
    cbor::Writer writer{buffer};
    return writer.open_map().close_map().finish();
}

[[nodiscard]] std::uint8_t command_id(OsCommand command) noexcept
{
    return static_cast<std::uint8_t>(command);
}

/// Reports \p error to \p on_done on the next poll(), never inside this call.
template<class T>
RequestHandle reject(SmpClient& client, Callback<T> on_done, Error error)
{
    if (on_done) {
        client.defer([callback = std::move(on_done), failure = std::move(error)]() mutable {
            callback(fail(std::move(failure)));
        });
    }
    return {};
}

} // namespace

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
    const auto payload = writer.close_map().finish();
    if (!payload.has_value()) {
        // Unreachable: see the static_assert on kRequestBufferSize. Kept as a
        // guard rather than deleted, so a future change to the sizing fails
        // loudly instead of sending a truncated request.
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::Internal, "os: request buffer too small"});
    }

    const RequestSpec spec{.op = Operation::Write,
                           .group = Group::Os,
                           .command = command_id(OsCommand::Reset),
                           .payload = *payload,
                           .timeout = options.timeout};

    return client_->request(spec, [callback = std::move(on_done)](Result<RawResponse> response) {
        if (!callback) {
            return;
        }
        if (!response.has_value()) {
            callback(fail(response.error()));
            return;
        }
        // Success carries an empty map. There is nothing to decode, and
        // nothing to check: the device accepted the request.
        callback({});
    });
}

RequestHandle OsManagement::reset(Callback<void> on_done)
{
    return reset(ResetOptions{}, std::move(on_done));
}

RequestHandle OsManagement::mcumgr_parameters(Callback<McumgrParameters> on_done)
{
    std::array<std::byte, kRequestBufferSize> buffer{};
    const auto payload = encode_empty(MutBytes{buffer});
    if (!payload.has_value()) {
        // Unreachable: see the static_assert on kRequestBufferSize. Kept as a
        // guard rather than deleted, so a future change to the sizing fails
        // loudly instead of sending a truncated request.
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::Internal, "os: request buffer too small"});
    }

    const RequestSpec spec{.op = Operation::Read,
                           .group = Group::Os,
                           .command = command_id(OsCommand::McumgrParameters),
                           .payload = *payload,
                           .timeout = {}};

    return client_->request(spec, [callback = std::move(on_done)](Result<RawResponse> response) {
        if (!callback) {
            return;
        }
        if (!response.has_value()) {
            // SmpError::NotSupported arrives here like any other device error.
            // Recognising it and falling back is the caller's decision, not
            // this layer's.
            callback(fail(response.error()));
            return;
        }

        cbor::Reader reader{response->payload};
        if (const auto entered = reader.enter_map(); !entered.has_value()) {
            // Unreachable today: SmpClient::interpret() has already run
            // extract_mgmt_error() over this payload, which fails unless it is
            // a map. Checked anyway -- a decoder that assumes its input was
            // validated elsewhere is one refactor away from trusting a device.
            callback(fail(entered.error()));
            return;
        }
        const std::optional<std::uint64_t> buf_size = reader.uint("buf_size");
        const std::optional<std::uint64_t> buf_count = reader.uint("buf_count");
        static_cast<void>(reader.leave_map());

        // Checked before the values are trusted: a wrong-typed field poisons
        // the reader and leaves both looking merely absent (P5's rule).
        if (const auto status = reader.status(); !status.has_value()) {
            callback(fail(status.error()));
            return;
        }
        if (!buf_size.has_value() || !buf_count.has_value()) {
            callback(fail(Error{ErrorCode::CborDecode, "os: parameters incomplete"}));
            return;
        }
        if (*buf_size > std::numeric_limits<std::uint32_t>::max() ||
            *buf_count > std::numeric_limits<std::uint32_t>::max()) {
            callback(fail(Error{ErrorCode::CborDecode, "os: parameters out of range"}));
            return;
        }

        callback(McumgrParameters{.buf_size = static_cast<std::uint32_t>(*buf_size),
                                  .buf_count = static_cast<std::uint32_t>(*buf_count)});
    });
}

RequestHandle OsManagement::echo(std::string_view text, Callback<std::string> on_done)
{
    if (text.size() > limits::kMaxEchoLength) {
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::InvalidArgument, "os: echo string too long"});
    }

    std::array<std::byte, kRequestBufferSize> buffer{};
    cbor::Writer writer{MutBytes{buffer}};
    const auto payload = writer.open_map().put_text("d", text).close_map().finish();
    if (!payload.has_value()) {
        // Unreachable: see the static_assert on kRequestBufferSize. Kept as a
        // guard rather than deleted, so a future change to the sizing fails
        // loudly instead of sending a truncated request.
        return reject(*client_, std::move(on_done),
                      Error{ErrorCode::Internal, "os: request buffer too small"});
    }

    const RequestSpec spec{.op = Operation::Write,
                           .group = Group::Os,
                           .command = command_id(OsCommand::Echo),
                           .payload = *payload,
                           .timeout = {}};

    return client_->request(spec, [callback = std::move(on_done)](Result<RawResponse> response) {
        if (!callback) {
            return;
        }
        if (!response.has_value()) {
            callback(fail(response.error()));
            return;
        }

        cbor::Reader reader{response->payload};
        if (const auto entered = reader.enter_map(); !entered.has_value()) {
            // Unreachable today: SmpClient::interpret() has already run
            // extract_mgmt_error() over this payload, which fails unless it is
            // a map. Checked anyway -- a decoder that assumes its input was
            // validated elsewhere is one refactor away from trusting a device.
            callback(fail(entered.error()));
            return;
        }
        const std::optional<std::string_view> echoed = reader.text("r");
        static_cast<void>(reader.leave_map());

        if (const auto status = reader.status(); !status.has_value()) {
            callback(fail(status.error()));
            return;
        }
        if (!echoed.has_value()) {
            callback(fail(Error{ErrorCode::CborDecode, "os: echo reply has no text"}));
            return;
        }
        if (echoed->size() > limits::kMaxEchoLength) {
            // Bounded before the copy. A device cannot make smply allocate on
            // a size it chose, and a reply longer than the request is not an
            // answer to it.
            callback(fail(Error{ErrorCode::CborDecode, "os: echo reply too long"}));
            return;
        }

        // The view points into the assembler's buffer, which is valid only for
        // this callback. The copy is what the caller keeps.
        callback(std::string{*echoed});
    });
}

} // namespace smply
