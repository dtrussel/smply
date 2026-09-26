// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SUPPORT_DFU_PACKAGE_JSON_HPP
#define SMPLY_SUPPORT_DFU_PACKAGE_JSON_HPP

/// \file
/// A small, bounded JSON reader, for a DFU package's `manifest.json`.
///
/// The whole of RFC 8259 is accepted, so a manifest written by any tool reads,
/// but the reader is built for one small document: the input, the nesting, the
/// number of values and the length of every string are all bounded, and a
/// document over any bound is refused rather than read part-way. It is not a
/// general JSON library and is not meant to become one; no new dependency is
/// worth a manifest of a dozen keys (ADR-0021).

#include "smply/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace smply::dfu_package {

/// At most this many bytes of JSON.
inline constexpr std::size_t kMaxJsonSize = 64 * 1024;
/// At most this deep a nesting of arrays and objects.
inline constexpr std::size_t kMaxJsonDepth = 16;
/// At most this many values in one document.
inline constexpr std::size_t kMaxJsonValues = 4096;
/// At most this many bytes in one string or number, after unescaping.
inline constexpr std::size_t kMaxJsonString = 4096;

struct JsonMember;

/// One JSON value.
struct JsonValue
{
    enum class Kind : std::uint8_t
    {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object,
    };

    Kind kind = Kind::Null;
    bool boolean = false;
    /// A string's contents, unescaped, as UTF-8; or a number's text, verbatim.
    std::string text;
    std::vector<JsonValue> items;
    /// In document order. A key never appears twice: the reader refuses that.
    std::vector<JsonMember> members;

    /// The member called \p key of an object, or nullptr.
    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;

    /// A number that is a non-negative integer, if it fits.
    [[nodiscard]] std::optional<std::uint64_t> as_uint() const noexcept;
};

struct JsonMember
{
    std::string key;
    JsonValue value;
};

/// Parses one JSON document.
///
/// \return `MessageTooLarge` for a document over any of the bounds above, and
///         `MalformedMessage` for anything that is not JSON, or an object with
///         a key given twice.
[[nodiscard]] Result<JsonValue> parse_json(std::string_view text);

} // namespace smply::dfu_package

#endif // SMPLY_SUPPORT_DFU_PACKAGE_JSON_HPP
