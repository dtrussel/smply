// SPDX-License-Identifier: Apache-2.0

#include "cbor/cbor.hpp"

#include "smply/error.hpp"

#include <cstring>

namespace smply::cbor {
namespace {

[[nodiscard]] UsefulBufC to_useful(ConstBytes bytes) noexcept
{
    return UsefulBufC{bytes.data(), bytes.size()};
}

[[nodiscard]] UsefulBufC to_useful(std::string_view text) noexcept
{
    return UsefulBufC{text.data(), text.size()};
}

} // namespace

Writer::Writer(MutBytes out, unsigned max_nesting) noexcept : max_nesting_{max_nesting}
{
    QCBOREncode_Init(&context_, UsefulBuf{out.data(), out.size()});
}

const char* Writer::label(std::string_view key) noexcept
{
    // QCBOR's map API takes a C string, so the key has to be null-terminated.
    // Copying into a fixed buffer avoids assuming a string_view is terminated,
    // which is the sort of assumption that works until one call site passes a
    // substring.
    if (key.size() > kMaxKeyLength) {
        failed_ = true;
        return nullptr;
    }
    std::memcpy(label_, key.data(), key.size());
    label_[key.size()] = '\0';
    return static_cast<const char*>(label_);
}

Writer& Writer::open_map() noexcept
{
    if (failed_) {
        return *this;
    }
    if (depth_ >= max_nesting_) {
        failed_ = true;
        return *this;
    }
    ++depth_;
    QCBOREncode_OpenMap(&context_);
    return *this;
}

Writer& Writer::close_map() noexcept
{
    if (failed_ || depth_ == 0) {
        return *this;
    }
    --depth_;
    QCBOREncode_CloseMap(&context_);
    return *this;
}

Writer& Writer::put_uint(std::string_view key, std::uint64_t value) noexcept
{
    if (const char* name = label(key); name != nullptr) {
        QCBOREncode_AddUInt64ToMap(&context_, name, value);
    }
    return *this;
}

Writer& Writer::put_int(std::string_view key, std::int64_t value) noexcept
{
    if (const char* name = label(key); name != nullptr) {
        QCBOREncode_AddInt64ToMap(&context_, name, value);
    }
    return *this;
}

Writer& Writer::put_bool(std::string_view key, bool value) noexcept
{
    if (const char* name = label(key); name != nullptr) {
        QCBOREncode_AddBoolToMap(&context_, name, value);
    }
    return *this;
}

Writer& Writer::put_text(std::string_view key, std::string_view value) noexcept
{
    if (const char* name = label(key); name != nullptr) {
        QCBOREncode_AddTextToMap(&context_, name, to_useful(value));
    }
    return *this;
}

Writer& Writer::put_bytes(std::string_view key, ConstBytes value) noexcept
{
    if (const char* name = label(key); name != nullptr) {
        QCBOREncode_AddBytesToMap(&context_, name, to_useful(value));
    }
    return *this;
}

Result<ConstBytes> Writer::finish() noexcept
{
    if (failed_) {
        return fail(ErrorCode::CborEncode, "cbor writer: key too long or nesting exceeded");
    }

    UsefulBufC encoded{};
    const QCBORError status = QCBOREncode_Finish(&context_, &encoded);
    if (status != QCBOR_SUCCESS) {
        // Overwhelmingly the buffer being too small, which is a real
        // possibility for an upload chunk sized against a tight device budget.
        return fail(ErrorCode::CborEncode, "cbor writer: encoding failed");
    }

    return ConstBytes{static_cast<const std::byte*>(encoded.ptr), encoded.len};
}

} // namespace smply::cbor
