#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Coverage report for smply's own sources (docs/quality-gates.md section 6).
#
# With --enforce the thresholds are real and a shortfall fails the run; without
# it the report is printed and the script exits 0. CI passes --enforce (from
# P13); a developer running it by hand usually does not want a non-zero exit
# while iterating.
#
# **Enforcement requires gcovr.** The lcov and gcov fallbacks below produce a
# different measurement -- the branch figure moves by roughly 12 points -- so
# enforcing against one of them would be enforcing a different threshold than
# the one quality-gates.md names. --enforce without gcovr is an error, not a
# pass: a gate that cannot fail is not a gate, which this script learned the
# hard way between P0 and P7.
#
# Usage: tools/coverage.sh [build-dir] [--enforce]
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-build/linux-gcc-coverage}"
[[ "${1:-}" == --* ]] && BUILD_DIR="build/linux-gcc-coverage"
ENFORCE=0
for arg in "$@"; do [[ "$arg" == "--enforce" ]] && ENFORCE=1; done

cd "$REPO"
if [[ ! -d "$BUILD_DIR" ]]; then
    echo "error: $BUILD_DIR not found. Configure with --preset linux-gcc-coverage first." >&2
    exit 1
fi

# Only smply's own code counts; tests, examples and dependencies do not.
LINE_MIN=85
BRANCH_MIN=75

echo "=== coverage over src/ and include/smply/ ==="

# Checked before anything is measured, not after: the lcov and gcov fallbacks
# below produce a different number, and running one of them first would either
# print a figure nobody may act on or fail for an unrelated reason.
if [[ $ENFORCE -eq 1 ]] && ! command -v gcovr >/dev/null 2>&1; then
    echo "error: --enforce needs gcovr; the lcov and gcov fallbacks measure" >&2
    echo "       branches differently, so enforcing against one of them would" >&2
    echo "       enforce a different threshold than the documented one." >&2
    echo "       pip install gcovr" >&2
    exit 1
fi

if command -v gcovr >/dev/null 2>&1; then
    # The search path must NOT follow --txt: gcovr takes the next positional as
    # that option's output file, and a directory there makes it fail. It failed
    # exactly that way from P0 until P7 -- silently, because this script exits 0
    # by design, so CI stayed green while producing no report at all.
    gcovr --root "$REPO" \
          "$BUILD_DIR" \
          --filter "$REPO/src/" --filter "$REPO/include/smply/" \
          --exclude '.*/_deps/.*' \
          --exclude-throw-branches \
          --print-summary --txt \
          --fail-under-line "$( [[ $ENFORCE -eq 1 ]] && echo "$LINE_MIN" || echo 0 )" \
          --fail-under-branch "$( [[ $ENFORCE -eq 1 ]] && echo "$BRANCH_MIN" || echo 0 )"
    GCOVR_STATUS=$?
elif command -v lcov >/dev/null 2>&1; then
    lcov --capture --directory "$BUILD_DIR" --output-file "$BUILD_DIR/coverage.info" \
         --rc branch_coverage=1 --quiet
    lcov --extract "$BUILD_DIR/coverage.info" "$REPO/src/*" "$REPO/include/smply/*" \
         --output-file "$BUILD_DIR/coverage.filtered.info" --rc branch_coverage=1 --quiet
    lcov --list "$BUILD_DIR/coverage.filtered.info" --rc branch_coverage=1
else
    echo "note: neither gcovr nor lcov installed -- falling back to plain gcov"
    # Absolute paths: gcov is invoked from inside the build directory.
    mapfile -t gcda < <(find "$REPO/$BUILD_DIR/CMakeFiles" -name '*.gcda' 2>/dev/null)
    if [[ ${#gcda[@]} -eq 0 ]]; then
        echo "error: no .gcda files. Did you build AND run the tests?" >&2
        exit 1
    fi
    (cd "$BUILD_DIR" && gcov -b -p "${gcda[@]}" 2>/dev/null) \
        | grep -A3 -F "File '$REPO/src" || echo "(no covered sources under src/ yet)"
fi

if [[ $ENFORCE -eq 1 ]]; then
    if [[ ${GCOVR_STATUS:-1} -ne 0 ]]; then
        echo >&2
        echo "error: below the thresholds in docs/quality-gates.md section 6" >&2
        echo "       (${LINE_MIN}% line, ${BRANCH_MIN}% branch)." >&2
        exit 1
    fi
    echo
    echo "coverage OK: at or above ${LINE_MIN}% line and ${BRANCH_MIN}% branch"
    exit 0
fi

echo
echo "reported only; pass --enforce to apply the ${LINE_MIN}%/${BRANCH_MIN}% thresholds"
