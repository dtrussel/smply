#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Prints smply's own C++ sources, one per line. The single definition of "our
# code" shared by the formatting and lint gates -- never touches _deps/ or
# build/.
#
# Directories that do not exist yet (transports/, added in a later roadmap
# phase) are skipped rather than treated as an error.
#
# ADD NEW TOP-LEVEL SOURCE DIRECTORIES HERE. A directory missing from this list
# is not an error and produces no warning -- the format and lint gates simply
# never see it, and go on passing. support/ was added in P14b for exactly that
# reason.
set -euo pipefail
cd "$(dirname "$0")/.."

roots=()
for d in include src support tests transports examples; do
    [[ -d "$d" ]] && roots+=("$d")
done
[[ ${#roots[@]} -eq 0 ]] && exit 0

find "${roots[@]}" -type f \
    \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.cc' \) \
    | sort
