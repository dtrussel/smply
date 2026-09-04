// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_DETAIL_EXPECTED_HPP
#define SMPLY_DETAIL_EXPECTED_HPP

/// \file
/// A minimal stand-in for std::expected, used when the standard library's is
/// unavailable (ADR-0002).
///
/// This implements only the subset smply uses, with std::expected's names and
/// semantics: has_value / operator bool, operator*, operator->, error(),
/// value_or, converting construction, and unexpected<E>. It is deliberately
/// NOT a complete implementation -- notably there is no `value()`, because its
/// standard behaviour is to throw and smply does not use exceptions for
/// protocol outcomes.
///
/// Because the subset is smaller than std::expected's API, code that compiles
/// against the standard type can fail against this one. The
/// `linux-gcc-cxx23-std-expected` CI job builds the same tests against
/// std::expected so the two stay interchangeable.
///
/// Storage is a std::variant rather than a hand-managed union. A union means
/// writing the cross-state assignment dance by hand -- destroy one member,
/// construct the other, stay correct when that construction throws -- which is
/// exactly the manual lifetime management this project avoids
/// (docs/design.md section 11). Because both alternatives are required below to
/// be nothrow-move-constructible, the variant can never become
/// valueless_by_exception, so that state needs no handling.
///
/// When smply moves to C++23, delete this file; no public API changes.

#include <cassert>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>

namespace smply::detail {

/// Wraps an error value so it can be returned where an expected is expected.
template<class E>
class unexpected
{
public:
    constexpr explicit unexpected(E error) noexcept(std::is_nothrow_move_constructible_v<E>)
        : error_{std::move(error)}
    {}

    [[nodiscard]] constexpr const E& error() const& noexcept
    {
        return error_;
    }

    [[nodiscard]] constexpr E& error() & noexcept
    {
        return error_;
    }

    [[nodiscard]] constexpr E&& error() && noexcept
    {
        return std::move(error_);
    }

    // Not constexpr: E is smply::Error, whose comparison touches a std::string.
    // A defaulted constexpr comparison would require E's to be constexpr too.
    [[nodiscard]] friend bool operator==(const unexpected&, const unexpected&) = default;

private:
    E error_;
};

template<class E>
unexpected(E) -> unexpected<E>;

/// Tag for constructing an expected in its error state in place.
struct unexpect_t
{
    explicit unexpect_t() = default;
};

inline constexpr unexpect_t unexpect{};

/// Either a value of type T or an error of type E.
template<class T, class E>
class expected
{
    static_assert(!std::is_reference_v<T>, "expected<T&> is not supported");
    static_assert(!std::is_void_v<T>, "use the expected<void, E> specialisation");
    // Guarantees the variant can never be valueless_by_exception, so every
    // accessor below has exactly two states to consider. Every T and E smply
    // uses satisfies this; one that does not gets a clear error here rather
    // than a surprising third state at run time.
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "expected<T, E> requires T to be nothrow-move-constructible");
    static_assert(std::is_nothrow_move_constructible_v<E>,
                  "expected<T, E> requires E to be nothrow-move-constructible");

    static constexpr std::size_t kValue = 0;
    static constexpr std::size_t kError = 1;

public:
    using value_type = T;
    using error_type = E;

    constexpr expected() noexcept(std::is_nothrow_default_constructible_v<T>)
        requires std::is_default_constructible_v<T>
        : storage_{std::in_place_index<kValue>}
    {}

    // Converting construction from a value. Excluded for arguments that are
    // themselves an expected or an unexpected, so those pick the right ctor.
    template<class U = T>
        requires(!std::is_same_v<std::remove_cvref_t<U>, expected> &&
                 !std::is_same_v<std::remove_cvref_t<U>, unexpect_t> &&
                 !std::is_same_v<std::remove_cvref_t<U>, unexpected<E>> &&
                 std::is_constructible_v<T, U>)
    constexpr explicit(!std::is_convertible_v<U, T>)
        expected(U&& value) noexcept(std::is_nothrow_constructible_v<T, U>)
        : storage_{std::in_place_index<kValue>, std::forward<U>(value)}
    {}

    constexpr expected(const unexpected<E>& error)
        : storage_{std::in_place_index<kError>, error.error()}
    {}

    constexpr expected(unexpected<E>&& error) noexcept(std::is_nothrow_move_constructible_v<E>)
        : storage_{std::in_place_index<kError>, std::move(error).error()}
    {}

