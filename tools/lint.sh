#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Static analysis: clang-tidy (required) and cppcheck (complementary).
#
# clang-tidy is run over an EXPLICIT file list rather than over all of
# compile_commands.json, which would otherwise walk _deps/ and QCBOR's C
# sources.
#
# Usage: tools/lint.sh [build-dir]
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${1:-build/linux-clang}"
if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
    echo "error: $BUILD_DIR/compile_commands.json not found." >&2
    echo "       Configure first, e.g. cmake --preset linux-clang" >&2
    exit 1
fi

status=0

# --- clang-tidy (required) --------------------------------------------------
CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
if command -v "$CLANG_TIDY" >/dev/null 2>&1; then
    # Only translation units, and only ours. Headers are covered via
    # HeaderFilterRegex when they are included by these TUs.
    mapfile -t tus < <(tools/sources.sh | grep -E '\.(cpp|cc)$' \
        | grep -v '^tests/consumer/')
    if [[ ${#tus[@]} -gt 0 ]]; then
        echo "running $("$CLANG_TIDY" --version | grep -m1 -oE '[Vv]ersion [0-9.]+') over ${#tus[@]} TUs"
        "$CLANG_TIDY" -p "$BUILD_DIR" "${tus[@]}" || status=1
    fi
else
    echo "error: $CLANG_TIDY not found (required)" >&2
    status=1
fi

# --- cppcheck (complementary; skipped when absent) --------------------------
if command -v cppcheck >/dev/null 2>&1; then
    echo "running $(cppcheck --version)"
    cppcheck --enable=warning,performance,portability \
             --std=c++20 \
             --language=c++ \
             --inline-suppr \
             --suppressions-list=tools/cppcheck-suppressions.txt \
             --error-exitcode=1 \
             --quiet \
             include src tests || status=1
else
    echo "note: cppcheck not installed -- skipping (CI runs it; see docs/quality-gates.md)"
fi

exit $status
