// SPDX-License-Identifier: Apache-2.0

#include "test_cbor.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace smply::test::tcbor {
namespace {

/// A cursor over the input. Every read is bounds-checked and reports failure by
/// returning nullopt, so a truncated or hostile encoding ends the parse rather
/// than reading past the end -- the same rule the library itself works under.
class Cursor
{
public:
    explicit Cursor(ConstBytes bytes) noexcept : bytes_{bytes} {}

    [[nodiscard]] bool exhausted() const noexcept
    {
        return position_ == bytes_.size();
    }

    [[nodiscard]] std::optional<std::uint8_t> take_byte() noexcept
    {
        if (position_ == bytes_.size()) {
            return std::nullopt;
        }
        return std::to_integer<std::uint8_t>(bytes_[position_++]);
    }

    [[nodiscard]] std::optional<ConstBytes> take(std::uint64_t count) noexcept
    {
        const std::size_t remaining = bytes_.size() - position_;
        if (count > remaining) {
            return std::nullopt;
        }
        const auto width = static_cast<std::size_t>(count);
        const ConstBytes out = bytes_.subspan(position_, width);
        position_ += width;
        return out;
    }

private:
    ConstBytes bytes_;
    std::size_t position_ = 0;
};

constexpr std::uint8_t kMajorShift = 5;
constexpr std::uint8_t kExtraMask = 0x1F;

/// Reads the argument that follows an initial byte.
///
/// Additional information 28-30 is reserved and 31 is an indefinite length;
/// both are rejected rather than guessed at (see the header).
std::optional<std::uint64_t> read_argument(Cursor& cursor, std::uint8_t extra)
{
    if (extra < 24) {
        return extra;
    }
    std::size_t width = 0;
    switch (extra) {
    case 24:
        width = 1;
        break;
    case 25:
        width = 2;
        break;
    case 26:
        width = 4;
        break;
    case 27:
        width = 8;
        break;
    default:
        return std::nullopt;
    }

    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) {
        const std::optional<std::uint8_t> next = cursor.take_byte();
        if (!next.has_value()) {
            return std::nullopt;
        }
        value = (value << 8U) | *next;
    }
    return value;
}

/// Guards against a hostile encoding nesting deeply enough to exhaust the
/// stack. Nothing MCUmgr sends is more than four deep.
constexpr unsigned kMaxDepth = 8;

std::optional<Value> parse_item(Cursor& cursor, unsigned depth);

/// Reads \p count items into \p out.
///
/// Recursive descent is the shape of the grammar, and the depth is capped by
/// kMaxDepth above, so a hostile encoding cannot drive it into the stack.
// NOLINTNEXTLINE(misc-no-recursion): bounded by kMaxDepth; see above.
bool parse_items(Cursor& cursor, unsigned depth, std::uint64_t count, std::vector<Value>& out)
{
    for (std::uint64_t i = 0; i < count; ++i) {
        std::optional<Value> item = parse_item(cursor, depth);
        if (!item.has_value()) {
            return false;
        }
        out.push_back(std::move(*item));
    }
    return true;
}

// NOLINTNEXTLINE(misc-no-recursion): see parse_items above.
std::optional<Value> parse_item(Cursor& cursor, unsigned depth)
{
    if (depth > kMaxDepth) {
        return std::nullopt;
    }

    const std::optional<std::uint8_t> initial = cursor.take_byte();
    if (!initial.has_value()) {
        return std::nullopt;
    }
    const auto major = static_cast<std::uint8_t>(*initial >> kMajorShift);
    const auto extra = static_cast<std::uint8_t>(*initial & kExtraMask);

    // Simple values are the one major type whose argument is not a length or a
    // number, so they are handled before the shared argument read.
    if (major == 7) {
        Value value;
        value.kind = Value::Kind::Bool;
        if (extra == 20 || extra == 21) {
            value.bool_value = extra == 21;
            return value;
        }
        return std::nullopt;
    }

    const std::optional<std::uint64_t> argument = read_argument(cursor, extra);
    if (!argument.has_value()) {
        return std::nullopt;
    }

    Value value;
    switch (major) {
    case 0:
        value.kind = Value::Kind::Uint;
        value.uint_value = *argument;
        return value;

    case 1: {
        // -1 - argument, which is representable only while the argument fits.
        constexpr std::uint64_t kMaxNegativeArgument = 0x7FFFFFFFFFFFFFFFULL;
        if (*argument > kMaxNegativeArgument) {
            return std::nullopt;
        }
        value.kind = Value::Kind::Nint;
        value.int_value = -1 - static_cast<std::int64_t>(*argument);
        return value;
    }

    case 2: {
        const std::optional<ConstBytes> content = cursor.take(*argument);
        if (!content.has_value()) {
            return std::nullopt;
        }
        value.kind = Value::Kind::Bytes;
        value.bytes.assign(content->begin(), content->end());
        return value;
    }

    case 3: {
        const std::optional<ConstBytes> content = cursor.take(*argument);
        if (!content.has_value()) {
            return std::nullopt;
        }
        value.kind = Value::Kind::Text;
        value.text.reserve(content->size());
        for (const std::byte byte : *content) {
            value.text.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
        }
        return value;
    }

    case 4:
        value.kind = Value::Kind::Array;
        if (!parse_items(cursor, depth + 1, *argument, value.items)) {
            return std::nullopt;
        }
        return value;

    case 5:
        value.kind = Value::Kind::Map;
        for (std::uint64_t i = 0; i < *argument; ++i) {
            std::optional<Value> key = parse_item(cursor, depth + 1);
            if (!key.has_value() || !key->is(Value::Kind::Text)) {
                return std::nullopt;
            }
            std::optional<Value> entry = parse_item(cursor, depth + 1);
            if (!entry.has_value()) {
                return std::nullopt;
            }
            value.items.push_back(std::move(*key));
            value.items.push_back(std::move(*entry));
        }
        return value;

    default:
        // Major type 6 is a tag; MCUmgr never sends one.
        return std::nullopt;
    }
}

} // namespace

std::optional<Value> parse(ConstBytes bytes)
{
    Cursor cursor{bytes};
    std::optional<Value> value = parse_item(cursor, 0);
    if (!value.has_value() || !cursor.exhausted()) {
        return std::nullopt;
    }
    return value;
}

} // namespace smply::test::tcbor
