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
# check_docs.py R5 resolves layout entries against `git ls-files`, so the copy
# needs an index. Without one, the documentation gate fails on the unmodified
# tree, and every case below that expects it to fail passes without testing
# anything. The expect_ok before the documentation cases guards that.
git -C "$WORK" init -q && git -C "$WORK" add -A

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

# expect_ok <description> <command...>
# The mirror of expect_fail. A gate that rejects everything protects nothing
# either -- it just gets disabled. Used where a violation and its absence are
# both cheap to stage.
expect_ok() {
    local description="$1"
    shift
    local output
    output="$(cd "$WORK" && "$@" 2>&1)"
    local status=$?
    if [[ $status -eq 0 ]]; then
        printf '  PASS  %s\n' "$description"
        PASS=$((PASS + 1))
    else
        printf '  FAIL  %s\n' "$description"
        printf '        Expected success; the gate said: %s\n' \
            "$(echo "$output" | grep -viE '^\s*$' | tail -1 | cut -c1-100)"
        FAIL=$((FAIL + 1))
    fi
}

restore() { tar -C "$REPO" --exclude=build --exclude=.git -cf - "$1" | tar -C "$WORK" -xf -; }

# substitute <file> <sed-expression>
# Applies the expression and FAILS THE SCRIPT if it changed nothing.
#
# A fixture that injects its violation by rewriting real source rots the moment
# that source is reworded: sed matches nothing, the scratch tree stays valid, the
# gate has nothing to reject, and the case reports PASS while testing nothing.
# It has happened twice -- P1's roadmap fixture, and P15a's, when
# smply_internal_options gained a $<BUILD_INTERFACE:> wrapper. A silent no-op is
# the one failure this script must never have, so it is now loud.
substitute() {
    local file="$1"
    local expression="$2"
    local before after
    before="$(cat "$file")"
    sed -i "$expression" "$file"
    after="$(cat "$file")"
    if [[ "$before" == "$after" ]]; then
        echo "FATAL: fixture is stale -- this substitution matched nothing:" >&2
        echo "         file: ${file#"$WORK"/}" >&2
        echo "         sed:  $expression" >&2
        echo "       The gate below would have passed while testing nothing." >&2
        exit 1
    fi
}

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
expect_fail "clang-tidy rejects reinterpret_cast over raw bytes" \
    env SMPLY_LINT_SKIP_CPPCHECK=1 tools/lint.sh build
restore src/version.cpp

# 3b. Three directories are Windows-only and invisible to clang-tidy and
# cppcheck (transports/winrt_ble/, examples/winrt_ble_dfu/ and tests/hil/; see
# tools/lint.sh). An
# exclusion is a hole in a gate, so what has to be proved is that each hole is
# exactly the shape it claims: a DIRECTORY, not the substring "winrt". A filter
# written `grep -v winrt` would also swallow a portable file that merely
# mentions it, and would go on passing -- the silent-skip shape that has cost
# this project six defects.
#
# The decoys are portable files whose names contain "winrt" or "hil", one
# beside each excluded directory so no prefix can quietly widen. An exact filter
# keeps all three and the TU count rises by three; an over-broad filter drops
# them and the count does not move. clang-tidy is stubbed out because only the count matters
# and running it twice over the whole tree costs minutes.
printf '// SPDX-License-Identifier: Apache-2.0\n' > "$WORK/transports/common/winrt_decoy.cpp"
printf '// SPDX-License-Identifier: Apache-2.0\n' > "$WORK/examples/cli_dfu/winrt_decoy.cpp"
printf '// SPDX-License-Identifier: Apache-2.0\n' > "$WORK/tests/support/hil_decoy.cpp"
cat > "$WORK/tidy-stub" <<'STUB'
#!/usr/bin/env bash
if [[ "${1:-}" == "--version" ]]; then echo "stub version 0.0"; fi
exit 0
STUB
chmod +x "$WORK/tidy-stub"
expect_ok "the WinRT lint exclusions are directories, not the substring 'winrt'" \
    bash -c 'expected=$(tools/sources.sh | grep -E "\.(cpp|cc)$" \
                        | grep -Ev "^tests/consumer/|^tests/consumption/" \
                        | grep -Ev "^transports/winrt_ble/|^examples/winrt_ble_dfu/|^tests/hil/" | wc -l)
             actual=$(env SMPLY_LINT_SKIP_CPPCHECK=1 CLANG_TIDY=./tidy-stub tools/lint.sh build 2>&1 \
                        | grep -oE "over [0-9]+ TUs" | grep -oE "[0-9]+")
             [[ -n "$actual" && "$expected" == "$actual" ]]'
