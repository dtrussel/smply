// SPDX-License-Identifier: Apache-2.0

#include "dfu_package/json.hpp"

#include "smply/error.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace smply::dfu_package {
namespace {

[[nodiscard]] unexpected<Error> malformed(const char* what) noexcept
{
    return fail(ErrorCode::MalformedMessage, what);
}

[[nodiscard]] unexpected<Error> too_large(const char* what) noexcept
{
    return fail(ErrorCode::MessageTooLarge, what);
}

[[nodiscard]] bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

/// Appends \p code_point as UTF-8.
void append_utf8(std::string& out, std::uint32_t code_point)
{
    const auto byte = [&out](std::uint32_t value) { out.push_back(static_cast<char>(value)); };
    if (code_point < 0x80U) {
        byte(code_point);
    } else if (code_point < 0x800U) {
        byte(0xC0U | (code_point >> 6U));
        byte(0x80U | (code_point & 0x3FU));
    } else if (code_point < 0x10000U) {
        byte(0xE0U | (code_point >> 12U));
        byte(0x80U | ((code_point >> 6U) & 0x3FU));
        byte(0x80U | (code_point & 0x3FU));
    } else {
        byte(0xF0U | (code_point >> 18U));
        byte(0x80U | ((code_point >> 12U) & 0x3FU));
        byte(0x80U | ((code_point >> 6U) & 0x3FU));
        byte(0x80U | (code_point & 0x3FU));
    }
}

/// Recursive descent over the text, with every bound checked as it goes.
class Parser
{
public:
    explicit Parser(std::string_view text) noexcept : text_{text} {}

    [[nodiscard]] Result<JsonValue> document()
    {
        Result<JsonValue> value = parse_value(0);
        if (!value.has_value()) {
            return value;
        }
        skip_space();
        if (at_ != text_.size()) {
            return malformed("json: text after the document");
        }
        return value;
    }

private:
    [[nodiscard]] bool done() const noexcept
    {
        return at_ >= text_.size();
    }

    [[nodiscard]] char peek() const noexcept
    {
        return text_[at_];
    }

    void skip_space() noexcept
    {
        while (!done() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
            ++at_;
        }
    }

    /// Consumes \p word if the text continues with it.
    [[nodiscard]] bool literal(std::string_view word) noexcept
    {
        if (text_.substr(at_, word.size()) != word) {
            return false;
        }
        at_ += word.size();
        return true;
    }

    // The three below recurse through each other, one level per nested array or
    // object, and kMaxJsonDepth bounds that before each level is entered.
    // NOLINTNEXTLINE(misc-no-recursion): bounded by kMaxJsonDepth; see above.
    [[nodiscard]] Result<JsonValue> parse_value(std::size_t depth)
    {
        if (++values_ > kMaxJsonValues) {
            return too_large("json: too many values");
        }
        skip_space();
        if (done()) {
            return malformed("json: a value was expected");
        }
        JsonValue value;
        switch (peek()) {
        case '{':
            return parse_object(depth);
        case '[':
            return parse_array(depth);
        case '"': {
            value.kind = JsonValue::Kind::String;
            Result<std::string> text = parse_string();
            if (!text.has_value()) {
                return fail(text.error());
            }
            value.text = std::move(*text);
            return value;
        }
        default:
            break;
        }
        if (literal("true")) {
            value.kind = JsonValue::Kind::Boolean;
            value.boolean = true;
            return value;
        }
        if (literal("false")) {
            value.kind = JsonValue::Kind::Boolean;
            return value;
        }
        if (literal("null")) {
            return value;
        }
        return parse_number();
    }

    // NOLINTNEXTLINE(misc-no-recursion): see parse_value above.
    [[nodiscard]] Result<JsonValue> parse_object(std::size_t depth)
    {
        if (depth == kMaxJsonDepth) {
            return too_large("json: nested too deeply");
        }
        ++at_; // '{'
        JsonValue object;
        object.kind = JsonValue::Kind::Object;
        skip_space();
        if (!done() && peek() == '}') {
            ++at_;
            return object;
        }
        for (;;) {
            skip_space();
            if (done() || peek() != '"') {
                return malformed("json: an object key was expected");
            }
            Result<std::string> key = parse_string();
            if (!key.has_value()) {
                return fail(key.error());
            }
            if (object.find(*key) != nullptr) {
                return malformed("json: a key appears twice in one object");
            }
            skip_space();
            if (done() || peek() != ':') {
                return malformed("json: ':' was expected");
            }
            ++at_;
            Result<JsonValue> value = parse_value(depth + 1);
            if (!value.has_value()) {
                return value;
            }
            object.members.push_back(JsonMember{std::move(*key), std::move(*value)});
            skip_space();
            if (done()) {
                return malformed("json: unterminated object");
            }
            if (peek() == '}') {
                ++at_;
                return object;
            }
            if (peek() != ',') {
                return malformed("json: ',' or '}' was expected");
            }
            ++at_;
        }
    }

