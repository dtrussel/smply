// SPDX-License-Identifier: Apache-2.0
//
// These tests are written against the SUBSET of std::expected that ADR-0002
// commits to, and they run unchanged against both backings: the default C++20
// build uses smply's own expected, and the linux-gcc-cxx23-std-expected preset
// builds the very same file against std::expected. A test that passes under one
// and not the other means the two have diverged.

#include "smply/result.hpp"

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

using smply::Error;
using smply::ErrorCode;
using smply::fail;
using smply::Result;

namespace {

/// Counts copies and moves so tests can assert that Results do not copy behind
/// the caller's back.
struct Counted
{
    static inline int copies = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
    static inline int moves = 0;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

    int value = 0;

    Counted() = default;

    explicit Counted(int v) : value{v} {}

    Counted(const Counted& other) : value{other.value}
    {
        ++copies;
    }

    Counted(Counted&& other) noexcept : value{other.value}
    {
        ++moves;
    }

    Counted& operator=(const Counted& other)
    {
        if (this != &other) {
            value = other.value;
            ++copies;
        }
        return *this;
    }

    Counted& operator=(Counted&& other) noexcept
    {
        if (this != &other) {
            value = other.value;
            ++moves;
        }
        return *this;
    }

    ~Counted() = default;

    static void reset()
    {
        copies = 0;
        moves = 0;
    }
};

} // namespace

TEST_CASE("Result holds a value", "[result]")
{
    Result<int> result{42};

    REQUIRE(result.has_value());
    REQUIRE(static_cast<bool>(result));
    REQUIRE(*result == 42);
    REQUIRE(result.value_or(7) == 42);
}

TEST_CASE("Result holds an error", "[result]")
{
    Result<int> result = fail(ErrorCode::Timeout, "test");

    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(static_cast<bool>(result));
    REQUIRE(result.error().code() == ErrorCode::Timeout);
    REQUIRE(result.value_or(7) == 7);
}

TEST_CASE("fail() builds a failure from a code or a full Error", "[result]")
{
    SECTION("from a bare code")
    {
        Result<int> result = fail(ErrorCode::Disconnected);
        REQUIRE(result.error().code() == ErrorCode::Disconnected);
        REQUIRE(result.error().where() == nullptr);
    }

    SECTION("from a code and a call site")
    {
        Result<int> result = fail(ErrorCode::CborDecode, "decoder");
        REQUIRE(result.error().code() == ErrorCode::CborDecode);
        REQUIRE(std::string{result.error().where()} == "decoder");
    }

    SECTION("from a fully built Error, preserving device detail")
    {
        auto mgmt = smply::MgmtError::scoped(smply::Group::Image, 30);
        Result<int> result =
            fail(Error{ErrorCode::ProtocolError, mgmt, "upload"}.with_reason("too large"));

        REQUIRE(result.error().code() == ErrorCode::ProtocolError);
        REQUIRE(result.error().mgmt().has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by REQUIRE above.
        REQUIRE(result.error().mgmt()->rc == 30);
        REQUIRE(result.error().reason() == "too large");
    }
}

TEST_CASE("Result<void> distinguishes success from failure", "[result]")
{
    SECTION("success")
    {
        const Result<void> result{};
        REQUIRE(result.has_value());
        REQUIRE(static_cast<bool>(result));
    }

    SECTION("failure")
    {
        Result<void> result = fail(ErrorCode::TransportBusy);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code() == ErrorCode::TransportBusy);
    }
}

TEST_CASE("Result carries move-only payloads", "[result]")
{
    Result<std::unique_ptr<int>> result{std::make_unique<int>(11)};

    REQUIRE(result.has_value());
    REQUIRE(**result == 11);

    // Moving out leaves the Result engaged but its payload moved-from.
    auto owned = std::move(*result);
    REQUIRE(*owned == 11);
}

TEST_CASE("Result of a move-only payload can be moved", "[result]")
{
    Result<std::unique_ptr<int>> source{std::make_unique<int>(5)};
    Result<std::unique_ptr<int>> destination{std::move(source)};

    REQUIRE(destination.has_value());
    REQUIRE(**destination == 5);
}

TEST_CASE("Result does not copy its payload when constructed or moved", "[result]")
{
    Counted::reset();

    Result<Counted> result{Counted{3}};
    const int copies_after_construction = Counted::copies;

    Result<Counted> moved{std::move(result)};
    const int copies_after_move = Counted::copies;

    REQUIRE(copies_after_construction == 0);
    REQUIRE(copies_after_move == 0);
    REQUIRE(moved->value == 3);
}

TEST_CASE("operator-> reaches through to the payload", "[result]")
{
    struct Payload
    {
        int field = 9;
    };

    Result<Payload> result{Payload{}};
    REQUIRE(result->field == 9);

    result->field = 12;
    REQUIRE(result->field == 12);
}

TEST_CASE("Results compare by state and content", "[result]")
{
    REQUIRE(Result<int>{1} == Result<int>{1});
    REQUIRE_FALSE(Result<int>{1} == Result<int>{2});

    REQUIRE(Result<int>{fail(ErrorCode::Timeout)} == Result<int>{fail(ErrorCode::Timeout)});
    REQUIRE_FALSE(Result<int>{fail(ErrorCode::Timeout)} == Result<int>{fail(ErrorCode::Cancelled)});

    // A value and an error are never equal, whatever they contain.
    REQUIRE_FALSE(Result<int>{1} == Result<int>{fail(ErrorCode::Timeout)});
}

TEST_CASE("Result can be reassigned across states", "[result]")
{
    Result<std::string> result{std::string{"first"}};
    REQUIRE(*result == "first");

    // value -> error: the string must be destroyed, not leaked.
    result = fail(ErrorCode::Cancelled);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code() == ErrorCode::Cancelled);

    // error -> value, with a payload long enough to defeat SSO so ASan sees any
    // mistake in the union's lifetime management.
    result = std::string(256, 'x');
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 256);

    // value -> value.
    result = std::string{"third"};
    REQUIRE(*result == "third");
}

TEST_CASE("Result<void> can be reassigned across states", "[result]")
{
    Result<void> result{};
    REQUIRE(result.has_value());

    result = fail(Error{ErrorCode::Internal}.with_reason(std::string(256, 'y')));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().reason().size() == 256);

    result = Result<void>{};
    REQUIRE(result.has_value());
}

TEST_CASE("value_or moves out of an rvalue rather than copying", "[result]")
{
    Counted::reset();

    Result<Counted> result{Counted{4}};
    const Counted taken = std::move(result).value_or(Counted{0});

    REQUIRE(taken.value == 4);
    REQUIRE(Counted::copies == 0);
}

TEST_CASE("the expected backing matches the build configuration", "[result]")
{
    // Guards the selection logic in result.hpp. Without this, a mistake there
    // would silently make every build test the same backing -- which is exactly
    // what happens by default, since std::expected does not exist in C++20.
    INFO("SMPLY_USING_STD_EXPECTED = " << SMPLY_USING_STD_EXPECTED);
#if defined(__cpp_lib_expected) && (__cpp_lib_expected >= 202202L) &&                              \
    !defined(SMPLY_FORCE_FALLBACK_EXPECTED)
    STATIC_REQUIRE(SMPLY_USING_STD_EXPECTED == 1);
    STATIC_REQUIRE(std::is_same_v<Result<int>, std::expected<int, Error>>);
#else
    STATIC_REQUIRE(SMPLY_USING_STD_EXPECTED == 0);
    STATIC_REQUIRE(std::is_same_v<Result<int>, smply::detail::expected<int, Error>>);
#endif
}
