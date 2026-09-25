#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate an SPDX 2.3 SBOM for smply, and check it stays honest.

This is the SBOM `quality-gates.md` section 9 describes.

The inventory is read from the same two places the dependency gate reads, so an
SBOM cannot disagree with the build:

  * `cmake/dependencies.cmake` -- the FetchContent declarations and their exact
    tag + commit pins, which are what actually ends up in the binary.
  * `CMakeLists.txt` -- `project(... VERSION ...)`, for smply's own package.

Licences are not inferred. They live in LICENCES below, beside the name, and
`--check` fails when a dependency appears in the build with no entry here -- so
adding a dependency without deciding its licence fails the gate rather than
producing an SBOM with a guess in it.

Usage:
    tools/sbom.py                     write the SBOM to stdout
    tools/sbom.py -o sbom.spdx.json   write it to a file
    tools/sbom.py --check             verify every declared dependency is
                                      covered, and exit non-zero if not
"""

from __future__ import annotations

import argparse
import datetime
import json
import pathlib
import re
import sys

from cmake_deps import declared_names

REPO = pathlib.Path(__file__).resolve().parent.parent

# Licence and origin per dependency, keyed by the FetchContent_Declare name,
# lowercased. Deliberately hand-maintained: a licence is a decision, and
# guessing one from a repository would put a guess in a compliance artefact.
LICENCES = {
    "qcbor": {
        "licence": "BSD-3-Clause",
        "supplier": "Organization: Laurence Lundblade",
        "url": "https://github.com/laurencelundblade/QCBOR",
        "purl": "pkg:github/laurencelundblade/qcbor",
        # Linked into libsmply.a, so it reaches a consumer's binary.
        "shipped": True,
    },
    "catch2": {
        "licence": "BSL-1.0",
        "supplier": "Organization: Catch2 contributors",
        "url": "https://github.com/catchorg/Catch2",
        "purl": "pkg:github/catchorg/catch2",
        # Test-only: never linked into anything installed.
        "shipped": False,
    },
}

PROJECT_VERSION = re.compile(r"project\(\s*smply\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)")


def declared_dependencies(text: str) -> list[str]:
    """Every FetchContent_Declare name in dependencies.cmake, in order."""
    return declared_names(text)


def pin_for(text: str, name: str) -> tuple[str | None, str | None]:
    """The SMPLY_<NAME>_TAG / _COMMIT pair for a dependency, if it has one."""
    upper = name.upper()
    tag = re.search(rf'set\(SMPLY_{upper}_TAG\s+"([^"]+)"\)', text)
    commit = re.search(rf'set\(SMPLY_{upper}_COMMIT\s+"([0-9a-f]{{40}})"\)', text)
    return (tag.group(1) if tag else None, commit.group(1) if commit else None)


def smply_version(text: str) -> str:
    match = PROJECT_VERSION.search(text)
    if match is None:
        raise SystemExit("sbom: could not read project(VERSION) from CMakeLists.txt")
    return match.group(1)


def build(deps_text: str, root_text: str, *, now: str) -> dict:
    version = smply_version(root_text)
    packages = [
        {
            "SPDXID": "SPDXRef-Package-smply",
            "name": "smply",
            "versionInfo": version,
            "downloadLocation": "NOASSERTION",
            "filesAnalyzed": False,
            "licenseConcluded": "Apache-2.0",
            "licenseDeclared": "Apache-2.0",
            "supplier": "NOASSERTION",
            "copyrightText": "NOASSERTION",
        }
    ]
    relationships = [
        {
            "spdxElementId": "SPDXRef-DOCUMENT",
            "relationshipType": "DESCRIBES",
            "relatedSpdxElement": "SPDXRef-Package-smply",
        }
    ]

    for name in declared_dependencies(deps_text):
        key = name.lower()
        info = LICENCES[key]
        tag, commit = pin_for(deps_text, key)
        spdx_id = f"SPDXRef-Package-{key}"
        package = {
            "SPDXID": spdx_id,
            "name": name,
            "versionInfo": tag or "NOASSERTION",
            "downloadLocation": f"git+{info['url']}@{commit}" if commit else info["url"],
            "filesAnalyzed": False,
            "licenseConcluded": info["licence"],
            "licenseDeclared": info["licence"],
            "supplier": info["supplier"],
            "copyrightText": "NOASSERTION",
        }
        if commit:
            # The pin that actually determines the bytes. The tag is a label and
            # can move; the commit cannot.
            package["checksums"] = [{"algorithm": "SHA1", "checksumValue": commit}]

        # A package identifier, without which the document is a list of names.
        #
        # This is not decoration. An SBOM with no PURL and no CPE parses
        # perfectly and is *unusable*: OSV-Scanner read the first version of
        # this file, listed all three packages, reported "found 0 packages" and
        # had nothing to look up -- because a scanner matches advisories on the
        # identifier, not on the name. The commit is the version, since that is
        # what the build actually pins.
        purl = info.get("purl")
        if purl:
            package["externalRefs"] = [
                {
                    "referenceCategory": "PACKAGE-MANAGER",
                    "referenceType": "purl",
                    "referenceLocator": f"{purl}@{commit or tag or 'NOASSERTION'}",
                }
            ]
        packages.append(package)

        # DEPENDENCY_OF for what is linked in, BUILD_DEPENDENCY_OF for what is
        # not. A consumer scanning this needs to tell the two apart: only the
        # first reaches their binary.
        relationships.append(
            {
                "spdxElementId": spdx_id,
                "relationshipType": (
                    "DEPENDENCY_OF" if info["shipped"] else "BUILD_DEPENDENCY_OF"
                ),
                "relatedSpdxElement": "SPDXRef-Package-smply",
            }
        )

    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"smply-{version}",
        "documentNamespace": f"https://github.com/dtrussel/smply/spdx/smply-{version}",
        "creationInfo": {
            "created": now,
            "creators": ["Tool: smply-tools-sbom"],
        },
        "packages": packages,
        "relationships": relationships,
    }


def check(deps_text: str) -> int:
    """Fail if the build declares a dependency this script cannot describe.

    This is the half that can go wrong silently: a new FetchContent_Declare with
    no LICENCES entry would otherwise produce an SBOM that is simply missing a
    component, which is worse than having none.
    """
    missing = [n for n in declared_dependencies(deps_text) if n.lower() not in LICENCES]
    if missing:
        print(
            "sbom: no licence entry in tools/sbom.py for: " + ", ".join(sorted(missing)),
            file=sys.stderr,
        )
        return 1

    unidentified = [
        n
        for n in declared_dependencies(deps_text)
        if not LICENCES[n.lower()].get("purl")
    ]
    if unidentified:
        print(
            "sbom: no PURL in tools/sbom.py for: " + ", ".join(sorted(unidentified)),
            file=sys.stderr,
        )
        print(
            "      A component with no package identifier cannot be matched "
            "against any advisory database -- the SBOM would list it and a "
            "scanner would report zero packages.",
            file=sys.stderr,
        )
        return 1

    unpinned = [
        n
        for n in declared_dependencies(deps_text)
        if pin_for(deps_text, n.lower()) == (None, None)
    ]
    if unpinned:
        print("sbom: no tag/commit pin found for: " + ", ".join(sorted(unpinned)), file=sys.stderr)
        return 1

    print(f"sbom: OK, {len(declared_dependencies(deps_text))} dependencies covered")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", help="write the SBOM here instead of stdout")
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the inventory covers the build and exit, writing no SBOM",
    )
    args = parser.parse_args()

    deps_text = (REPO / "cmake" / "dependencies.cmake").read_text(encoding="utf-8")
    root_text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")

    if args.check:
        return check(deps_text)

    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    document = json.dumps(build(deps_text, root_text, now=now), indent=2) + "\n"

    if args.output:
        pathlib.Path(args.output).write_text(document, encoding="utf-8")
        print(f"sbom: wrote {args.output}")
    else:
        sys.stdout.write(document)
    return 0


if __name__ == "__main__":
    sys.exit(main())
