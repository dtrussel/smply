#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Documentation gate (docs/quality-gates.md section 11, ADR-0013).

Enforces that documentation stays a first-class product artefact:

  R1  code changes under include/smply, src/smp, src/dfu or src/groups must be
      accompanied by a docs/ change, unless the PR body carries
      'Docs-Impact: none' with a justification;
  R2  a roadmap phase marked Complete must have an empty "Remaining work";
  R3  every ADR referenced from a doc exists, and every ADR has a valid Status;
  R4  every public symbol declared in include/smply/ has a /// doc comment.

R1 needs a diff base and, for the escape hatch, a pull-request body. Outside a
PR (a plain branch push, or a local run) neither exists; R1 is then skipped
with a message rather than failing. R2-R4 always run.

Usage:
    tools/check_docs.py [--base REF] [--verbose]
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DOCS = REPO / "docs"
DECISIONS = DOCS / "decisions"

# Code whose change plausibly invalidates the architecture or design docs.
DOC_SENSITIVE_PREFIXES = (
    "include/smply/",
    "src/smp/",
    "src/dfu/",
    "src/groups/",
)

VALID_ADR_STATUS = re.compile(
    r"^\*\*Status:\*\*\s+(Proposed|Accepted|Deprecated|Superseded by ADR-\d{4})\b"
)


def git(*args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(REPO), *args], capture_output=True, text=True, check=False
    )
    return result.stdout.strip() if result.returncode == 0 else ""


