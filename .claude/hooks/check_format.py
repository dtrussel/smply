#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Claude Code PostToolUse hook: clang-format check of the one file just edited.

Reads the hook's JSON from stdin. When the edited file is one of smply's own
C++ sources (the set tools/sources.sh defines), runs
`clang-format --dry-run --Werror` on that file alone with the repository's
.clang-format, and reports any difference back to Claude. It never rewrites a
file: fixing is `tools/format.sh` or an edit, and CI's format job stays the
authority (docs/quality-gates.md section 4).

Exit status, as Claude Code reads it for PostToolUse:
  0  nothing to say (not a C++ source, or formatted correctly), or a note in
     JSON `additionalContext` (clang-format missing or failing to run);
  2  formatting differs: stderr goes to Claude beside the tool result.

`CLANG_FORMAT` overrides the binary, as it does for tools/format.sh.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path, PurePosixPath

# Mirrors tools/sources.sh: the roots and suffixes of "our code". The
# validation script (.claude/validate.py) checks the two agree.
SOURCE_ROOTS = ("include", "src", "support", "tests", "transports", "examples")
SOURCE_SUFFIXES = (".cpp", ".hpp", ".h", ".cc")

MAX_REPORTED_LINES = 40


def project_dir() -> Path:
    env = os.environ.get("CLAUDE_PROJECT_DIR")
    return Path(env) if env else Path(__file__).resolve().parent.parent.parent


def first_party_source(path: Path, root: Path) -> str | None:
    """The repo-relative POSIX path if `path` is one of smply's sources."""
    try:
        rel = path.resolve().relative_to(root.resolve())
    except ValueError:
        return None  # outside the project
    posix = PurePosixPath(rel.as_posix())
    if not posix.parts or posix.parts[0] not in SOURCE_ROOTS:
        return None
    if posix.suffix not in SOURCE_SUFFIXES:
        return None
    return posix.as_posix()


def note(text: str) -> int:
    json.dump({"hookSpecificOutput": {"hookEventName": "PostToolUse",
                                      "additionalContext": text}}, sys.stdout)
    return 0


def main() -> int:
    try:
        event = json.load(sys.stdin)
    except (json.JSONDecodeError, UnicodeDecodeError):
        return 0  # not a hook payload we understand; stay out of the way
    file_path = (event.get("tool_input") or {}).get("file_path")
    if not isinstance(file_path, str) or not file_path:
        return 0

    root = project_dir()
    path = Path(file_path)
    if not path.is_absolute():
        path = Path(event.get("cwd") or root) / path
    rel = first_party_source(path, root)
    if rel is None or not path.is_file():
        return 0

    clang_format = os.environ.get("CLANG_FORMAT", "clang-format")
    if shutil.which(clang_format) is None:
        return note(f"Format check skipped for {rel}: {clang_format} is not on PATH. "
                    "CI's format job still checks it (tools/format.sh --check).")
    try:
        result = subprocess.run([clang_format, "--dry-run", "--Werror", rel], cwd=root,
                                capture_output=True, text=True, timeout=30, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return note(f"Format check of {rel} could not run: {exc}. "
                    "tools/format.sh --check is the authoritative check.")
    if result.returncode == 0:
        return 0

    lines = (result.stderr or result.stdout).splitlines()
    if not any("-Wclang-format-violations" in line for line in lines):
        # A clang-format failure that is not a formatting verdict (a bad
        # .clang-format option for this version, say): report, don't block.
        return note(f"clang-format failed on {rel} (exit {result.returncode}): "
                    + " | ".join(lines[:5]))
    shown = lines[:MAX_REPORTED_LINES]
    more = len(lines) - len(shown)
    print(f"{rel} does not match .clang-format (clang-format --dry-run --Werror):",
          file=sys.stderr)
    print("\n".join(shown), file=sys.stderr)
    if more > 0:
        print(f"... {more} more line(s)", file=sys.stderr)
    print(f"Fix the reported lines, or run `{clang_format} -i {rel}` on this file only. "
          "Do not reformat other files.", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