    template<class... Args>
    constexpr explicit expected(unexpect_t /*tag*/, Args&&... args)
        : storage_{std::in_place_index<kError>, std::forward<Args>(args)...}
    {}

    [[nodiscard]] constexpr bool has_value() const noexcept
    {
        return storage_.index() == kValue;
    }

    constexpr explicit operator bool() const noexcept
    {
        return has_value();
    }

    // Preconditions: has_value(). Violating this is a programming error; debug
    // builds trap rather than reading the wrong alternative.
    [[nodiscard]] constexpr const T& operator*() const& noexcept
    {
        assert(has_value());
        return *std::get_if<kValue>(&storage_);
    }

    [[nodiscard]] constexpr T& operator*() & noexcept
    {
        assert(has_value());
        return *std::get_if<kValue>(&storage_);
    }

    [[nodiscard]] constexpr T&& operator*() && noexcept
    {
        assert(has_value());
        return std::move(*std::get_if<kValue>(&storage_));
    }

    [[nodiscard]] constexpr const T* operator->() const noexcept
    {
        assert(has_value());
        return std::get_if<kValue>(&storage_);
    }

    [[nodiscard]] constexpr T* operator->() noexcept
    {
        assert(has_value());
        return std::get_if<kValue>(&storage_);
    }

    // Preconditions: !has_value().
    [[nodiscard]] constexpr const E& error() const& noexcept
    {
        assert(!has_value());
        return *std::get_if<kError>(&storage_);
    }

    [[nodiscard]] constexpr E& error() & noexcept
    {
        assert(!has_value());
        return *std::get_if<kError>(&storage_);
    }

    [[nodiscard]] constexpr E&& error() && noexcept
    {
        assert(!has_value());
        return std::move(*std::get_if<kError>(&storage_));
    }

    template<class U>
    [[nodiscard]] constexpr T value_or(U&& fallback) const&
    {
        return has_value() ? **this : static_cast<T>(std::forward<U>(fallback));
    }

    template<class U>
    [[nodiscard]] constexpr T value_or(U&& fallback) &&
    {
        return has_value() ? std::move(**this) : static_cast<T>(std::forward<U>(fallback));
    }

    [[nodiscard]] friend bool operator==(const expected& lhs, const expected& rhs)
    {
        return lhs.storage_ == rhs.storage_;
    }

private:
    std::variant<T, E> storage_;
};

/// The void specialisation: success carries no value.
template<class E>
class expected<void, E>
{
    static_assert(std::is_nothrow_move_constructible_v<E>,
                  "expected<void, E> requires E to be nothrow-move-constructible");

    static constexpr std::size_t kValue = 0;
    static constexpr std::size_t kError = 1;

public:
    using value_type = void;
    using error_type = E;

    constexpr expected() noexcept : storage_{std::in_place_index<kValue>} {}

    constexpr expected(const unexpected<E>& error)
        : storage_{std::in_place_index<kError>, error.error()}
    {}

    constexpr expected(unexpected<E>&& error) noexcept(std::is_nothrow_move_constructible_v<E>)
        : storage_{std::in_place_index<kError>, std::move(error).error()}
    {}

    template<class... Args>
    constexpr explicit expected(unexpect_t /*tag*/, Args&&... args)
        : storage_{std::in_place_index<kError>, std::forward<Args>(args)...}
    {}

    [[nodiscard]] constexpr bool has_value() const noexcept
    {
        return storage_.index() == kValue;
    }

    constexpr explicit operator bool() const noexcept
    {
        return has_value();
    }

    constexpr void operator*() const noexcept
    {
        assert(has_value());
    }

    // Preconditions: !has_value().
    [[nodiscard]] constexpr const E& error() const& noexcept
    {
        assert(!has_value());
        return *std::get_if<kError>(&storage_);
    }

    [[nodiscard]] constexpr E& error() & noexcept
    {
        assert(!has_value());
        return *std::get_if<kError>(&storage_);
    }

    [[nodiscard]] constexpr E&& error() && noexcept
    {
        assert(!has_value());
        return std::move(*std::get_if<kError>(&storage_));
    }

    [[nodiscard]] friend bool operator==(const expected& lhs, const expected& rhs)
    {
        return lhs.storage_ == rhs.storage_;
    }

private:
    std::variant<std::monostate, E> storage_;
};

} // namespace smply::detail

#endif // SMPLY_DETAIL_EXPECTED_HPP
