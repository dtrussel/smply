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
# Absolute, because clang-tidy resolves a relative -I against the compile
# command's directory rather than against this script's cwd.
REPO="$PWD"

BUILD_DIR="${1:-build/linux-clang}"
if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
    echo "error: $BUILD_DIR/compile_commands.json not found." >&2
    echo "       Configure first, e.g. cmake --preset linux-clang" >&2
    exit 1
fi

status=0

# --- the directories neither analyser can see -------------------------------
#
# The Windows-only code: the WinRT BLE adapter, the example that drives it and
# the hardware cases that drive both against a device.
# clang-tidy is driven from a LINUX compile database, where these translation
# units do not appear at all, so it would fall back to default arguments and die
# on the first <winrt/...> include; cppcheck can parse neither the projection
# headers nor the coroutines. Excluding them is not a judgement that the code
# needs less checking -- it is that these two tools cannot run on it from here.
#
# What still covers it: clang-format (tools/sources.sh feeds it everything), and
# MSVC /W4 /WX in the windows-winrt CI job. Recorded in quality-gates.md.
#
# Each prefix is an exact directory, and tools/verify_gates.sh asserts they
# exclude exactly these and nothing else -- an over-broad filter here (say,
# matching the substring "winrt") would silently stop analysing real code, which
# is the failure shape this project has hit six times.
WINRT_DIRS=(
    "transports/winrt_ble/"
    "examples/winrt_ble_dfu/"
    "tests/hil/"
)

# One extended-regex alternation of anchored prefixes, for grep -E below.
winrt_exclude_pattern() {
    local joined=""
    local dir
    for dir in "${WINRT_DIRS[@]}"; do
        joined+="${joined:+|}^${dir}"
    done
    printf '%s' "$joined"
}
WINRT_EXCLUDE="$(winrt_exclude_pattern)"

