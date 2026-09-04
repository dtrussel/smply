#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Proves that every quality gate actually FAILS when it should.
#
# A gate that never fires is worse than no gate: it produces a green tick that
# means nothing. P0's acceptance criterion is that each gate has been observed
# rejecting a deliberate violation, and this script is how that is demonstrated
# and re-demonstrated.
#
# It operates entirely on a throwaway copy of the tree in a temporary
# directory. THE WORKING TREE IS NEVER MODIFIED.
#
# Usage: tools/verify_gates.sh [--keep]
set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
KEEP=0
[[ "${1:-}" == "--keep" ]] && KEEP=1

SCRATCH="$(mktemp -d)"
cleanup() { [[ $KEEP -eq 1 ]] || rm -rf "$SCRATCH"; }
trap cleanup EXIT
WORK="$SCRATCH/repo"

echo "=== gate verification ==="
echo "scratch: $WORK"
echo

# Copy the tree without build outputs or git metadata.
mkdir -p "$WORK"
tar -C "$REPO" --exclude=build --exclude=.git --exclude=_deps -cf - . | tar -C "$WORK" -xf -

# Reuse the already-downloaded dependencies so this does not re-clone.
DEPS_CACHE="$REPO/build/linux-clang/_deps"
CONFIGURE_ARGS=(-G Ninja -S "$WORK" -B "$WORK/build"
                -DCMAKE_BUILD_TYPE=Debug
                -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
                -DSMPLY_BUILD_TESTS=ON)
[[ -d "$DEPS_CACHE" ]] && CONFIGURE_ARGS+=(-DFETCHCONTENT_BASE_DIR="$DEPS_CACHE")

echo "--- baseline configure (must succeed) ---"
if ! cmake "${CONFIGURE_ARGS[@]}" > "$SCRATCH/configure.log" 2>&1; then
    echo "FATAL: the baseline configure failed; the scratch copy is broken." >&2
    tail -30 "$SCRATCH/configure.log" >&2
    exit 1
fi
echo "ok"
echo

PASS=0
FAIL=0

# expect_fail <description> <command...>
# Runs the command in $WORK and asserts a non-zero exit.
expect_fail() {
    local description="$1"
    shift
    local output
    output="$(cd "$WORK" && "$@" 2>&1)"
    local status=$?
    if [[ $status -ne 0 ]]; then
        printf '  PASS  %s\n' "$description"
        printf '        (gate said: %s)\n' "$(echo "$output" | grep -viE '^\s*$' | tail -1 | cut -c1-100)"
        PASS=$((PASS + 1))
    else
        printf '  FAIL  %s\n' "$description"
        printf '        The gate did NOT reject the violation. It is not protecting anything.\n'
        FAIL=$((FAIL + 1))
    fi
}

restore() { tar -C "$REPO" --exclude=build --exclude=.git -cf - "$1" | tar -C "$WORK" -xf -; }

echo "--- each gate must reject its violation ---"

# 1. clang-format
printf 'int   main( ){return 0 ;}\n' >> "$WORK/src/version.cpp"
expect_fail "clang-format rejects misformatted code" tools/format.sh --check
restore src/version.cpp

# 2. Strict warnings (-Wconversion) on smply's own targets
cat >> "$WORK/src/version.cpp" <<'EOF'
namespace smply { int narrowing_violation(double d); int narrowing_violation(double d) { return d; } }
EOF
expect_fail "compiler warnings-as-errors reject a narrowing conversion" \
    cmake --build "$WORK/build" --target smply
restore src/version.cpp
cmake --build "$WORK/build" --target smply > /dev/null 2>&1  # back to green

# 3. clang-tidy
cat >> "$WORK/src/version.cpp" <<'EOF'
namespace smply { int* tidy_violation(char* p); int* tidy_violation(char* p) { return reinterpret_cast<int*>(p); } }
EOF
expect_fail "clang-tidy rejects reinterpret_cast over raw bytes" tools/lint.sh build
restore src/version.cpp

# 4. Public header discipline: third-party include
printf '#include <qcbor/qcbor.h>\n' >> "$WORK/include/smply/version.hpp.in"
expect_fail "check_public_headers rejects a QCBOR include in a public header" \
    python3 tools/check_public_headers.py --build-dir "$WORK/build"
restore include/smply/version.hpp.in

# 5. Public header discipline: self-containment
printf 'inline std::string broken() { return {}; }\n' >> "$WORK/include/smply/version.hpp.in"
sed -i 's|@PROJECT_VERSION_MAJOR@|0|; s|@PROJECT_VERSION_MINOR@|1|; s|@PROJECT_VERSION_PATCH@|0|; s|"@PROJECT_VERSION@"|"0.1.0"|' \
    "$WORK/include/smply/version.hpp.in"