    // NOLINTNEXTLINE(misc-no-recursion): see parse_value above.
    [[nodiscard]] Result<JsonValue> parse_array(std::size_t depth)
    {
        if (depth == kMaxJsonDepth) {
            return too_large("json: nested too deeply");
        }
        ++at_; // '['
        JsonValue array;
        array.kind = JsonValue::Kind::Array;
        skip_space();
        if (!done() && peek() == ']') {
            ++at_;
            return array;
        }
        for (;;) {
            Result<JsonValue> value = parse_value(depth + 1);
            if (!value.has_value()) {
                return value;
            }
            array.items.push_back(std::move(*value));
            skip_space();
            if (done()) {
                return malformed("json: unterminated array");
            }
            if (peek() == ']') {
                ++at_;
                return array;
            }
            if (peek() != ',') {
                return malformed("json: ',' or ']' was expected");
            }
            ++at_;
        }
    }

    /// Four hex digits of a `\u` escape.
    [[nodiscard]] std::optional<std::uint32_t> hex4()
    {
        if (text_.size() - at_ < 4) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[at_++];
            std::uint32_t digit = 0;
            if (is_digit(c)) {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return std::nullopt;
            }
            value = (value << 4U) | digit;
        }
        return value;
    }

    /// The `\u` escape after the backslash and the 'u': one code point, from a
    /// surrogate pair when it takes two.
    [[nodiscard]] std::optional<std::uint32_t> unicode_escape()
    {
        const std::optional<std::uint32_t> first = hex4();
        if (!first.has_value() || (*first >= 0xDC00U && *first <= 0xDFFFU)) {
            return std::nullopt; // not hex, or a lone low surrogate
        }
        if (*first < 0xD800U || *first > 0xDBFFU) {
            return first;
        }
        if (!literal("\\u")) {
            return std::nullopt; // a high surrogate with nothing after it
        }
        const std::optional<std::uint32_t> second = hex4();
        if (!second.has_value() || *second < 0xDC00U || *second > 0xDFFFU) {
            return std::nullopt;
        }
        return 0x10000U + ((*first - 0xD800U) << 10U) + (*second - 0xDC00U);
    }

    [[nodiscard]] Result<std::string> parse_string()
    {
        ++at_; // '"'
        std::string out;
        for (;;) {
            if (done()) {
                return malformed("json: unterminated string");
            }
            if (out.size() > kMaxJsonString) {
                return too_large("json: string too long");
            }
            const char c = text_[at_++];
            if (c == '"') {
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20U) {
                return malformed("json: control character in a string");
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (done()) {
                return malformed("json: unterminated escape");
            }
            switch (text_[at_++]) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                const std::optional<std::uint32_t> code_point = unicode_escape();
                if (!code_point.has_value()) {
                    return malformed("json: bad \\u escape");
                }
                append_utf8(out, *code_point);
                break;
            }
            default:
                return malformed("json: unknown escape");
            }
        }
    }

    /// RFC 8259 section 6, kept as text: the reader never needs a float.
    [[nodiscard]] Result<JsonValue> parse_number()
    {
        const std::size_t start = at_;
        const auto digits = [this] {
            const std::size_t from = at_;
            while (!done() && is_digit(peek())) {
                ++at_;
            }
            return at_ - from;
        };

        if (!done() && peek() == '-') {
            ++at_;
        }
        if (done() || !is_digit(peek())) {
            return malformed("json: a value was expected");
        }
        if (peek() == '0') {
            ++at_;
        } else {
            static_cast<void>(digits());
        }
        if (!done() && peek() == '.') {
            ++at_;
            if (digits() == 0) {
                return malformed("json: a digit was expected after '.'");
            }
        }
        if (!done() && (peek() == 'e' || peek() == 'E')) {
            ++at_;
            if (!done() && (peek() == '+' || peek() == '-')) {
                ++at_;
            }
            if (digits() == 0) {
                return malformed("json: a digit was expected in the exponent");
            }
        }
        if (at_ - start > kMaxJsonString) {
            return too_large("json: number too long");
        }
        JsonValue value;
        value.kind = JsonValue::Kind::Number;
        value.text = std::string{text_.substr(start, at_ - start)};
        return value;
    }

    std::string_view text_;
    std::size_t at_ = 0;
    std::size_t values_ = 0;
};

} // namespace

const JsonValue* JsonValue::find(std::string_view key) const noexcept
{
    const auto found = std::ranges::find_if(
        members, [key](const JsonMember& member) { return member.key == key; });
    return found == members.end() ? nullptr : &found->value;
}

std::optional<std::uint64_t> JsonValue::as_uint() const noexcept
{
    if (kind != Kind::Number || text.empty() ||
        !std::ranges::all_of(text, [](char c) { return is_digit(c); })) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            return std::nullopt;
        }
        value = value * 10U + digit;
    }
    return value;
}

Result<JsonValue> parse_json(std::string_view text)
{
    if (text.size() > kMaxJsonSize) {
        return too_large("json: document too large");
    }
    return Parser{text}.document();
}

} // namespace smply::dfu_package