def pr_body() -> str | None:
    """The pull-request body when running in PR context, else None."""
    event_path = os.environ.get("GITHUB_EVENT_PATH")
    if not event_path or not Path(event_path).is_file():
        return None
    try:
        event = json.loads(Path(event_path).read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return None
    pull_request = event.get("pull_request")
    if not isinstance(pull_request, dict):
        return None
    return pull_request.get("body") or ""


def resolve_base(explicit: str | None) -> str | None:
    if explicit:
        return explicit if git("rev-parse", "--verify", explicit) else None
    base_ref = os.environ.get("GITHUB_BASE_REF")
    if base_ref:
        candidate = f"origin/{base_ref}"
        if git("rev-parse", "--verify", candidate):
            return candidate
    for candidate in ("origin/main", "origin/master"):
        if git("rev-parse", "--verify", candidate):
            return candidate
    return None


def rule_1_docs_accompany_code(base: str | None, verbose: bool) -> list[str]:
    body = pr_body()
    if body is None:
        print("R1: not a pull request -- skipping the docs-accompany-code rule")
        return []
    if base is None:
        print("R1: no diff base available -- skipping the docs-accompany-code rule")
        return []

    changed = [f for f in git("diff", "--name-only", f"{base}...HEAD").splitlines() if f]
    if not changed:
        return []

    code_changed = [f for f in changed if f.startswith(DOC_SENSITIVE_PREFIXES)]
    docs_changed = [f for f in changed if f.startswith("docs/")]
    if verbose:
        print(f"R1: {len(code_changed)} doc-sensitive file(s), {len(docs_changed)} docs file(s)")

    if not code_changed or docs_changed:
        return []

    match = re.search(r"^\s*Docs-Impact:\s*none\s*(.*)$", body, re.M | re.I)
    if match and match.group(1).strip():
        print(f"R1: waived -- Docs-Impact: none ({match.group(1).strip()})")
        return []
    if match:
        return ["'Docs-Impact: none' needs a one-line justification on the same line"]

    return [
        "these files changed without any docs/ change:\n"
        + "\n".join(f"      {f}" for f in code_changed)
        + "\n    Update the affected documentation in this same change (ADR-0013), or add\n"
        "    'Docs-Impact: none <why>' to the PR body."
    ]


def rule_2_roadmap_consistency() -> list[str]:
    roadmap = DOCS / "roadmap.md"
    if not roadmap.is_file():
        return ["docs/roadmap.md is missing"]

    errors: list[str] = []
    text = roadmap.read_text(encoding="utf-8")

    # Phase sections start with '## P<n> — <title>' and carry a status line.
    sections = re.split(r"^##\s+(P\d+)\s*[—-]\s*", text, flags=re.M)
    # sections = [preamble, id, body, id, body, ...]
    for i in range(1, len(sections) - 1, 2):
        phase_id, body = sections[i], sections[i + 1]
        status = re.search(r"\*\*Status:\s*([A-Za-z ]+?)\*\*", body)
        if not status:
            errors.append(f"roadmap phase {phase_id} has no '**Status: ...**' line")
            continue
        if status.group(1).strip() != "Complete":
            continue
        remaining = re.search(
            r"\*\*Remaining(?: in this phase)?\.?\*\*\s*(.*?)(?=\n\*\*|\n##|\Z)",
            body,
            re.S,
        )
        if remaining:
            content = remaining.group(1).strip()
            if content and content.lower() not in {"none.", "none", "n/a", "n/a."}:
                errors.append(
                    f"roadmap phase {phase_id} is marked Complete but still lists "
                    f"remaining work: {content.splitlines()[0][:80]!r}"
                )
    return errors


def rule_3_adrs() -> list[str]:
    errors: list[str] = []
    if not DECISIONS.is_dir():
        return ["docs/decisions/ is missing"]

    adr_files = sorted(DECISIONS.glob("ADR-*.md"))
    existing_ids = {f.name[:8] for f in adr_files}  # 'ADR-0001'

    for adr in adr_files:
        text = adr.read_text(encoding="utf-8")
        if not any(VALID_ADR_STATUS.match(line) for line in text.splitlines()):
            errors.append(
                f"{adr.relative_to(REPO)}: no valid '**Status:** ...' line "
                "(Proposed | Accepted | Deprecated | Superseded by ADR-NNNN)"
            )
        superseded = re.search(r"\*\*Status:\*\*\s+Superseded by (ADR-\d{4})", text)
        if superseded and superseded.group(1) not in existing_ids:
            errors.append(
                f"{adr.relative_to(REPO)}: superseded by {superseded.group(1)}, which does not exist"
            )

    # Every ADR-NNNN referenced anywhere in docs/ must exist.
    for doc in sorted(DOCS.rglob("*.md")):
        for match in re.finditer(r"\b(ADR-\d{4})\b", doc.read_text(encoding="utf-8")):
            if match.group(1) not in existing_ids:
                errors.append(
                    f"{doc.relative_to(REPO)}: references {match.group(1)}, which does not exist"
                )
    return errors


def rule_4_public_symbols_documented() -> list[str]:
    """Each public declaration needs a preceding /// comment.

    Deliberately conservative: only namespace-scope declarations in
    include/smply/ are considered, and anything in a `detail` namespace (nested
    or not) is exempt. A doc comment may be separated from its declaration by a
    template parameter list, a requires-clause, attributes or preprocessor
    lines.
    """
    public_dir = REPO / "include" / "smply"
    if not public_dir.is_dir():
        return []

    declaration = re.compile(
        r"^(?:class|struct|enum\s+class|enum|using|constexpr|inline|template|"
        r"\[\[nodiscard\]\]|[A-Za-z_][\w:<>,\s*&]*\s+[A-Za-z_]\w*\s*\()"
    )
    errors: list[str] = []

    for header in sorted(list(public_dir.rglob("*.hpp")) + list(public_dir.rglob("*.hpp.in"))):
        lines = header.read_text(encoding="utf-8").splitlines()
        # Track the brace depth at which a `namespace detail` block was
        # entered, and clear the exemption when we come back out of it.
        # Comparing against zero instead would exempt everything after the
        # block, since the enclosing `namespace smply` never closes until EOF.
        detail_depth: int | None = None
        brace_depth = 0
        for i, raw in enumerate(lines):
            line = raw.strip()
            # Matches both `namespace detail {` and `namespace smply::detail {`.
            entering_detail = (
                re.match(r"^namespace\s+(?:[\w:]+::)?detail\b", line) is not None
            )
            brace_depth += raw.count("{") - raw.count("}")
            if entering_detail and detail_depth is None:
                detail_depth = brace_depth - 1
            elif detail_depth is not None and brace_depth <= detail_depth:
                detail_depth = None
            if detail_depth is not None:
                continue
            if not line or line.startswith(("//", "*", "/*", "#", "}")):
                continue
            # Only namespace scope: a declaration indented inside a class body
            # is documented by its enclosing type.
            if raw.startswith((" ", "\t")):
                continue
            if not declaration.match(line):
                continue
            if line.startswith(("using namespace", "template")) and line.endswith(">"):
                continue
            # Walk back past anything that legitimately sits between a doc
            # comment and the thing it documents: template parameter lists,
            # requires-clauses, attributes and preprocessor lines.
            preceding = ""
            for j in range(i - 1, -1, -1):
                candidate = lines[j].strip()
                if not candidate:
                    continue
                # Skip only lines that cannot themselves be the declaration:
                # a bare template header, a requires-clause, an attribute-only
                # line or a preprocessor directive. A line ending in ';' is a
                # complete declaration even if it starts with '[[nodiscard]]',
                # and must not be skipped over.
                if candidate.endswith((";", "}")):
                    preceding = candidate
                    break
                if re.match(r"^(template\s*<|requires\b|\[\[|#)", candidate):
                    continue
                preceding = candidate
                break
            if not preceding.startswith(("///", "*", "/**", "/*!")):
                errors.append(
                    f"{header.relative_to(REPO)}:{i + 1}: public declaration without a "
                    f"/// doc comment: {line[:70]}"
                )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", help="git ref to diff against for R1")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    all_errors: list[tuple[str, list[str]]] = [
        ("R1 docs accompany code", rule_1_docs_accompany_code(resolve_base(args.base), args.verbose)),
        ("R2 roadmap consistency", rule_2_roadmap_consistency()),
        ("R3 ADR integrity", rule_3_adrs()),
        ("R4 public symbols documented", rule_4_public_symbols_documented()),
    ]

    failed = False
    for rule, errors in all_errors:
        if errors:
            failed = True
            print(f"\n{rule}:", file=sys.stderr)
            for error in errors:
                print(f"    {error}", file=sys.stderr)

    if failed:
        print("\nSee docs/quality-gates.md section 11 and ADR-0013.", file=sys.stderr)
        return 1

    print("documentation gate OK (R1-R4)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
