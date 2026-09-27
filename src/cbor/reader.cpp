// SPDX-License-Identifier: Apache-2.0

#include "cbor/cbor.hpp"

#include "smply/error.hpp"

#include <cstring>

namespace smply::cbor {

Reader::Reader(ConstBytes input, unsigned max_nesting) noexcept
    : input_{input}, max_nesting_{max_nesting}
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
        // fail(), not record(): this is the one non-sticky failure on the
        // class, because the call doubles as a probe for an optional map. See
        // the declaration in cbor.hpp for why, and why the two guards above it
        // are sticky when this is not.
        //
        // Distinguishing "absent" from "present but not a map" here would need
        // a peek; callers that care check for the key first.
        QCBORDecode_GetAndResetError(&context_);
        return fail(ErrorCode::CborDecode, "cbor reader: no such map");
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

    // Each element is decoded by a child reader over the element's own bytes,
    // never by entering and exiting it in this context. That is a workaround,
    // and a load-bearing one: QCBOR (1.6.1 and current master alike) mishandles
    // consecutive indefinite-length breaks -- an indefinite-length map that is
    // the last element of an indefinite-length array -- when the map is left
    // through QCBORDecode_ExitMap(). It swallows the array's break as well, so
    // the walk either fails with QCBOR_ERR_BAD_BREAK or, worse, silently reads
    // the *parent map's* following entries as further array elements. Zephyr's
    // zcbor emits exactly that encoding unless CONFIG_ZCBOR_CANONICAL is set,
    // which the reference server does not (protocol-notes section 9, A18).
    // Peeking, skipping with VGetNextConsume() and re-reading the sub-span is
    // unaffected; tests/unit/test_cbor.cpp pins both shapes.
    Result<void> outcome{};
    std::size_t seen = 0;
    while (true) {
        QCBORItem item{};
        const QCBORError peeked = QCBORDecode_PeekNext(&context_, &item);
        if (peeked == QCBOR_ERR_NO_MORE_ITEMS) {
            break; // end of the array
        }
        if (peeked != QCBOR_SUCCESS) {
            outcome = record(Error{ErrorCode::CborDecode, "cbor reader: array element unreadable"});
            break;
        }
        if (item.uDataType != QCBOR_TYPE_MAP) {
            outcome = record(Error{ErrorCode::CborDecode, "cbor reader: array element not a map"});
            break;
        }
        if (seen == max_elements) {
            // Bounded by configuration, not by what the device claims. The cap
            // is tested once a further element has been *seen*, so an array of
            // exactly max_elements is accepted: checking before the peek would
            // reject the last legal element, having never looked for the end.
            outcome = record(Error{ErrorCode::CborDecode, "cbor reader: too many array elements"});
            break;
        }
        ++seen;

        const std::size_t first = QCBORDecode_Tell(&context_);
        QCBORDecode_VGetNextConsume(&context_, &item);
        if (const QCBORError status = QCBORDecode_GetAndResetError(&context_);
            status != QCBOR_SUCCESS) {
            outcome = record(Error{ErrorCode::CborDecode, "cbor reader: array element malformed"});
            break;
        }
        const std::size_t last = QCBORDecode_Tell(&context_);
        if (first > last || last > input_.size()) {
            outcome =
                record(Error{ErrorCode::CborDecode, "cbor reader: array element out of bounds"});
            break;
        }

        // The span may run past the element's own break when the QCBOR defect
        // above has swallowed the parent's; the child only ever looks inside
        // the map it enters, so trailing bytes are never examined. The nesting
        // budget is what remains of this reader's, so the total stays bounded.
        Reader element{input_.subspan(first, last - first), max_nesting_ - depth_};
        outcome = element.enter_map();
        if (outcome.has_value()) {
            outcome = visit(element);
        }
        // A poisoned child is a poisoned parent: whoever checks status() on this
        // reader afterwards must see the failure. `status()` returns by value,
        // so the Result is held in a named local -- binding a reference to the
        // Error inside a temporary dangles (GCC's -Wdangling-reference, and a
        // real use-after-free ASan catches). Rebuilt from the code and the
        // static call-site tag rather than copied, because this function is
        // noexcept and copying an Error copies a std::string.
        if (const Result<void> child = element.status();
            outcome.has_value() && !child.has_value()) {
            outcome = record(Error{child.error().code(), child.error().where()});
        }
        if (!outcome.has_value()) {
            break;
        }
    }

    --depth_;
    QCBORDecode_ExitArray(&context_);
    QCBORDecode_GetAndResetError(&context_);
    return outcome;
}

