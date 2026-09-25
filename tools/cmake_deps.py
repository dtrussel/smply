# SPDX-License-Identifier: Apache-2.0
"""Reads FetchContent declarations out of CMake source.

Shared by tools/check_deps.py (every declaration in the tree is inventoried and
pinned) and tools/sbom.py (every declaration in cmake/dependencies.cmake is
described). Each script keeps its own policy; this module is only the parsing,
so the two cannot disagree about what counts as a declaration.
"""

from __future__ import annotations

import re

# A dependency name as CMake accepts it. The wider set matters: a name with a
# dot or a dash that one reader accepted and the other did not would be
# inventoried but missing from the SBOM.
DECLARE_RE = re.compile(r"FetchContent_Declare\s*\(\s*([A-Za-z0-9_.-]+)", re.M)


def strip_comments(text: str) -> str:
    """Removes CMake comments, preserving line numbering.

    A comment that merely mentions FetchContent_Declare(...) -- documentation
    does this legitimately -- must not be mistaken for a declaration.
    """
    out = []
    for line in text.splitlines():
        in_string = False
        cut = len(line)
        for i, ch in enumerate(line):
            if ch == '"' and (i == 0 or line[i - 1] != "\\"):
                in_string = not in_string
            elif ch == "#" and not in_string:
                cut = i
                break
        out.append(line[:cut])
    return "\n".join(out)


def declared_names(text: str) -> list[str]:
    """Every FetchContent_Declare name in `text`, in order, ignoring comments."""
    return DECLARE_RE.findall(strip_comments(text))
