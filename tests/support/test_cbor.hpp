// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TESTS_TEST_CBOR_HPP
#define SMPLY_TESTS_TEST_CBOR_HPP

/// \file
/// A CBOR writer and reader for tests, deliberately **independent** of
/// `src/cbor/`.
///
/// Both halves are written straight from RFC 8949's major-type table rather
/// than from smply's façade, and neither shares a line with it. That
/// independence is the whole point: from P11 the component tests drive a real
/// client into a simulated server, and if the simulator decoded requests and
/// encoded responses with the same reader and writer the client uses, the round
/// trip would prove only that smply agrees with itself. A symmetric bug -- a
/// writer that emits a wrong head and a reader that accepts it -- would sail
/// straight through. Anchored on this side by hand-built byte vectors, it
/// cannot.
///
/// The subset is exactly what MCUmgr uses: definite lengths, unsigned and
/// negative integers, byte and text strings, booleans, arrays and maps with
/// text keys. Indefinite lengths, tags, floats and non-text map keys are
/// rejected rather than skipped -- a device that sent one would be doing
/// something this project has never seen, and silently tolerating it in a test
/// double would hide it.

#include "smply/bytes.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace smply::test::tcbor {

/// Builds CBOR by hand.
///
/// Every method writes one item; container methods write only the head, so a
/// caller states the element count itself and can therefore express a count
/// that disagrees with what follows.
class Writer
{
public:
    Writer& map(std::uint64_t pairs)
    {
        return head(5, pairs);
    }

    Writer& array(std::uint64_t items)
    {
        return head(4, items);
    }

    Writer& uint(std::uint64_t value)
    {
        return head(0, value);
    }

    /// A negative integer; \p value must be negative.
    Writer& nint(std::int64_t value)
    {
        return head(1, static_cast<std::uint64_t>(-(value + 1)));
    }

    Writer& text(std::string_view value)
    {
        head(3, value.size());
        for (const char ch : value) {
            out_.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
        }
        return *this;
    }

    Writer& blob(ConstBytes value)
    {
        head(2, value.size());
        out_.insert(out_.end(), value.begin(), value.end());
        return *this;
    }

    Writer& boolean(bool value)
    {
        out_.push_back(value ? std::byte{0xF5} : std::byte{0xF4});
        return *this;
    }

    /// Raw bytes, for expressing something no well-formed encoder would write.
    Writer& raw(std::initializer_list<std::uint8_t> values)
    {
        for (const std::uint8_t value : values) {
            out_.push_back(static_cast<std::byte>(value));
        }
        return *this;
    }

    /// \overload
    Writer& raw(ConstBytes values)
    {
        out_.insert(out_.end(), values.begin(), values.end());
        return *this;
    }

    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept
    {
        return out_;
    }

    [[nodiscard]] ConstBytes view() const noexcept
    {
        return ConstBytes{out_};
    }

private:
    /// The major type in the top three bits, then the argument in the shortest
    /// form that holds it -- which is what every MCUmgr encoder emits.
    Writer& head(std::uint8_t major, std::uint64_t value)
    {
        const auto initial = [&](std::uint8_t extra) {
            out_.push_back(static_cast<std::byte>((major << 5U) | extra));
        };
        if (value < 24) {
            initial(static_cast<std::uint8_t>(value));
        } else if (value <= 0xFFU) {
            initial(24);
            out_.push_back(static_cast<std::byte>(value));
        } else if (value <= 0xFFFFU) {
            initial(25);
            push_be(value, 2);
        } else if (value <= 0xFFFFFFFFU) {
            initial(26);
            push_be(value, 4);
        } else {
            initial(27);
            push_be(value, 8);
        }
        return *this;
    }

    void push_be(std::uint64_t value, std::size_t width)
    {
        for (std::size_t i = width; i > 0; --i) {
            const unsigned shift = static_cast<unsigned>((i - 1) * 8);
            out_.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
        }
    }

    std::vector<std::byte> out_;
};

/// One decoded CBOR item.
///
/// A tree rather than a flat map: MCUmgr responses nest (`images` is an array
/// of maps, and a slot-info slot carries its own list), and a decoder that
/// could only see the top level would not be able to check them.
class Value
{
public:
    enum class Kind : std::uint8_t
    {
        Uint,
        Nint,
        Bytes,
        Text,
        Bool,
        Array,
        Map,
    };

    Kind kind = Kind::Uint;
    std::uint64_t uint_value = 0;
    std::int64_t int_value = 0;
    bool bool_value = false;
    std::vector<std::byte> bytes;
    std::string text;
    /// Array elements; for a map, the entries flattened as key, value, key,
    /// value, exactly as CBOR itself encodes them.
    ///
    /// A map deliberately does *not* hold `vector<pair<string, Value>>`: a
    /// `std::pair` of an incomplete type is undefined, and while GCC accepted
    /// it Clang rejected the whole header. `std::vector<Value>` inside `Value`
    /// is the one nesting the standard actually permits.
    std::vector<Value> items;

    /// Entries, for a map: half the flattened size.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return kind == Kind::Map ? items.size() / 2 : items.size();
    }

    [[nodiscard]] bool is(Kind expected) const noexcept
    {
        return kind == expected;
    }

    /// The value for \p key, or nullptr when the key is absent or this is not
    /// a map. Absence and a present-but-wrong-typed value stay distinguishable,
    /// which is the distinction the whole protocol turns on.
    [[nodiscard]] const Value* find(std::string_view key) const noexcept
    {
        if (kind != Kind::Map) {
            return nullptr;
        }
        for (std::size_t i = 0; i + 1 < items.size(); i += 2) {
            if (items[i].text == key) {
                return &items[i + 1];
            }
        }
        return nullptr;
    }

    /// The unsigned value for \p key, when it is present and is one.
    [[nodiscard]] std::optional<std::uint64_t> get_uint(std::string_view key) const noexcept
    {
        const Value* found = find(key);
        if (found == nullptr || !found->is(Kind::Uint)) {
            return std::nullopt;
        }
        return found->uint_value;
    }

    /// The boolean value for \p key, when it is present and is one.
    [[nodiscard]] std::optional<bool> get_bool(std::string_view key) const noexcept
    {
        const Value* found = find(key);
        if (found == nullptr || !found->is(Kind::Bool)) {
            return std::nullopt;
        }
        return found->bool_value;
    }
};

/// Decodes one complete item, which must consume \p bytes exactly.
///
/// Trailing bytes are a failure rather than an ignored remainder: in this
/// protocol a payload is exactly one map, and anything after it means the
/// message was built wrong.
[[nodiscard]] std::optional<Value> parse(ConstBytes bytes);

} // namespace smply::test::tcbor

#endif // SMPLY_TESTS_TEST_CBOR_HPP