Result<bool>
Reader::for_each_text_in_array(std::string_view key, std::size_t max_elements,
                               const std::function<Result<void>(std::string_view)>& visit) noexcept
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
            return false;
        }
        return record(Error{ErrorCode::CborDecode, "cbor reader: not an array"});
    }
    ++depth_;

    // Every element is a scalar, so nothing here is entered or exited: the
    // consecutive-break defect for_each_map_in_array works around cannot arise
    // inside the array, and leaving the array itself is the ExitArray() path
    // that function already relies on.
    Result<void> outcome{};
    std::size_t seen = 0;
    while (true) {
        QCBORItem item{};
        const QCBORError peeked = QCBORDecode_PeekNext(&context_, &item);
        if (peeked == QCBOR_ERR_NO_MORE_ITEMS) {
            break;
        }
        if (peeked != QCBOR_SUCCESS) {
            outcome = record(Error{ErrorCode::CborDecode, "cbor reader: array element unreadable"});
            break;
        }
        if (item.uDataType != QCBOR_TYPE_TEXT_STRING) {
            outcome = record(Error{ErrorCode::CborDecode, "cbor reader: array element not text"});
            break;
        }
        if (seen == max_elements) {
            outcome = record(Error{ErrorCode::CborDecode, "cbor reader: too many array elements"});
            break;
        }
        ++seen;

        QCBORDecode_VGetNext(&context_, &item);
        // LCOV_EXCL_START -- unreachable guard: the peek above decoded this
        // very item successfully, so consuming it cannot fail.
        if (QCBORDecode_GetAndResetError(&context_) != QCBOR_SUCCESS) {
            outcome = record(Error{ErrorCode::CborDecode, "cbor reader: array element malformed"});
            break;
        }
        // LCOV_EXCL_STOP
        // Points into the caller's input buffer: no copy, and no ownership.
        outcome = visit(
            std::string_view{static_cast<const char*>(item.val.string.ptr), item.val.string.len});
        if (!outcome.has_value()) {
            break;
        }
    }

    --depth_;
    QCBORDecode_ExitArray(&context_);
    QCBORDecode_GetAndResetError(&context_);
    if (!outcome.has_value()) {
        return fail(outcome.error());
    }
    return true;
}

Result<bool> Reader::for_each_uint_in_map(
    std::string_view key, std::size_t max_entries,
    const std::function<Result<void>(std::string_view, std::uint64_t)>& visit) noexcept
{
    if (error_.has_value()) {
        return fail(error_.value());
    }
    if (depth_ >= max_nesting_) {
        return record(Error{ErrorCode::CborDecode, "cbor reader: nesting limit"});
    }
    if (key.size() > kMaxKeyLength) {
        return record(Error{ErrorCode::CborDecode, "cbor reader: key too long"});
    }

    // Find the value's byte range by walking the current map from its start,
    // rather than by entering the value with EnterMapFromMapSZ(). Entering is
    // the easy part; leaving is not. The map is usually the last entry of the
    // response, so an indefinite-length encoding -- which is what a Zephyr
    // device sends (protocol-notes section 9, A18) -- closes it with two
    // consecutive breaks, and QCBOR's ExitMap() mishandles exactly that. The
    // walk peeks, measures each entry with Tell() around VGetNextConsume(),
    // and hands the matching one to a child reader, as for_each_map_in_array
    // does with array elements. Lookups by key afterwards are unaffected:
    // QCBOR searches a bounded map from its start, wherever the cursor is.
    QCBORDecode_Rewind(&context_);
    std::size_t first = 0;
    std::size_t last = 0;
    bool found = false;
    while (!found) {
        QCBORItem item{};
        const QCBORError peeked = QCBORDecode_PeekNext(&context_, &item);
        if (peeked == QCBOR_ERR_NO_MORE_ITEMS) {
            break;
        }
        if (peeked != QCBOR_SUCCESS) {
            return record(Error{ErrorCode::CborDecode, "cbor reader: map entry unreadable"});
        }
        found = item.uLabelType == QCBOR_TYPE_TEXT_STRING && item.label.string.len == key.size() &&
                std::memcmp(item.label.string.ptr, key.data(), key.size()) == 0;
        if (found && item.uDataType != QCBOR_TYPE_MAP) {
            QCBORDecode_Rewind(&context_);
            return record(Error{ErrorCode::CborDecode, "cbor reader: not a map"});
        }
        first = QCBORDecode_Tell(&context_);
        QCBORDecode_VGetNextConsume(&context_, &item);
        if (QCBORDecode_GetAndResetError(&context_) != QCBOR_SUCCESS) {
            return record(Error{ErrorCode::CborDecode, "cbor reader: map entry malformed"});
        }
        last = QCBORDecode_Tell(&context_);
    }
    // Leaves the cursor where a caller that has not walked would expect it.
    QCBORDecode_Rewind(&context_);
    QCBORDecode_GetAndResetError(&context_);
    if (!found) {
        return false;
    }
    if (first > last || last > input_.size()) {
        return record(Error{ErrorCode::CborDecode, "cbor reader: map entry out of bounds"});
    }

    // The range holds `label, map`. A CBOR text label is itself a complete
    // item, so the child reads it as a one-item sequence: skip the label with
    // GetNext(), then enter the map that follows. The span may run past the
    // map's own break if QCBOR has swallowed the parent's; the child only ever
    // looks inside the map it enters.
    Reader child{input_.subspan(first, last - first), max_nesting_ - depth_};
    Result<void> outcome = child.enter_labelled_map();
    if (outcome.has_value()) {
        outcome = child.visit_uint_entries(max_entries, visit);
    }
    // A poisoned child is a poisoned parent, whether the child failed in its own
    // walk or inside the visitor. Rebuilt from the code and the static tag, as
    // in for_each_map_in_array, because copying an Error copies a string.
    if (const Result<void> status = child.status(); !status.has_value()) {
        return record(Error{status.error().code(), status.error().where()});
    }
    if (!outcome.has_value()) {
        // A visitor's own failure propagates without poisoning this reader,
        // exactly as it does from for_each_map_in_array.
        return fail(outcome.error());
    }
    return true;
}

