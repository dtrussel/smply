#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""API-discipline gate (docs/quality-gates.md section 10).

Public headers under include/smply/ must:

  1. not include any third-party or platform header;
  2. not mention forbidden tokens (WinRT, Windows, QCBOR, Catch2);
  3. be self-contained -- each compiles on its own with no other include.

Usage:
    tools/check_public_headers.py [--build-dir DIR] [--cxx COMPILER]

--build-dir supplies the directory holding generated public headers
(<build>/generated/smply/*.hpp). Self-containment checking is skipped, with a
message, when no compiler is available.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PUBLIC_INCLUDE = REPO / "include"
PUBLIC_DIR = PUBLIC_INCLUDE / "smply"

# Include targets that may never appear in a public header. The standard
# library is the only permitted external dependency (ADR-0011).
FORBIDDEN_INCLUDE_PATTERNS = [
    (re.compile(r"^\s*#\s*include\s*[<\"](qcbor|UsefulBuf)", re.I), "QCBOR"),
    (re.compile(r"^\s*#\s*include\s*[<\"](winrt|windows\.h|Windows\.h)", re.I), "Windows/WinRT"),
    (re.compile(r"^\s*#\s*include\s*[<\"]catch2/", re.I), "Catch2"),
    (re.compile(r"^\s*#\s*include\s*[<\"](boost|fmt|nlohmann|QtCore)/", re.I), "third-party"),
]

# Bare tokens that betray a leaked platform dependency even without an include.
FORBIDDEN_TOKENS = [
    (re.compile(r"\bwinrt\s*::"), "winrt:: namespace"),
    (re.compile(r"\bQCBOR[A-Za-z_]*\b"), "QCBOR type"),
    (re.compile(r"\bUsefulBuf[A-Za-z_]*\b"), "QCBOR UsefulBuf type"),
    (re.compile(r"\bHRESULT\b|\bLPCWSTR\b|\bDWORD\b"), "Windows type"),
    (re.compile(r"#\s*ifdef\s+_WIN32|#\s*if\s+defined\s*\(\s*_WIN32"), "_WIN32 conditional"),
]


def public_headers(build_dir: Path | None) -> list[Path]:
    headers = sorted(PUBLIC_DIR.rglob("*.hpp")) if PUBLIC_DIR.is_dir() else []
    if build_dir:
        gen = build_dir / "generated" / "smply"
        if gen.is_dir():
            headers += sorted(gen.rglob("*.hpp"))
    return headers


def scan_text(paths: list[Path]) -> list[str]:
    """Rules 1 and 2. Also scans .in templates, which never get compiled."""
    errors: list[str] = []
    for path in paths:
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except UnicodeDecodeError:
            errors.append(f"{path}: not valid UTF-8")
            continue
        for lineno, line in enumerate(lines, 1):
            if line.lstrip().startswith("//"):
                continue  # a comment may legitimately name these
            for pattern, what in FORBIDDEN_INCLUDE_PATTERNS:
                if pattern.search(line):
                    errors.append(f"{path}:{lineno}: includes {what}: {line.strip()}")
            for pattern, what in FORBIDDEN_TOKENS:
                if pattern.search(line):
                    errors.append(f"{path}:{lineno}: mentions {what}: {line.strip()}")
    return errors


def check_self_contained(headers: list[Path], cxx: str, build_dir: Path | None) -> list[str]:
    """Rule 3: compile a TU that includes only this header."""
    if not shutil_which(cxx):
        print(f"note: {cxx} not found -- skipping self-containment check")
        return []

    includes = ["-I", str(PUBLIC_INCLUDE)]
    if build_dir and (build_dir / "generated").is_dir():
        includes += ["-I", str(build_dir / "generated")]

    errors: list[str] = []
    with tempfile.TemporaryDirectory() as tmp:
        for header in headers:
            # The include path as a consumer would spell it.
            for root in (PUBLIC_INCLUDE, (build_dir / "generated") if build_dir else None):
                if root and root in header.parents:
                    spelling = header.relative_to(root).as_posix()
                    break
            else:
                spelling = header.name

            tu = Path(tmp) / "tu.cpp"
            tu.write_text(f'#include "{spelling}"\nint main() {{ return 0; }}\n')
            result = subprocess.run(
                [cxx, "-std=c++20", "-fsyntax-only", *includes, str(tu)],
                capture_output=True,
                text=True,
            )
            if result.returncode != 0:
                errors.append(
                    f"{header}: not self-contained (including it alone fails):\n"
                    + "\n".join("    " + line for line in result.stderr.strip().splitlines()[:10])
                )
    return errors


def shutil_which(cmd: str) -> str | None:
    import shutil

    return shutil.which(cmd)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=REPO / "build" / "linux-clang")
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()

    build_dir = args.build_dir if args.build_dir.is_dir() else None

    headers = public_headers(build_dir)
    templates = sorted(PUBLIC_DIR.rglob("*.hpp.in")) if PUBLIC_DIR.is_dir() else []

    if not headers and not templates:
        print("error: no public headers found under include/smply/", file=sys.stderr)
        return 1

    errors = scan_text(headers + templates)
    errors += check_self_contained(headers, args.cxx, build_dir)

    if errors:
        print("Public header discipline violations:\n", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        print(
            f"\n{len(errors)} violation(s). See docs/quality-gates.md section 10.",
            file=sys.stderr,
        )
        return 1

    print(
        f"public headers OK: {len(headers)} header(s) and {len(templates)} template(s) checked"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
