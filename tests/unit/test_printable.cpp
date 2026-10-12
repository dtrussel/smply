// SPDX-License-Identifier: Apache-2.0
//
// The tools' escape for untrusted text (security.md T12): control characters
// become `\xNN`, and everything else, UTF-8 included, is kept.

#include "dfu_app/printable.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using smply::dfu_app::printable;

TEST_CASE("printable keeps printable text as it is", "[dfu_app][printable]")
{
    CHECK(printable("").empty());
    CHECK(printable("image 0: expected board name \"a\"") == "image 0: expected board name \"a\"");
    // Non-control UTF-8: "é" (C3 A9) and U+00A0 (C2 A0), just past C1.
    CHECK(printable("caf\xC3\xA9 \xC2\xA0") == "caf\xC3\xA9 \xC2\xA0");
}

TEST_CASE("printable escapes C0, DEL and UTF-8 C1", "[dfu_app][printable]")
{
    CHECK(printable("a\x1B[2Jb") == "a\\x1B[2Jb");
    CHECK(printable("\n\r\t") == "\\x0A\\x0D\\x09");
    CHECK(printable(std::string_view{"\0", 1}) == "\\x00");
    CHECK(printable("\x7F") == "\\x7F");
    // U+009B, the C1 control sequence introducer, then both ends of C1.
    CHECK(printable("\xC2\x9B"
                    "31m") == "\\xC2\\x9B31m");
    CHECK(printable("\xC2\x80\xC2\x9F") == "\\xC2\\x80\\xC2\\x9F");
}

TEST_CASE("printable keeps a 0xC2 that does not start a C1 control", "[dfu_app][printable]")
{
    CHECK(printable("\xC2") == "\xC2");
    CHECK(printable("\xC2"
                    "A") == "\xC2"
                            "A");
}