# --- clang-tidy (required) --------------------------------------------------
CLANG_TIDY="${CLANG_TIDY:-clang-tidy}"
if command -v "$CLANG_TIDY" >/dev/null 2>&1; then
    # Only translation units, and only ours. Headers are covered via
    # HeaderFilterRegex when they are included by these TUs.
    #
    # tests/consumer/ and tests/consumption/ are excluded for the same reason,
    # and it is not the WinRT one: they are *consumer* projects that the main
    # build deliberately does not add, so neither appears in
    # compile_commands.json. clang-tidy then infers a command from a
    # neighbouring directory, which is wrong in a way that is not the code's
    # fault -- tests/consumption/smoke.cpp includes "common/ble_framing.hpp",
    # which needs an include root only a target linking smply::transport_common
    # carries. And tests/consumer/consumer_check.cpp deliberately contains a
    # narrowing conversion and a C-style cast, because its whole job is to be
    # compiled with -Wall and prove smply's strict set did not leak.
    #
    # Both are covered better elsewhere: check_install.sh compiles and runs
    # smoke.cpp three times per push with real compilers, and consumer_check is
    # a ctest.
    mapfile -t tus < <(tools/sources.sh | grep -E '\.(cpp|cc)$' \
        | grep -Ev '^tests/consumer/|^tests/consumption/' \
        | grep -Ev "$WINRT_EXCLUDE")
    # The fuzz targets are NOT in this build's compile database: SMPLY_BUILD_FUZZERS
    # is on only in linux-clang-fuzz, and the gates job configures linux-clang.
    # clang-tidy therefore *infers* their command from a neighbouring directory,
    # exactly as it does for the two consumer projects above -- but unlike those
    # it still analyses them, under a guessed command that need not carry the
    # include roots they need. A fuzz target that includes a transport header
    # turns that into a hard "file not found"; one that does not is silently
    # linted under flags that are not the flags it compiles with.
    #
    # Naming the project's own include roots explicitly fixes it without making
    # the gate depend on a second build directory that CI does not configure.
    # These are roots every real target already carries, so a duplicate -I is a
    # no-op for the TUs that do have a command; what it cannot do is paper over
    # a missing *system* include, which would still fail here.
    tidy_roots=(
        "--extra-arg=-I$REPO/include"
        "--extra-arg=-I$REPO/src"
        "--extra-arg=-I$REPO/support"
        "--extra-arg=-I$REPO/transports"
        "--extra-arg=-I$REPO/tests/support"
        "--extra-arg=-I$REPO/tests/fuzz"
        "--extra-arg=-I$REPO/$BUILD_DIR/generated"
    )
    if [[ ${#tus[@]} -gt 0 ]]; then
        echo "running $("$CLANG_TIDY" --version | grep -m1 -oE '[Vv]ersion [0-9.]+') over ${#tus[@]} TUs"
        "$CLANG_TIDY" -p "$BUILD_DIR" "${tidy_roots[@]}" "${tus[@]}" || status=1
    fi
else
    echo "error: $CLANG_TIDY not found (required)" >&2
    status=1
fi

# --- cppcheck (complementary; skipped when absent) --------------------------
#
# cppcheck is given the project's include paths, unlike clang-tidy it cannot
# read compile_commands.json for them. Without them it parsed the Catch2 test
# files blind and reported a syntaxError at the first TEST_CASE, which would
# need a blanket `syntaxError:tests/*` suppression.
#
# The include paths alone were not enough. The real cause was the *preprocessor
# configuration*: with no macros pinned, cppcheck explores Catch2's own option
# macros, and in the `CATCH_CONFIG_DISABLE;CATCH_CONFIG_PREFIX_ALL` combination
# Catch2 does not define TEST_CASE at all -- so the file genuinely has no valid
# parse. No build uses that combination. Telling cppcheck those two are
# undefined removes exactly those configurations and nothing else, which is why
# the suppression could be deleted rather than narrowed.
if [[ -n "${SMPLY_LINT_SKIP_CPPCHECK:-}" ]]; then
    # tools/verify_gates.sh sets this: its clang-tidy case only needs the
    # *required* half of this script to reject a violation, and a full cppcheck
    # pass over a scratch copy of the tree adds minutes to every run of it.
    echo "note: SMPLY_LINT_SKIP_CPPCHECK set -- skipping cppcheck"
elif command -v cppcheck >/dev/null 2>&1; then
    echo "running $(cppcheck --version)"

    cppcheck_includes=(-I include -I src -I support -I transports -I tests/support -I tests/component -I tests/fuzz)
    # Catch2 is fetched into the build tree; its generated config header lives
    # beside it. Both are optional -- a source-only checkout still lints.
    for dir in "$BUILD_DIR/_deps/catch2-src/src" \
               "$BUILD_DIR/_deps/catch2-build/generated-includes" \
               "$BUILD_DIR/_deps/qcbor-src/inc"; do
        # An `if`, not `[[ ... ]] &&`: the loop's status is its last command's,
        # and under `set -e` a final iteration whose directory is absent would
        # end the script rather than the loop.
        if [[ -d "$dir" ]]; then
            cppcheck_includes+=(-I "$dir")
        fi
    done

    cppcheck_excludes=()
    for dir in "${WINRT_DIRS[@]}"; do
        cppcheck_excludes+=(-i "$dir")
    done

    cppcheck --enable=warning,performance,portability \
             --std=c++20 \
             --language=c++ \
             --inline-suppr \
             "${cppcheck_includes[@]}" \
             -UCATCH_CONFIG_DISABLE \
             -UCATCH_CONFIG_PREFIX_ALL \
             --suppressions-list=tools/cppcheck-suppressions.txt \
             --error-exitcode=1 \
             --quiet \
             "${cppcheck_excludes[@]}" \
             include src support transports tests || status=1
else
    echo "note: cppcheck not installed -- skipping (CI runs it; see docs/quality-gates.md)"
fi

exit $status