rm -f "$WORK/transports/common/winrt_decoy.cpp" "$WORK/examples/cli_dfu/winrt_decoy.cpp" \
      "$WORK/tests/support/hil_decoy.cpp" "$WORK/tidy-stub"

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
substitute "$WORK/cmake/dependencies.cmake" 's|^set(SMPLY_QCBOR_COMMIT .*|set(SMPLY_QCBOR_COMMIT "v1.6.1")|'
expect_fail "check_deps rejects a tag pin instead of a full commit hash" \
    python3 tools/check_deps.py
restore cmake/dependencies.cmake

# 7a. Dependency inventory: the self-declaration exemption is the exact project
# name, not a prefix and not the directory it sits in.
#
# tests/consumption/fetchcontent/ declares smply itself, which check_deps.py
# skips -- nothing is a third-party dependency of itself. This plants a
# declaration in the same file whose name merely *starts with* smply, and
# requires it to be rejected. An exemption widened to a prefix or to that
# directory would let a real dependency in unpinned and uninventoried, which is
# the failure the gate exists to prevent. Same shape as the WinRT lint case
# above: the exclusion is an identity, not a substring.
cat >> "$WORK/tests/consumption/fetchcontent/CMakeLists.txt" <<'EOF'

FetchContent_Declare(smply_extra_dependency
    GIT_REPOSITORY https://example.invalid/x.git)
EOF
expect_fail "the check_deps self-exemption is the exact project name, not a prefix" \
    python3 tools/check_deps.py
restore tests/consumption/fetchcontent/CMakeLists.txt

# 7b. The documentation gate passes on the unmodified tree. Every documentation
# case below expects a failure, so without this a gate that failed on
# everything would make them all pass.
expect_ok "check_docs passes on the unmodified tree" python3 tools/check_docs.py

# 8. Docs R2: the roadmap is a backlog.
# Each violation is appended, not patched into real content, so no later edit
# to the roadmap can turn a case into a no-op that still reports PASS.
printf '\n| ~~A finished item, struck through instead of deleted~~ | - |\n' >> "$WORK/docs/roadmap.md"
expect_fail "check_docs R2 rejects a struck-through roadmap row" \
    python3 tools/check_docs.py
restore docs/roadmap.md

printf '\nThis depends on open question O99.\n' >> "$WORK/docs/architecture.md"
expect_fail "check_docs R2 rejects a citation of an undefined open question" \
    python3 tools/check_docs.py
restore docs/architecture.md

# 9. Docs R3: reference to a non-existent ADR
printf '\nSee [ADR-0099](decisions/ADR-0099-imaginary.md).\n' >> "$WORK/docs/architecture.md"
expect_fail "check_docs R3 rejects a reference to a non-existent ADR" \
    python3 tools/check_docs.py
restore docs/architecture.md

# 9b. Docs R3: an ADR the index does not list.
printf '# ADR-0999 -- Synthetic\n\n**Status:** Accepted\n' > "$WORK/docs/decisions/ADR-0999-synthetic.md"
expect_fail "check_docs R3 rejects an ADR missing from the index" \
    python3 tools/check_docs.py
rm -f "$WORK/docs/decisions/ADR-0999-synthetic.md"

# 10. Docs R3: invalid ADR status
substitute "$WORK/docs/decisions/ADR-0001-cpp-standard.md" \
    's|^\*\*Status:\*\* Accepted.*|**Status:** Probably fine|'
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

# 11a. Docs R5: a layout entry naming a file that does not exist.
#
# Inserted into the layout tree with the tree's own glyphs, because that is what
# R5 parses. Note what this case really guards: R5 *skips* anything it cannot
# read as a single path, so a regression that broke its parsing would skip
# everything and still report a pass. This decoy fails only while R5 is
# genuinely reading entries.
python3 - "$WORK/docs/architecture.md" <<'PY'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); t = p.read_text(encoding="utf-8")
needle = "\u251c\u2500\u2500 tools/"
assert t.count(needle) == 1, "the layout tree's tools/ entry moved"
t = t.replace(needle, "\u251c\u2500\u2500 no_such_directory_for_r5/\n"
                      + needle, 1)