Result<void> Reader::enter_labelled_map() noexcept
{
    // At the top level of a child's range the label is an item of its own: the
    // range is the two-item sequence `label, map`.
    QCBORItem label{};
    QCBORDecode_VGetNext(&context_, &label);
    // LCOV_EXCL_START -- unreachable guard: the parent peeked this entry and
    // matched its text label before handing over the range.
    if (QCBORDecode_GetAndResetError(&context_) != QCBOR_SUCCESS ||
        label.uDataType != QCBOR_TYPE_TEXT_STRING) {
        return record(Error{ErrorCode::CborDecode, "cbor reader: map entry malformed"});
    }
    // LCOV_EXCL_STOP
    return enter_map();
}

Result<void> Reader::visit_uint_entries(
    std::size_t max_entries,
    const std::function<Result<void>(std::string_view, std::uint64_t)>& visit) noexcept
{
    std::size_t seen = 0;
    while (true) {
        QCBORItem item{};
        const QCBORError peeked = QCBORDecode_PeekNext(&context_, &item);
        if (peeked == QCBOR_ERR_NO_MORE_ITEMS) {
            return {};
        }
        if (peeked != QCBOR_SUCCESS) {
            return record(Error{ErrorCode::CborDecode, "cbor reader: map entry unreadable"});
        }
        if (item.uLabelType != QCBOR_TYPE_TEXT_STRING) {
            return record(Error{ErrorCode::CborDecode, "cbor reader: map key not text"});
        }
        std::uint64_t value = 0;
        if (item.uDataType == QCBOR_TYPE_INT64 && item.val.int64 >= 0) {
            value = static_cast<std::uint64_t>(item.val.int64);
        } else if (item.uDataType == QCBOR_TYPE_UINT64) {
            value = item.val.uint64;
        } else {
            // A negative number, a string, or a nested container -- the last
            // being what a Zephyr server writes if a walk fails after the map
            // was opened (protocol-notes section 10).
            return record(Error{ErrorCode::CborDecode, "cbor reader: map value not unsigned"});
        }
        if (seen == max_entries) {
            return record(Error{ErrorCode::CborDecode, "cbor reader: too many map entries"});
        }
        ++seen;

        QCBORDecode_VGetNext(&context_, &item);
        // LCOV_EXCL_START -- unreachable guard: the peek decoded this item.
        if (QCBORDecode_GetAndResetError(&context_) != QCBOR_SUCCESS) {
            return record(Error{ErrorCode::CborDecode, "cbor reader: map entry malformed"});
        }
        // LCOV_EXCL_STOP
        if (auto visited = visit(std::string_view{static_cast<const char*>(item.label.string.ptr),
                                                  item.label.string.len},
                                 value);
            !visited.has_value()) {
            return visited;
        }
    }
}

Result<void> Reader::status() const noexcept
{
    if (error_.has_value()) {
        return fail(error_.value());
    }
    return {};
}

} // namespace smply::cbor
