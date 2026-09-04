#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Coverage report for smply's own sources (docs/quality-gates.md section 6).
#
# Thresholds are NOT enforced yet: against P0's placeholder library the numbers
# are meaningless. Enforcement switches on in P13, when there is protocol logic
# and state-machine code worth measuring. Until then this reports and always
# exits 0 unless --enforce is passed.
#
# Prefers gcovr, falls back to lcov, falls back to plain gcov -- so it is
# usable in a container with none of the extras installed.
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

if command -v gcovr >/dev/null 2>&1; then
    gcovr --root "$REPO" \
          --filter "$REPO/src/" --filter "$REPO/include/smply/" \
          --exclude '.*/_deps/.*' \
          --print-summary --txt \
          "$BUILD_DIR"
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
    echo
    echo "error: --enforce is not active until P13 (docs/quality-gates.md section 6)." >&2
    echo "       Thresholds when it is: ${LINE_MIN}% line, ${BRANCH_MIN}% branch." >&2
    exit 1
fi

echo
echo "reported only; thresholds (${LINE_MIN}% line / ${BRANCH_MIN}% branch) are enforced from P13"
