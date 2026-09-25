#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Proves smply is consumable out of tree, in all three ways a consumer starts.
#
#   find_package      against a prefix that `cmake --install` produced
#   add_subdirectory  over the source tree, as a vendored checkout would
#   FetchContent      declared with SOURCE_DIR, as a dependency would
#
# All three build and RUN the same program, tests/consumption/smoke.cpp. One
# program, because three would drift and the drifted one would be the mode that
# had quietly stopped checking anything.
#
# The find_package mode is the one that proves the *export*, and it is why the
# consumer projects are separate: anything built inside our own tree consumes
# the build-tree targets, where the in-tree ALIAS names resolve, so it cannot
# tell a correct export from one that ships smply::smply_util.
#
# Usage: tools/check_install.sh [work-dir]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${1:-$(mktemp -d)}"
BUILD="$WORK/build"
PREFIX="$WORK/prefix"

mkdir -p "$WORK"

echo "=== out-of-tree consumption check ==="
echo "work: $WORK"

# Release, because that is what a consumer builds, and because some warnings
# fire only at -O2 or above.
cmake -S "$REPO" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSMPLY_BUILD_TESTS=OFF \
    -DSMPLY_BUILD_EXAMPLES=OFF \
    ${FETCHCONTENT_BASE_DIR:+-DFETCHCONTENT_BASE_DIR="$FETCHCONTENT_BASE_DIR"} \
    > "$WORK/configure.log" 2>&1

cmake --build "$BUILD" > "$WORK/build.log" 2>&1
cmake --install "$BUILD" --prefix "$PREFIX" > "$WORK/install.log" 2>&1

# The un-configured template must not be in the package: it sits beside the
# public headers, and a consumer including it gets @SMPLY_VERSION_MAJOR@.
if [[ -e "$PREFIX/include/smply/version.hpp.in" ]]; then
    echo "FAIL: version.hpp.in was installed" >&2
    exit 1
fi

# One consumer project per mode. Each is a standalone CMake project; the two
# source-tree modes are told where the checkout is, since they consume it
# rather than the prefix.
run_mode() {
    local mode="$1"
    shift
    local dir="$WORK/consumer-$mode"

    echo "--- $mode ---"
    cmake -S "$REPO/tests/consumption/$mode" -B "$dir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        "$@" \
        > "$WORK/$mode-configure.log" 2>&1

    cmake --build "$dir" > "$WORK/$mode-build.log" 2>&1

    # Running it matters: a link error is caught above, but a package that
    # exports the headers and forgets the archive can still configure and build
    # a program that never calls into it.
    "$dir/smply_consumption_check"
}

run_mode find_package -DCMAKE_PREFIX_PATH="$PREFIX"

# The source-tree modes must not inherit the prefix: if CMAKE_PREFIX_PATH were
# set they could silently find the *installed* package instead of building the
# checkout, and would then be a third copy of mode 1.
run_mode add_subdirectory -DSMPLY_SOURCE_ROOT="$REPO" \
    ${FETCHCONTENT_BASE_DIR:+-DFETCHCONTENT_BASE_DIR="$FETCHCONTENT_BASE_DIR"}
run_mode fetchcontent -DSMPLY_SOURCE_ROOT="$REPO" \
    ${FETCHCONTENT_BASE_DIR:+-DFETCHCONTENT_BASE_DIR="$FETCHCONTENT_BASE_DIR"}

echo "out-of-tree consumption check OK (3 modes)"
