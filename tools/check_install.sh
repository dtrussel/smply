#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Proves the installed package is consumable out of tree.
#
# Builds smply, installs it into a throwaway prefix, then configures a SEPARATE
# CMake project (tests/install/) against that prefix with find_package(smply),
# builds it and RUNS it. Building anything inside our own tree would consume the
# build-tree targets and prove nothing about the export.
#
# Covers find_package only. The add_subdirectory and FetchContent modes are P18.
#
# Usage: tools/check_install.sh [work-dir]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${1:-$(mktemp -d)}"
BUILD="$WORK/build"
PREFIX="$WORK/prefix"
CONSUMER="$WORK/consumer"

mkdir -p "$WORK"

echo "=== install check ==="
echo "work: $WORK"

# Release, because that is what a consumer builds -- and because until P15a
# nothing in this project had ever been compiled at -O2 or above.
cmake -S "$REPO" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSMPLY_BUILD_TESTS=OFF \
    -DSMPLY_BUILD_EXAMPLES=OFF \
    ${FETCHCONTENT_BASE_DIR:+-DFETCHCONTENT_BASE_DIR="$FETCHCONTENT_BASE_DIR"} \
    > "$WORK/configure.log" 2>&1

cmake --build "$BUILD" > "$WORK/build.log" 2>&1
cmake --install "$BUILD" --prefix "$PREFIX" > "$WORK/install.log" 2>&1

echo "--- consuming it from outside the tree ---"
cmake -S "$REPO/tests/install" -B "$CONSUMER" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    > "$WORK/consumer-configure.log" 2>&1

cmake --build "$CONSUMER" > "$WORK/consumer-build.log" 2>&1

# Running it matters: a link error is caught above, but a package that exports
# the headers and forgets the archive can still configure and build a program
# that never calls into it.
"$CONSUMER/smply_install_check"

echo "install check OK"
