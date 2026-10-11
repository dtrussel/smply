#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Ask OSV about every pinned dependency, by the commit the build pins.

This is the dependency scan `quality-gates.md` section 9 describes, run weekly
by `.github/workflows/osv.yml`.

**Why not OSV-Scanner.** smply's two dependencies are consumed from git, so the
SBOM can identify them only as `pkg:github/<owner>/<repo>@<commit>`.
OSV-Scanner reads those PURLs, counts the packages, and then drops every one of
them as "local/unscannable": the run of 2026-10-05 logged "found 2 packages"
and "Filtered 2 local/unscannable package/s", and reported success. Every green
scan before this script checked nothing.

**What this does instead.** OSV indexes C and C++ projects by commit: an
advisory in the `GIT` ecosystem lists commit ranges, and `POST /v1/querybatch`
with `{"commit": "<sha>"}` answers whether a commit falls in one
(https://google.github.io/osv.dev/post-v1-querybatch/). The commit is exactly
what `cmake/dependencies.cmake` pins and what the SBOM records, so nothing is
lost in translation.

**How it cannot pass while checking nothing.** It fails when:
  * the SBOM yields no commit to ask about;
  * the query fails, or its answer is not one result per question;
  * the canary comes back clean. The canary is a commit OSV is known to flag,
    asked in the same batch. A clean answer for it means the commit query has
    stopped finding anything, and then a clean answer for QCBOR means nothing
    either.

A finding against a dependency does **not** fail the run. The scan is advisory:
acting on a disclosure is a decision (ADR-0011). It is printed as a GitHub
warning, written to the step summary, and kept in the JSON report.

Usage:
    tools/osv_query.py smply.spdx.json [-o osv-report.json]
    tools/osv_query.py smply.spdx.json --response FILE
        answer from a saved querybatch response instead of the network;
        tools/verify_gates.sh uses it to prove each failure fires
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import sys
import urllib.error
import urllib.request

QUERYBATCH = "https://api.osv.dev/v1/querybatch"

# A commit OSV's own documentation queries by commit as its worked example
# (https://google.github.io/osv.dev/post-v1-query/), so OSV is known to flag
# it. If OSV ever stops doing so, this run fails and says why, and the canary
# is replaced by another commit inside a GIT-ecosystem advisory's range.
CANARY_COMMIT = "6879efc2c1596d11a6a6ad296f80063b558d5e0f"

TIMEOUT_SECONDS = 60


class ScanError(Exception):
    """The scan could not establish anything. Always a failure."""


def pinned_commits(sbom: dict) -> list[tuple[str, str]]:
    """(name, commit) for every package the SBOM records a commit for.

    `tools/sbom.py` records the pin as the package's SHA1 checksum, because the
    commit is what determines the bytes. smply's own package has none.
    """
    pinned = []
    for package in sbom.get("packages", []):
        for checksum in package.get("checksums", []):
            if checksum.get("algorithm") == "SHA1":
                pinned.append((package.get("name", "?"), checksum["checksumValue"]))
    return pinned


def ask(commits: list[str]) -> dict:
    body = json.dumps({"queries": [{"commit": c} for c in commits]}).encode()
    request = urllib.request.Request(
        QUERYBATCH, data=body, headers={"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT_SECONDS) as reply:
            return json.load(reply)
    except (urllib.error.URLError, OSError, ValueError) as error:
        raise ScanError(f"the OSV query failed: {error}") from error


def vulns_per_query(response: dict, expected: int) -> list[list[str]]:
    """The advisory IDs answered for each question, in the order asked."""
    results = response.get("results")
    if not isinstance(results, list) or len(results) != expected:
        got = len(results) if isinstance(results, list) else "no"
        raise ScanError(f"OSV answered {got} result(s) to {expected} question(s)")
    answered = []
    for result in results:
        vulns = result.get("vulns", []) if isinstance(result, dict) else None
        if not isinstance(vulns, list):
            raise ScanError(f"an OSV result is not shaped as documented: {result!r}")
        answered.append([v.get("id", "?") for v in vulns if isinstance(v, dict)])
    return answered


def scan(sbom: dict, response: dict | None) -> dict:
    pinned = pinned_commits(sbom)
    if not pinned:
        raise ScanError(
            "the SBOM pins no commit, so there is nothing to ask OSV about; "
            "a clean report would mean nothing"
        )

    commits = [commit for _, commit in pinned] + [CANARY_COMMIT]
    if response is None:
        response = ask(commits)
    answered = vulns_per_query(response, len(commits))

    canary = answered[-1]
    if not canary:
        raise ScanError(
            f"the canary commit {CANARY_COMMIT} came back clean. OSV is known to "
            "flag it, so the commit query is finding nothing and a clean answer "
            "for the dependencies proves nothing. Replace the canary if OSV has "
            "withdrawn its advisory; otherwise the query itself is broken"
        )

    return {
        "packages": [
            {"name": name, "commit": commit, "vulns": vulns}
            for (name, commit), vulns in zip(pinned, answered)
        ],
        "canary": {"commit": CANARY_COMMIT, "vulns": canary},
    }


def summarise(report: dict) -> list[str]:
    lines = [
        "| Dependency | Commit | Advisories |",
        "| ---------- | ------ | ---------- |",
    ]
    for package in report["packages"]:
        found = ", ".join(package["vulns"]) or "none"
        lines.append(f"| {package['name']} | `{package['commit'][:12]}` | {found} |")
    canary = report["canary"]
    lines.append("")
    lines.append(
        f"Canary `{canary['commit'][:12]}` flagged by {len(canary['vulns'])} "
        "advisory(ies): the commit query is live."
    )
    return lines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("sbom", type=pathlib.Path, help="the SPDX SBOM from tools/sbom.py")
    parser.add_argument("-o", "--output", type=pathlib.Path, help="write the JSON report here")
    parser.add_argument(
        "--response", type=pathlib.Path, help="a saved querybatch response, instead of the network"
    )
    args = parser.parse_args()

    try:
        sbom = json.loads(args.sbom.read_text())
        response = json.loads(args.response.read_text()) if args.response else None
        report = scan(sbom, response)
    except (OSError, ValueError, ScanError) as error:
        print(f"osv_query: FAILED: {error}", file=sys.stderr)
        return 1

    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + "\n")

    lines = summarise(report)
    print("\n".join(lines))
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as out:
            out.write("## OSV, by pinned commit\n\n" + "\n".join(lines) + "\n")

    for package in report["packages"]:
        for vuln in package["vulns"]:
            # Advisory: surfaced, not failed (ADR-0011).
            print(f"::warning::{package['name']} at {package['commit']} is affected by {vuln}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
