#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""API-discipline gate (docs/quality-gates.md section 10).

Public headers under include/smply/ must:

  1. not include any third-party or platform header;
  2. not mention forbidden tokens (WinRT, Windows, QCBOR, Catch2);
  3. be self-contained -- each compiles on its own with no other include;
  4. include only public headers of a lower layer (PUBLIC_LAYERS).

And the implementation must keep the same shape:

  5. a directory under src/ includes only the internal directories and the
     public layers it is allowed (SOURCE_DEPENDENCIES).

Rules 4 and 5 are architecture.md section 3's dependency diagram, in code:
dependencies point downward only, with no cycles.

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


# Rule 4. Each public header's layer. A header may include headers of a lower
# layer only, except that the core headers (layer 0) may include each other.
# A header missing from this table is an error, so a new one is placed
# deliberately rather than by accident.
PUBLIC_LAYERS: dict[str, int] = {
    "bytes": 0, "clock": 0, "group": 0, "error": 0, "result": 0, "limits": 0,
    "detail/expected": 0, "version": 0,
    "smp/header": 1, "transport": 1, "image_source": 1, "groups/image_upload": 1,
    "mcuboot_image": 2,
    "smp_client": 3,
    "groups/os": 4, "groups/image": 4,
    "dfu/firmware_updater": 5,
    # smply::util and smply::asyncutil are separate targets the core never
    # links (SEPARATE_TARGET_HEADERS below). They build on the core types.
    "util/dispatcher": 1,
    "async/task": 1,
    "async/future": 2,
}

# Headers of the targets libsmply never links. Nothing outside this set may
# include them -- no core header, and no file under src/ except the one that
# implements the header. Otherwise a header-only target would compile its way
# into the core without anything failing to link.
SEPARATE_TARGET_HEADERS = {"util/dispatcher", "async/task", "async/future"}

# Rule 5. For each directory under src/ ("" is src/ itself): the internal
# directories it may include, and the highest public layer it may include.
# Its own public header is always allowed, whatever its layer.
SOURCE_DEPENDENCIES: dict[str, tuple[set[str], int]] = {
    "": (set(), 0),
    "detail": ({"detail"}, 0),
    "cbor": ({"cbor"}, 0),
    "smp": ({"smp", "cbor", "detail"}, 3),
    "image": ({"image", "detail"}, 2),
    "groups": ({"groups", "cbor"}, 3),
    "groups/os": ({"groups/os", "groups", "cbor", "detail"}, 4),
    "groups/image": ({"groups/image", "groups", "cbor", "detail"}, 4),
    "dfu": ({"dfu"}, 5),
    "util": ({"util"}, 0),
}
OWN_PUBLIC_HEADER = {"dfu": "dfu/firmware_updater", "util": "util/dispatcher",
                     "groups/os": "groups/os", "groups/image": "groups/image"}

QUOTED_INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')


def _quoted_includes(path: Path) -> list[tuple[int, str]]:
    found = []
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        match = QUOTED_INCLUDE.match(line)
        if match:
            found.append((lineno, match.group(1)))
    return found


def _public_name(include: str) -> str | None:
    """'smply/groups/image.hpp' -> 'groups/image'; None if not a public header."""
    if include.startswith("smply/") and include.endswith(".hpp"):
        return include[len("smply/"):-len(".hpp")]
    return None


def _header_name(header: Path) -> str:
    """include/smply/groups/image.hpp -> 'groups/image'.

    Relative to the *last* `smply` directory, so that a generated header under
    <build>/generated/smply/ is named the same way as one under include/smply/,
    and a checkout that is itself called smply does not confuse it.
    """
    parts = header.parts
    last = len(parts) - 1 - parts[::-1].index("smply")
    return "/".join(parts[last + 1:])[:-len(".hpp")]


def check_public_layers(headers: list[Path]) -> list[str]:
    """Rule 4."""
    errors: list[str] = []
    for header in headers:
        name = _header_name(header)
        if name not in PUBLIC_LAYERS:
            errors.append(f"{header}: has no layer in PUBLIC_LAYERS; place it deliberately")
            continue
        layer = PUBLIC_LAYERS[name]
        for lineno, include in _quoted_includes(header):
            target = _public_name(include)
            if target is None:
                continue
            if target not in PUBLIC_LAYERS:
                errors.append(f"{header}:{lineno}: includes {include}, which has no layer")
            elif target in SEPARATE_TARGET_HEADERS and name not in SEPARATE_TARGET_HEADERS:
                errors.append(
                    f"{header}:{lineno}: includes {include}, a header of a separate target "
                    "the core never links (docs/architecture.md section 5)")
            elif PUBLIC_LAYERS[target] > layer or (PUBLIC_LAYERS[target] == layer and layer != 0):
                errors.append(
                    f"{header}:{lineno}: layer-{layer} header includes {include} "
                    f"(layer {PUBLIC_LAYERS[target]}); dependencies must point downward "
                    "(docs/architecture.md section 3)")
    return errors


def check_source_dependencies() -> list[str]:
    """Rule 5."""
    errors: list[str] = []
    src = REPO / "src"
    for path in sorted(list(src.rglob("*.cpp")) + list(src.rglob("*.hpp"))):
        directory = path.parent.relative_to(src).as_posix()
        directory = "" if directory == "." else directory
        if directory not in SOURCE_DEPENDENCIES:
            errors.append(f"{path}: src/{directory} has no entry in SOURCE_DEPENDENCIES")
            continue
        allowed_dirs, ceiling = SOURCE_DEPENDENCIES[directory]
        for lineno, include in _quoted_includes(path):
            public = _public_name(include)
            if public is not None:
                if public == OWN_PUBLIC_HEADER.get(directory):
                    continue
                if public in SEPARATE_TARGET_HEADERS:
                    errors.append(
                        f"{path}:{lineno}: src/{directory} includes {include}, a header of a "
                        "separate target libsmply never links (docs/architecture.md section 5)")
                    continue
                layer = PUBLIC_LAYERS.get(public)
                if layer is None or layer > ceiling:
                    errors.append(
                        f"{path}:{lineno}: src/{directory} includes {include}, above its "
                        f"public-layer ceiling of {ceiling} (docs/architecture.md section 3)")
                continue
            target_dir = include.rsplit("/", 1)[0] if "/" in include else ""
            if target_dir not in allowed_dirs:
                errors.append(
                    f"{path}:{lineno}: src/{directory} includes {include}; it may use only "
                    f"{sorted(allowed_dirs) or 'no internal directory'} "
                    "(docs/architecture.md section 3)")
    return errors


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
    errors += check_public_layers(headers)
    errors += check_source_dependencies()
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