cp "$WORK/include/smply/version.hpp.in" "$WORK/include/smply/selfcontain_probe.hpp"
expect_fail "check_public_headers rejects a header that is not self-contained" \
    python3 tools/check_public_headers.py --build-dir "$WORK/build"
rm -f "$WORK/include/smply/selfcontain_probe.hpp"
restore include/smply/version.hpp.in

# 6. Dependency inventory: undeclared dependency
cat >> "$WORK/cmake/dependencies.cmake" <<'EOF'
FetchContent_Declare(totally_undeclared_library
    GIT_REPOSITORY https://example.invalid/x.git
    GIT_TAG        0123456789abcdef0123456789abcdef01234567)
EOF
expect_fail "check_deps rejects a dependency absent from docs/dependencies.md" \
    python3 tools/check_deps.py
restore cmake/dependencies.cmake

# 7. Dependency inventory: pinned to a tag rather than a commit hash
sed -i 's|^set(SMPLY_QCBOR_COMMIT .*|set(SMPLY_QCBOR_COMMIT "v1.6.1")|' "$WORK/cmake/dependencies.cmake"
expect_fail "check_deps rejects a tag pin instead of a full commit hash" \
    python3 tools/check_deps.py
restore cmake/dependencies.cmake

# 8. Docs R2: a phase marked Complete that still lists remaining work
python3 - "$WORK/docs/roadmap.md" <<'PY'
import re, sys, pathlib
p = pathlib.Path(sys.argv[1]); t = p.read_text()
t = t.replace("## P1 — Core types\n\n**Status: Planned**",
              "## P1 — Core types\n\n**Status: Complete**\n\n**Remaining in this phase.** The whole thing, actually.")
p.write_text(t)
PY
expect_fail "check_docs R2 rejects a Complete phase with remaining work" \
    python3 tools/check_docs.py
restore docs/roadmap.md

# 9. Docs R3: reference to a non-existent ADR
printf '\nSee [ADR-0099](decisions/ADR-0099-imaginary.md).\n' >> "$WORK/docs/architecture.md"
expect_fail "check_docs R3 rejects a reference to a non-existent ADR" \
    python3 tools/check_docs.py
restore docs/architecture.md

# 10. Docs R3: invalid ADR status
sed -i 's|^\*\*Status:\*\* Accepted (2026-09-04)|**Status:** Probably fine|' \
    "$WORK/docs/decisions/ADR-0001-cpp-standard.md"
expect_fail "check_docs R3 rejects an invalid ADR Status line" python3 tools/check_docs.py
restore docs/decisions/ADR-0001-cpp-standard.md

# 11. Docs R4: an undocumented public symbol
python3 - "$WORK/include/smply/version.hpp.in" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); t = p.read_text()
t = t.replace("}  // namespace smply", "int undocumented_public_thing(int x);\n\n}  // namespace smply")
p.write_text(t)
PY
expect_fail "check_docs R4 rejects an undocumented public symbol" python3 tools/check_docs.py
restore include/smply/version.hpp.in

# 12 and 13. Consumer flag-leak guard, at configure time and at compile time.
# Both layers are checked: the configure-time assertion gives the good error
# message, the compile of tests/consumer is the ground truth behind it.
sed -i 's|target_link_libraries(smply PRIVATE smply_internal_options)|target_link_libraries(smply PUBLIC smply_internal_options)|' \
    "$WORK/CMakeLists.txt"

expect_fail "the configure-time guard rejects strict flags leaking to consumers" \
    cmake "${CONFIGURE_ARGS[@]}"

# Now with the configure-time guard removed, so the compile is the only thing
# standing between a leak and a silent regression.
sed -i '/^get_target_property(_smply_iface_libs/,/^endif()$/d' "$WORK/tests/consumer/CMakeLists.txt"
cmake "${CONFIGURE_ARGS[@]}" > /dev/null 2>&1
expect_fail "the consumer target fails to compile when it inherits strict flags" \
    cmake --build "$WORK/build" --target smply_consumer_check

restore CMakeLists.txt
restore tests/consumer/CMakeLists.txt
cmake "${CONFIGURE_ARGS[@]}" > /dev/null 2>&1

echo
echo "=== $PASS gate(s) verified, $FAIL not protecting anything ==="
[[ $FAIL -eq 0 ]] || exit 1