p.write_text(t, encoding="utf-8")
PY
expect_fail "check_docs R5 rejects a layout entry that does not exist" \
    python3 tools/check_docs.py
restore docs/architecture.md

# 11a-bis. Docs R5: a file named in SECOND position on a layout line.
#
# The distinct case, and the reason it exists. R5 read only the first token on
# an entry line until P18, so every file listed beside another -- most of the
# tree -- went unchecked *and* uncounted, and two entries naming files that do
# not exist sat under a passing gate for four phases. 11a above would still
# pass with that defect restored; this one would not.
python3 - "$WORK/docs/architecture.md" <<'PY2'
import sys, pathlib
p = pathlib.Path(sys.argv[1]); t = p.read_text(encoding="utf-8")
needle = "\u251c\u2500\u2500 tools/                      format.sh"
assert t.count(needle) == 1, "the layout tree's tools/ entry moved"
t = t.replace(needle, needle + "  no_such_file_for_r5.py", 1)
p.write_text(t, encoding="utf-8")
PY2
expect_fail "check_docs R5 reads past the first token on a layout line" \
    python3 tools/check_docs.py
restore docs/architecture.md

# 11b. Docs R6: a development-phase ID in a living document.
printf '\nThis was added in P99.\n' >> "$WORK/docs/architecture.md"
expect_fail "check_docs R6 rejects a development-phase ID in a living document" \
    python3 tools/check_docs.py
restore docs/architecture.md

# 12 and 13. Consumer flag-leak guard, at configure time and at compile time.
# Both layers are checked: the configure-time assertion gives the good error
# message, the compile of tests/consumer is the ground truth behind it.
# The real line links it PRIVATE *and* wraps it in $<BUILD_INTERFACE:> so the
# install export is possible (P15a). The violation drops both.
substitute "$WORK/CMakeLists.txt" \
    's|target_link_libraries(smply PRIVATE .*smply_internal_options.*)|target_link_libraries(smply PUBLIC smply_internal_options)|'

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


# 17. The SBOM inventory.
#
# quality-gates.md section 9 promised an SBOM from P0 and nothing produced one
# until P18, so the interesting failure is not a malformed document -- it is an
# SBOM that silently omits a component, which reads as a clean bill of health.
# tools/sbom.py --check exists for exactly that, and this proves it fires.
cat >> "$WORK/cmake/dependencies.cmake" <<'EOF'

FetchContent_Declare(library_with_no_licence_entry
    GIT_REPOSITORY https://example.invalid/x.git
    GIT_TAG        0123456789abcdef0123456789abcdef01234567)
EOF
expect_fail "sbom.py --check rejects a dependency with no licence entry" \
    python3 tools/sbom.py --check
restore cmake/dependencies.cmake

# 18. The export.
#
# The P15a defect this guards against: install(EXPORT) names an exported target
# <namespace><target-name>, so a target without EXPORT_NAME ships under a name
# no consumer says, while everything in this repository still builds -- the
# in-tree ALIAS resolves. It configured, built and installed perfectly and
# failed only under find_package from a separate project.
#
# Here the violation is the blunter form of the same thing: a target the
# package documents but does not export. The consumer's configure must fail.
#
# Only the negative arm is staged. The positive one -- all three consumption
# modes building and RUNNING against a real install -- is the install-check job
# in ci.yml, which runs on every push and is not optional, so staging it twice
# would double the slowest thing in this script for no extra evidence.
if command -v ninja > /dev/null 2>&1; then
    EXPORTPROBE="$WORK/build-export"
    substitute "$WORK/transports/CMakeLists.txt" \
        's|^install(TARGETS smply_transport_common EXPORT smplyTargets)$|# removed by verify_gates.sh|'

    export_probe() {
        cmake -G Ninja -S "$WORK" -B "$EXPORTPROBE" \
            -DCMAKE_BUILD_TYPE=Release \
            -DSMPLY_BUILD_TESTS=OFF -DSMPLY_BUILD_EXAMPLES=OFF \
            ${DEPS_CACHE:+-DFETCHCONTENT_BASE_DIR="$DEPS_CACHE"} &&
            cmake --build "$EXPORTPROBE" &&
            cmake --install "$EXPORTPROBE" --prefix "$WORK/export-prefix" &&
            cmake -G Ninja -S "$WORK/tests/consumption/find_package" \
                -B "$WORK/build-export-consumer" \
                -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_PREFIX_PATH="$WORK/export-prefix"
    }

    expect_fail "an unexported target is rejected by the find_package consumer" \
        bash -c "$(declare -f export_probe); export_probe > '$SCRATCH/export-probe.log' 2>&1"

    restore transports/CMakeLists.txt
    rm -rf "$EXPORTPROBE" "$WORK/export-prefix" "$WORK/build-export-consumer"
