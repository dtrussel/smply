#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Prints smply's own C++ sources, one per line. The single definition of "our
# code" shared by the formatting and lint gates -- never touches _deps/ or
# build/.
#
# Directories that do not exist yet (transports/, examples/, added in later
# roadmap phases) are skipped rather than treated as an error.
set -euo pipefail
cd "$(dirname "$0")/.."

roots=()
for d in include src tests transports examples; do
    [[ -d "$d" ]] && roots+=("$d")
done
[[ ${#roots[@]} -eq 0 ]] && exit 0

find "${roots[@]}" -type f \
    \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.cc' \) \
    | sort
