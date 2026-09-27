// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_TESTS_CBOR_SHAPES_HPP
#define SMPLY_TESTS_CBOR_SHAPES_HPP

/// \file
/// One response document, written in either container encoding.
///
/// A Zephyr device writes indefinite-length maps and arrays unless
/// `CONFIG_ZCBOR_CANONICAL` is set, and nothing in MCUmgr sets it
/// (docs/protocol-notes.md section 9, A18). Hand-built goldens were all
/// definite-length until hardware showed otherwise, so every response a new
/// decoder reads is built here once and run in both encodings. The container
/// methods take the definite count and ignore it when writing indefinitely, so
/// a test states the document exactly once.

#include "minicbor/minicbor.hpp"

#include <cstdint>
#include <string_view>

namespace smply::test {

enum class Encoding : std::uint8_t
{
    Definite,
    Indefinite, ///< What a stock Zephyr device sends.
};

/// A `minicbor::Writer` whose containers follow one `Encoding`.
class Shape
{
public:
    explicit Shape(Encoding encoding) noexcept : encoding_{encoding} {}

    /// Opens a map of \p pairs entries; close it with `end()`.
    Shape& map(std::uint64_t pairs)
    {
        if (encoding_ == Encoding::Definite) {
            writer_.map(pairs);
        } else {
            writer_.raw({0xBF});
        }
        return *this;
    }

    /// Opens an array of \p items elements; close it with `end()`.
    Shape& array(std::uint64_t items)
    {
        if (encoding_ == Encoding::Definite) {
            writer_.array(items);
        } else {
            writer_.raw({0x9F});
        }
        return *this;
    }

    /// Closes the innermost container: a break when indefinite, nothing when
    /// definite.
    Shape& end()
    {
        if (encoding_ == Encoding::Indefinite) {
            writer_.raw({0xFF});
        }
        return *this;
    }

    Shape& text(std::string_view value)
    {
        writer_.text(value);
        return *this;
    }

    Shape& uint(std::uint64_t value)
    {
        writer_.uint(value);
        return *this;
    }

    /// The underlying writer, for items that have no container encoding.
    [[nodiscard]] minicbor::Writer& raw() noexcept
    {
        return writer_;
    }

    [[nodiscard]] ConstBytes view() const noexcept
    {
        return writer_.view();
    }

private:
    Encoding encoding_;
    minicbor::Writer writer_;
};

} // namespace smply::test

#endif // SMPLY_TESTS_CBOR_SHAPES_HPP