else
    printf '  SKIP  the export (needs ninja)\n'
fi

# 14, 15 and 16. The coverage reporter.
#
# This is the gate with the worst history in the project: from P0 to P7 it
# passed a directory in the position gcovr reads as an output filename, printed
# nothing, and exited 0 -- so CI reported a coverage gate that had never
# measured anything. Every other check in this script proves a *checker*
# rejects a violation; none of them covered the reporter, which is exactly how
# that survived seven phases.
#
# A real coverage build of the scratch tree would cost minutes. It is not
# needed: what has to be proven is that coverage.sh distinguishes below the
# threshold from above it, and refuses to answer when it cannot measure. A
# two-branch probe compiled with --coverage gives all three answers in about a
# second, and it lives under src/ so the script's own filter picks it up.
if command -v gcovr >/dev/null 2>&1 && command -v g++ >/dev/null 2>&1; then
    COVPROBE="$WORK/build-covprobe"
    mkdir -p "$COVPROBE"

    # take_branch is called with true only, so the false arm and the line it
    # guards stay uncovered: 3 of 4 lines and 1 of 2 branches.
    cat > "$WORK/src/coverage_probe.cpp" <<'PROBE'
// SPDX-License-Identifier: Apache-2.0
// Written by tools/verify_gates.sh into a throwaway copy of the tree; never
// present in the working tree.
namespace smply {
int take_branch(bool go, int x);
int take_branch(bool go, int x)
{
    if (go) {
        return x + 1;
    }
    return x - 1;
}
} // namespace smply
int main()
{
    return smply::take_branch(true, -1);
}
PROBE

    # No -fprofile-dir: it mangles the .gcda path so that gcovr cannot infer a
    # working directory and gives up with an error rather than a number. Left
    # to itself, gcc writes the .gcda beside the .gcno, which is where the -o
    # path puts it -- exactly what gcovr expects.
    build_probe() {
        rm -rf "$COVPROBE"
        mkdir -p "$COVPROBE"
        (cd "$WORK" && g++ --coverage -O0 -o build-covprobe/probe src/coverage_probe.cpp) \
            >> "$SCRATCH/covprobe-build.log" 2>&1 &&
            (cd "$WORK" && ./build-covprobe/probe > /dev/null 2>&1; true) &&
            find "$COVPROBE" -name '*.gcda' | grep -q .
    }

    if build_probe; then

        expect_fail "coverage.sh --enforce rejects coverage below the thresholds" \
            tools/coverage.sh build-covprobe --enforce

        # And is not simply always-red: the same reporter passes once the
        # thresholds are met. Reaching both arms covers every line and branch.
        sed -i 's|return smply::take_branch(true, -1);|return smply::take_branch(true, -1) + smply::take_branch(false, 1);|' \
            "$WORK/src/coverage_probe.cpp"
        build_probe

        expect_ok "coverage.sh --enforce accepts coverage above the thresholds" \
            tools/coverage.sh build-covprobe --enforce

        # Without gcovr the measurement is a different one, so --enforce must
        # refuse rather than enforce the documented threshold against a number
        # that does not mean the same thing. Only gcovr is hidden -- env -i
        # would drop the compiler too, and the check would pass for the wrong
        # reason.
        GCOVR_DIR="$(dirname "$(command -v gcovr)")"
        NO_GCOVR_PATH="$(echo "$PATH" | tr ':' '\n' | grep -vxF "$GCOVR_DIR" | paste -sd:)"
        expect_fail "coverage.sh --enforce refuses to pass without gcovr" \
            env PATH="$NO_GCOVR_PATH" tools/coverage.sh build-covprobe --enforce
    else
        printf '  SKIP  coverage reporter (the --coverage probe did not build)\n'
        tail -5 "$SCRATCH/covprobe-build.log"
    fi

    rm -f "$WORK/src/coverage_probe.cpp"
    rm -rf "$COVPROBE"
else
    printf '  SKIP  coverage reporter (needs gcovr and g++; pip install gcovr)\n'
fi

echo
echo "=== $PASS gate(s) verified, $FAIL not protecting anything ==="
[[ $FAIL -eq 0 ]] || exit 1
