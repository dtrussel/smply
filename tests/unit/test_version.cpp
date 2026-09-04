// SPDX-License-Identifier: Apache-2.0

#include "smply/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

TEST_CASE("version() reports the compiled-in version", "[version]")
{
    REQUIRE(std::string_view{smply::version()} == std::string_view{SMPLY_VERSION_STRING});
}

TEST_CASE("version macros are consistent with the version string", "[version]")
{
    // Guards against a configure_file() mistake producing a header whose
    // components disagree with its string.
    const std::string expected = std::to_string(SMPLY_VERSION_MAJOR) + "." +
                                 std::to_string(SMPLY_VERSION_MINOR) + "." +
                                 std::to_string(SMPLY_VERSION_PATCH);
    REQUIRE(expected == std::string{SMPLY_VERSION_STRING});
}
