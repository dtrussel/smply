// SPDX-License-Identifier: Apache-2.0
//
// src/detail/narrow.hpp: the two narrowing helpers. checked_narrow is what
// stands between a number a device sent and the type smply stores it in, so its
// boundaries are the ones worth pinning: both ends of each target type, one
// step past them, and a negative value into an unsigned type, which a naive
// "value <= max" comparison accepts.

#include "detail/narrow.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

using smply::detail::checked_narrow;
using smply::detail::narrow_cast;

TEST_CASE("checked_narrow keeps every value the target can hold", "[narrow]")
{
    constexpr std::uint64_t kU32Max = std::numeric_limits<std::uint32_t>::max();
    CHECK(checked_narrow<std::uint32_t>(std::uint64_t{0}) == std::uint32_t{0});
    CHECK(checked_narrow<std::uint32_t>(kU32Max) == std::numeric_limits<std::uint32_t>::max());

    constexpr std::int64_t kI32Min = std::numeric_limits<std::int32_t>::min();
    constexpr std::int64_t kI32Max = std::numeric_limits<std::int32_t>::max();
    CHECK(checked_narrow<std::int32_t>(kI32Min) == std::numeric_limits<std::int32_t>::min());
    CHECK(checked_narrow<std::int32_t>(kI32Max) == std::numeric_limits<std::int32_t>::max());
    CHECK(checked_narrow<std::int32_t>(std::int64_t{-1}) == -1);
}

TEST_CASE("checked_narrow refuses one step past either end", "[narrow]")
{
    constexpr std::uint64_t kU32Max = std::numeric_limits<std::uint32_t>::max();
    CHECK(checked_narrow<std::uint32_t>(kU32Max + 1) == std::nullopt);
    CHECK(checked_narrow<std::uint32_t>(std::numeric_limits<std::uint64_t>::max()) == std::nullopt);

    constexpr std::int64_t kI32Min = std::numeric_limits<std::int32_t>::min();
    constexpr std::int64_t kI32Max = std::numeric_limits<std::int32_t>::max();
    CHECK(checked_narrow<std::int32_t>(kI32Min - 1) == std::nullopt);
    CHECK(checked_narrow<std::int32_t>(kI32Max + 1) == std::nullopt);
}

TEST_CASE("checked_narrow is correct across signedness", "[narrow]")
{
    // A negative number is never an unsigned one, however small it is.
    CHECK(checked_narrow<std::uint32_t>(std::int64_t{-1}) == std::nullopt);
    CHECK(checked_narrow<std::uint8_t>(std::int8_t{-128}) == std::nullopt);

    // And an unsigned value above the signed maximum does not wrap negative.
    CHECK(checked_narrow<std::int32_t>(std::uint32_t{0x80000000U}) == std::nullopt);
    CHECK(checked_narrow<std::int32_t>(std::uint64_t{7}) == 7);
}

TEST_CASE("narrow_cast converts a value the caller has already bounded", "[narrow]")
{
    // The same type on a 64-bit host: the case -Wuseless-cast would reject as a
    // direct static_cast, which is why the helper exists.
    constexpr std::uint64_t bounded = 4096;
    STATIC_REQUIRE(narrow_cast<std::size_t>(bounded) == std::size_t{4096});
    STATIC_REQUIRE(narrow_cast<std::uint16_t>(std::uint32_t{65535}) == std::uint16_t{65535});
}
