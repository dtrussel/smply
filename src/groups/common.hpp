// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SRC_GROUPS_COMMON_HPP
#define SMPLY_SRC_GROUPS_COMMON_HPP

/// \file
/// What every management group does the same way (docs/design.md section 5).
///
/// A group command is always the same five steps: encode a small request into
/// a fixed buffer, build a `RequestSpec`, hand it to `SmpClient`, and when the
/// response arrives either pass on the failure or decode the payload. The
/// client has already done everything else: sequence numbers, deadlines, and
/// the `rc` and `err` checks. So a group is its encoders and decoders plus a
/// call to `send()`.
///
/// Each group sizes its own request buffer with a `static_assert` against the
/// largest request it can build. That is why an encode failure here is an
/// `Internal` error behind a guard that coverage excludes: it cannot happen
/// unless that sizing is broken, and then it must fail loudly rather than send
/// a truncated request.

#include "cbor/cbor.hpp"
#include "smply/bytes.hpp"
#include "smply/error.hpp"
#include "smply/result.hpp"
#include "smply/smp_client.hpp"

#include <cstdint>
#include <type_traits>
#include <utility>

namespace smply::groups {

/// The command ID of a group's command enumeration, as the SMP header carries
/// it.
template<class Command>
    requires std::is_enum_v<Command>
[[nodiscard]] constexpr std::uint8_t command_id(Command command) noexcept
{
    return static_cast<std::uint8_t>(command);
}

/// The empty CBOR map, `{}`, the body of every request with no fields.
///
/// The server does not require a map before dispatching, but each handler that
/// reads anything decodes one, so a well-formed empty map keeps a no-argument
/// request indistinguishable from any other.
[[nodiscard]] inline Result<ConstBytes> encode_empty(MutBytes buffer) noexcept
{
    cbor::Writer writer{buffer};
    return writer.open_map().close_map().finish();
}

/// Reports \p error to \p on_done on the next poll(), never inside this call
/// (ADR-0003), and returns the invalid handle a refused operation returns.
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

/// Opens a response's top-level map.
///
/// Every response is a map by the time a group sees it: `SmpClient` has already
/// run `extract_mgmt_error()` over the payload, which fails on anything else.
/// Checked anyway, because a decoder that assumes its input was validated
/// elsewhere is one refactor away from trusting a device.
[[nodiscard]] inline Result<void> enter_response(cbor::Reader& reader)
{
    // LCOV_EXCL_START -- unreachable guard, and the whole block is: marking
    // only the `if` leaves its body counted against the branch denominator,
    // which is what docs/quality-gates.md section 6 excludes it for.
    if (const auto entered = reader.enter_map(); !entered.has_value()) {
        return fail(entered.error());
    }
    // LCOV_EXCL_STOP
    return {};
}

/// Completes \p callback with the result of \p decode, or with the failure that
/// arrived instead of a response.
///
/// \p decode takes the response payload and returns a `Result<T>`.
template<class T, class Decode>
void complete(Callback<T>& callback, Result<RawResponse>& response, Decode& decode)
{
    if (!callback) {
        return;
    }
    if (!response.has_value()) {
        callback(fail(response.error()));
        return;
    }
    Result<T> decoded = decode(response->payload);
    if (!decoded.has_value()) {
        callback(fail(decoded.error()));
        return;
    }
    callback(std::move(decoded));
}

/// Sends one group command and decodes its answer.
///
/// \param spec    Everything but the payload.
/// \param payload The encoded request. It must stay valid for this call only:
///                `SmpClient::request()` copies it into the message.
/// \param decode  `Result<T>(ConstBytes payload)`, run on a successful
///                response.
/// \param where   The error text if the request did not fit its buffer.
template<class T, class Decode>
RequestHandle send(SmpClient& client, RequestSpec spec, const Result<ConstBytes>& payload,
                   Callback<T> on_done, Decode decode, const char* where)
{
    // LCOV_EXCL_START -- unreachable guard; see the file comment.
    if (!payload.has_value()) {
        return reject(client, std::move(on_done), Error{ErrorCode::Internal, where});
    }
    // LCOV_EXCL_STOP
    spec.payload = *payload;
    return client.request(spec, [callback = std::move(on_done),
                                 decode = std::move(decode)](Result<RawResponse> response) mutable {
        complete(callback, response, decode);
    });
}

/// The decoder for a command whose success carries nothing to read: an empty
/// map, or `{"rc": 0}` from a server with the legacy result-code behaviour,
/// which `SmpClient` has already read as success.
[[nodiscard]] inline Result<void> decode_nothing(ConstBytes /*payload*/) noexcept
{
    return {};
}

} // namespace smply::groups

#endif // SMPLY_SRC_GROUPS_COMMON_HPP
