// SPDX-License-Identifier: Apache-2.0
//
// See tests/consumer/CMakeLists.txt. This file is intentionally written in a
// style that smply's own warning set rejects. It must still compile cleanly
// here, because a consumer does not inherit those flags.

#include "smply/version.hpp"

#include <cstdio>
#include <cstring>

namespace {

// Rejected by -Wconversion / MSVC C4244 in smply's own code.
int narrowing(double value)
{
    return value;
}

// Rejected by -Wold-style-cast in smply's own code.
unsigned c_style_cast(int value)
{
    return (unsigned)value;
}

} // namespace

int main()
{
    const char* v = smply::version();
    if (v == nullptr || std::strlen(v) == 0) {
        std::fputs("smply::version() returned an empty string\n", stderr);
        return 1;
    }
    if (std::strcmp(v, SMPLY_VERSION_STRING) != 0) {
        std::fputs("header/library version mismatch\n", stderr);
        return 1;
    }
    if (narrowing(3.7) != 3 || c_style_cast(7) != 7u) {
        return 1;
    }
    std::printf("smply %s\n", v);
    return 0;
}
