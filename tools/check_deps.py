#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Dependency-inventory gate (docs/quality-gates.md section 9).

Every dependency declared to CMake must:

  1. appear by name in docs/dependencies.md;
  2. be pinned to an exact 40-character commit hash, not a tag or a branch.

Rule 2 is what makes the build reproducible and the SBOM meaningful: a tag can
be moved, a commit hash cannot.

Usage:
    tools/check_deps.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
INVENTORY = REPO / "docs" / "dependencies.md"

DECLARE_RE = re.compile(r"FetchContent_Declare\s*\(\s*([A-Za-z0-9_.-]+)", re.M)
GIT_TAG_RE = re.compile(r"GIT_TAG\s+(\S+)")
FULL_HASH_RE = re.compile(r"^[0-9a-f]{40}$")


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


def cmake_files() -> list[Path]:
    files = [REPO / "CMakeLists.txt"]
    files += sorted((REPO / "cmake").glob("*.cmake"))
    for sub in ("tests", "transports", "examples", "src"):
        base = REPO / sub
        if base.is_dir():
            files += sorted(base.rglob("CMakeLists.txt"))
    return [f for f in files if f.is_file()]


def declared_dependencies() -> dict[str, list[tuple[Path, str]]]:
    """Maps dependency name -> list of (file, resolved GIT_TAG value)."""
    found: dict[str, list[tuple[Path, str]]] = {}
    for path in cmake_files():
        text = strip_comments(path.read_text(encoding="utf-8"))
        for match in DECLARE_RE.finditer(text):
            name = match.group(1)
            # The GIT_TAG belonging to this declaration: search from the
            # declaration to the closing parenthesis.
            end = text.find(")", match.end())
            block = text[match.end() : end if end != -1 else len(text)]
            tag_match = GIT_TAG_RE.search(block)
            raw = tag_match.group(1) if tag_match else ""
            # Resolve a ${VAR} indirection against a set() in the same file.
            var = re.fullmatch(r"\$\{([A-Za-z0-9_]+)\}", raw)
            if var:
                setter = re.search(
                    r'set\s*\(\s*' + re.escape(var.group(1)) + r'\s+"?([^"\s)]+)"?', text
                )
                raw = setter.group(1) if setter else raw
            found.setdefault(name, []).append((path, raw))
    return found


def main() -> int:
    if not INVENTORY.is_file():
        print(f"error: {INVENTORY} is missing", file=sys.stderr)
        return 1

    inventory = INVENTORY.read_text(encoding="utf-8").lower()
    declared = declared_dependencies()

    if not declared:
        print("no FetchContent dependencies declared")
        return 0

    errors: list[str] = []
    for name, occurrences in sorted(declared.items()):
        if name.lower() not in inventory:
            errors.append(
                f"'{name}' is declared to CMake but is absent from docs/dependencies.md "
                f"(declared in {occurrences[0][0].relative_to(REPO)})"
            )
        for path, tag in occurrences:
            if not tag:
                errors.append(f"'{name}' in {path.relative_to(REPO)} has no GIT_TAG")
            elif not FULL_HASH_RE.match(tag):
                errors.append(
                    f"'{name}' in {path.relative_to(REPO)} is pinned to '{tag}', "
                    "not a full 40-character commit hash"
                )

    if errors:
        print("Dependency inventory violations:\n", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        print(
            f"\n{len(errors)} violation(s). See docs/quality-gates.md section 9.",
            file=sys.stderr,
        )
        return 1

    print(f"dependency inventory OK: {', '.join(sorted(declared))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
