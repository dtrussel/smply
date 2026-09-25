// SPDX-License-Identifier: Apache-2.0
#ifndef SMPLY_SRC_DETAIL_NARROW_HPP
#define SMPLY_SRC_DETAIL_NARROW_HPP

/// \file
/// Integer narrowing, in two forms with different promises.
///
/// * `narrow_cast` is for a value the caller has **already** bounded. It checks
///   nothing, and says at the call site that the check happened.
/// * `checked_narrow` is for a value **nobody** has bounded yet, typically a
///   number a device sent. It answers `std::nullopt` rather than truncating.
///
/// Every device-supplied number goes through the second form, or through an
/// explicit comparison against a bound, before it is used (CLAUDE.md rule 6).

#include <optional>
#include <type_traits>
#include <utility>

namespace smply::detail {

/// Converts a value already known to fit into \p To.
///
/// A template on purpose. On a 64-bit host `std::uint64_t` and `std::size_t`
/// are the same type, and GCC's `-Wuseless-cast` rejects a direct `static_cast`
/// between them. On a 32-bit host the narrowing is real and must not be left
/// implicit. A dependent conversion satisfies both.
template<class To, class From>
    requires std::is_integral_v<To> && std::is_integral_v<From>
[[nodiscard]] constexpr To narrow_cast(From value) noexcept
{
    return static_cast<To>(value);
}

/// Converts \p value to \p To if it is representable there, and answers
/// `std::nullopt` if it is not. Correct across signedness: a negative value is
/// never representable in an unsigned type.
template<class To, class From>
    requires std::is_integral_v<To> && std::is_integral_v<From>
[[nodiscard]] constexpr std::optional<To> checked_narrow(From value) noexcept
{
    if (!std::in_range<To>(value)) {
        return std::nullopt;
    }
    return static_cast<To>(value);
}

} // namespace smply::detail

#endif // SMPLY_SRC_DETAIL_NARROW_HPP
