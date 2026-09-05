// SPDX-License-Identifier: Apache-2.0

#include "cbor/cbor.hpp"

#include "smply/error.hpp"

#include <cstring>

namespace smply::cbor {

Reader::Reader(ConstBytes input, unsigned max_nesting) noexcept : max_nesting_{max_nesting}
{
    QCBORDecode_Init(&context_, UsefulBufC{input.data(), input.size()}, QCBOR_DECODE_MODE_NORMAL);
}

const char* Reader::label(std::string_view key) noexcept
{
    if (key.size() > kMaxKeyLength) {
        static_cast<void>(record(Error{ErrorCode::CborDecode, "cbor reader: key too long"}));
        return nullptr;
    }
    std::memcpy(label_, key.data(), key.size());
    label_[key.size()] = '\0';
    return static_cast<const char*>(label_);
}

unexpected<Error> Reader::record(Error error) noexcept
{
    // First failure wins: later ones are usually consequences of it, and the
    // first is the one that explains what actually happened.
    if (!error_.has_value()) {
        error_ = std::move(error);
    }
    return fail(error_.value());
}

bool Reader::consume_lookup() noexcept
{
    const QCBORError status = QCBORDecode_GetAndResetError(&context_);
    if (status == QCBOR_SUCCESS) {
        return true;
    }
    if (status == QCBOR_ERR_LABEL_NOT_FOUND) {
        // Absence is normal: MCUmgr omits a field rather than sending false or
        // zero. Reported to the caller as nullopt, not as a failure.
        return false;
    }
    // Anything else -- wrong type, malformed document, truncated input -- is a
    // real decode failure and must not be mistaken for an absent field.
    static_cast<void>(record(Error{ErrorCode::CborDecode, "cbor reader: decode failed"}));
    return false;
}

Result<void> Reader::enter_map() noexcept
{
    if (error_.has_value()) {
        return fail(error_.value());
    }
    if (depth_ >= max_nesting_) {
        return record(Error{ErrorCode::CborDecode, "cbor reader: nesting limit"});
    }

    QCBORDecode_EnterMap(&context_, nullptr);
    if (QCBORDecode_GetError(&context_) != QCBOR_SUCCESS) {
        return record(Error{ErrorCode::CborDecode, "cbor reader: not a map"});
    }
    ++depth_;
    return {};
}

Result<void> Reader::enter_map(std::string_view key) noexcept
{
    if (error_.has_value()) {
        return fail(error_.value());
    }
    if (depth_ >= max_nesting_) {
        return record(Error{ErrorCode::CborDecode, "cbor reader: nesting limit"});
    }

    const char* name = label(key);
    if (name == nullptr) {
        return record(Error{ErrorCode::CborDecode, "cbor reader: key too long"});
    }

    QCBORDecode_EnterMapFromMapSZ(&context_, name);
    if (QCBORDecode_GetError(&context_) != QCBOR_SUCCESS) {
        // Distinguishing "absent" from "present but not a map" here would need
        // a peek; callers that care check for the key first.
        QCBORDecode_GetAndResetError(&context_);
        return fail(Error{ErrorCode::CborDecode, "cbor reader: no such map"});
    }
    ++depth_;
    return {};
}

Result<void> Reader::leave_map() noexcept
{
    if (depth_ == 0) {
        return record(Error{ErrorCode::CborDecode, "cbor reader: unbalanced leave_map"});
    }
    --depth_;
    QCBORDecode_ExitMap(&context_);
    if (QCBORDecode_GetError(&context_) != QCBOR_SUCCESS) {
        return record(Error{ErrorCode::CborDecode, "cbor reader: exit map failed"});
    }
    return {};
}

std::optional<std::uint64_t> Reader::uint(std::string_view key) noexcept
{
    if (error_.has_value()) {
        return std::nullopt;
    }
    const char* name = label(key);
    if (name == nullptr) {
        return std::nullopt;
    }

    std::uint64_t value = 0;
    QCBORDecode_GetUInt64InMapSZ(&context_, name, &value);
    return consume_lookup() ? std::optional{value} : std::nullopt;
}

std::optional<std::int64_t> Reader::integer(std::string_view key) noexcept
{
    if (error_.has_value()) {
        return std::nullopt;
    }
    const char* name = label(key);
    if (name == nullptr) {
        return std::nullopt;
    }

    std::int64_t value = 0;
    QCBORDecode_GetInt64InMapSZ(&context_, name, &value);
    return consume_lookup() ? std::optional{value} : std::nullopt;
}

std::optional<bool> Reader::boolean(std::string_view key) noexcept
{
    if (error_.has_value()) {
        return std::nullopt;
    }
    const char* name = label(key);
    if (name == nullptr) {
        return std::nullopt;
    }

    bool value = false;
    QCBORDecode_GetBoolInMapSZ(&context_, name, &value);
    return consume_lookup() ? std::optional{value} : std::nullopt;
}

std::optional<std::string_view> Reader::text(std::string_view key) noexcept
{
    if (error_.has_value()) {
        return std::nullopt;
    }
    const char* name = label(key);
    if (name == nullptr) {
        return std::nullopt;
    }

    UsefulBufC value{};
    QCBORDecode_GetTextStringInMapSZ(&context_, name, &value);
    if (!consume_lookup()) {
        return std::nullopt;
    }
    // Points into the caller's input buffer: no copy, and no ownership.
    return std::string_view{static_cast<const char*>(value.ptr), value.len};
}

std::optional<ConstBytes> Reader::bytes(std::string_view key) noexcept
{
    if (error_.has_value()) {
        return std::nullopt;
    }
    const char* name = label(key);
    if (name == nullptr) {
        return std::nullopt;
    }

    UsefulBufC value{};
    QCBORDecode_GetByteStringInMapSZ(&context_, name, &value);
    if (!consume_lookup()) {
        return std::nullopt;
    }
    return ConstBytes{static_cast<const std::byte*>(value.ptr), value.len};
}

Result<void>
Reader::for_each_map_in_array(std::string_view key, std::size_t max_elements,
                              const std::function<Result<void>(Reader&)>& visit) noexcept
{
    if (error_.has_value()) {
        return fail(error_.value());
    }
    if (depth_ >= max_nesting_) {
        return record(Error{ErrorCode::CborDecode, "cbor reader: nesting limit"});
    }

    const char* name = label(key);
    if (name == nullptr) {
        return record(Error{ErrorCode::CborDecode, "cbor reader: key too long"});
    }

    QCBORDecode_EnterArrayFromMapSZ(&context_, name);
    if (const QCBORError status = QCBORDecode_GetAndResetError(&context_);
        status != QCBOR_SUCCESS) {
        if (status == QCBOR_ERR_LABEL_NOT_FOUND) {
            // An absent array is an empty one. MCUmgr omits "images" entirely
            // when a device has no valid image to report, which is a normal
            // state after erasing a slot, not an error.
            return {};
        }
        return record(Error{ErrorCode::CborDecode, "cbor reader: not an array"});
    }
    ++depth_;

    Result<void> outcome{};
    std::size_t seen = 0;
    while (true) {
        if (seen == max_elements) {
            // Bounded by configuration, not by what the device claims.
            outcome = record(Error{ErrorCode::CborDecode, "cbor reader: too many array elements"});
            break;
        }

        QCBORDecode_EnterMap(&context_, nullptr);
        if (const QCBORError status = QCBORDecode_GetAndResetError(&context_);
            status != QCBOR_SUCCESS) {
            if (status != QCBOR_ERR_NO_MORE_ITEMS) {
                outcome =
                    record(Error{ErrorCode::CborDecode, "cbor reader: array element not a map"});
            }
            break; // end of the array
        }
        ++seen;

        outcome = visit(*this);

        QCBORDecode_ExitMap(&context_);
        if (!outcome.has_value()) {
            break;
        }
        if (QCBORDecode_GetError(&context_) != QCBOR_SUCCESS) {
            outcome =
                record(Error{ErrorCode::CborDecode, "cbor reader: exit array element failed"});
            break;
        }
    }

    --depth_;
    QCBORDecode_ExitArray(&context_);
    QCBORDecode_GetAndResetError(&context_);
    return outcome;
}

Result<void> Reader::status() const noexcept
{
    if (error_.has_value()) {
        return fail(error_.value());
    }
    return {};
}

} // namespace smply::cbor
