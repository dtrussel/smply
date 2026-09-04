#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Usage: tools/format.sh [--check]
#   (no args)  reformat smply's sources in place
#   --check    fail if any file differs from the repository style (CI mode)
set -euo pipefail
cd "$(dirname "$0")/.."

CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    echo "error: $CLANG_FORMAT not found" >&2
    exit 1
fi

mapfile -t files < <(tools/sources.sh)
if [[ ${#files[@]} -eq 0 ]]; then
    echo "no sources to format"
    exit 0
fi

if [[ "${1:-}" == "--check" ]]; then
    echo "checking format of ${#files[@]} files with $($CLANG_FORMAT --version)"
    "$CLANG_FORMAT" --dry-run --Werror "${files[@]}"
    echo "format OK"
else
    "$CLANG_FORMAT" -i "${files[@]}"
    echo "formatted ${#files[@]} files"
fi
