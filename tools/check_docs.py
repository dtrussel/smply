#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Documentation gate (docs/quality-gates.md section 11, ADR-0013).

Enforces that documentation stays a first-class product artefact:

  R1  code changes under include/smply, src/smp, src/dfu or src/groups must be
      accompanied by a docs/ change, unless the PR body carries
      'Docs-Impact: none' with a justification;
  R2  a roadmap phase marked Complete must have an empty "Remaining work";
  R3  every ADR referenced from a doc exists, and every ADR has a valid Status;
  R4  every public symbol declared in include/smply/ has a /// doc comment;
  R5  every path in architecture.md's repository-layout tree exists;
  R6  no "(planned, P<n>)" marker names a phase the roadmap marks Complete.

R1 needs a diff base and, for the escape hatch, a pull-request body. Outside a
PR (a plain branch push, or a local run) neither exists; R1 is then skipped
with a message rather than failing. R2-R6 always run.

R5 and R6 exist because P17c found three wrong entries in a layout section whose
own opening line says "Keep this accurate", and two `(planned)` markers naming
phases that had been Complete for four phases. Nothing checked either, so the
drift was invisible until someone read the file against the tree. Adding a check
for a defect just fixed by hand is this repository's habit, not a new idea: it is
what committing a fuzz reproducer alongside its fix does.

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
    #
    # The trailing letter matters: P14 was split into P14a and P14b, and a
    # pattern of P\d+ alone does not merely miss them -- it fails to match the
    # heading at all, so both sections are silently skipped and R2 stops
    # checking them while still reporting a pass. That is the same failure mode
    # as the P1 fixture that rotted in verify_gates.sh.
    sections = re.split(r"^##\s+(P\d+[a-z]?)\s*[—-]\s*", text, flags=re.M)
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


def _phase_status() -> dict[str, str]:
    """Phase id to Status, from the roadmap. Shared by R2's reader and R6."""
    roadmap = DOCS / "roadmap.md"
    if not roadmap.is_file():
        return {}
    text = roadmap.read_text(encoding="utf-8")
    sections = re.split(r"^##\s+(P\d+[a-z]?)\s*[—-]\s*", text, flags=re.M)
    out: dict[str, str] = {}
    for i in range(1, len(sections) - 1, 2):
        status = re.search(r"\*\*Status:\s*([A-Za-z ]+?)\*\*", sections[i + 1])
        if status:
            out[sections[i]] = status.group(1).strip()
    return out


# A layout-tree line: box-drawing glyphs, then a path, then optional prose after
# two or more spaces.
LAYOUT_LINE = re.compile(r"^[│|\s]*(?:├──|└──|\|--|`--)\s*(\S+)")

# ...and the token has to look like a path. Without this, R5 reads the borders
# of the *other* fenced diagrams in architecture.md as entries: a line such as
# "└─────┬────┘" matches the glyph pattern above and yields a "path" made
# entirely of box-drawing characters. Those lines are not layout entries at all,
# so they are neither checked nor counted as skipped -- inflating the skip count
# with them would hide a real narrowing of the rule, which the count exists to
# expose.
PATHISH = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.+@/{}*-]*$")

_TRACKED: list[str] | None = None


def _tracked_paths() -> list[str]:
    """Every tracked path, as forward-slashed repository-relative strings.

    Read once. Also covers directories, which `git ls-files` does not list, by
    adding every prefix of every file -- so a layout entry naming a directory
    resolves without a second command.
    """
    global _TRACKED
    if _TRACKED is None:
        listing = git("ls-files").splitlines()
        paths = set(listing)
        for path in listing:
            parts = path.split("/")
            for i in range(1, len(parts)):
                paths.add("/".join(parts[:i]))
        _TRACKED = sorted(paths)
    return _TRACKED


def rule_5_layout_tree_exists(verbose: bool = False) -> list[str]:
    """Every path named in architecture.md's layout tree must exist.

    The tree is prose, not data, so this rule is deliberately conservative: it
    checks the first token after the glyphs and **skips anything it cannot read
    as a single path** -- a brace expansion (`upload_session.{hpp,cpp}`), a glob
    (`update_state_machine.*`), a line naming several files, or an entry marked
    `(planned`.

    It prints how many entries it skipped. That number is the point: a rule that
    silently narrows to nothing still reports a pass, which is exactly how
    `verify_gates.sh`'s R2 fixture rotted (roadmap.md, P1 follow-up). A reader
    who sees "checked 3, skipped 60" knows the rule has stopped working; a
    reader who sees "OK" does not.
    """
    architecture = DOCS / "architecture.md"
    if not architecture.is_file():
        return ["docs/architecture.md is missing"]

    text = architecture.read_text(encoding="utf-8")
    # The tree lives in one fenced block; take every fenced block and only look
    # at lines that parse as tree entries, so the rule does not depend on the
    # section's heading text.
    errors: list[str] = []
    checked = skipped = 0
    in_fence = False
    for line in text.splitlines():
        if line.startswith("```"):
            in_fence = not in_fence
            continue
        if not in_fence:
            continue
        match = LAYOUT_LINE.match(line)
        if not match:
            continue
        token = match.group(1)
        if not PATHISH.match(token):
            continue
        if "(planned" in line or any(c in token for c in "{}*?"):
            skipped += 1
            continue
        bare = token.rstrip("/")
        # A directory entry ends in "/"; a file entry does not. Both resolve
        # against the repository root first.
        if (REPO / bare).exists():
            checked += 1
            continue
        # A leaf named without its directories -- `header.hpp` inside a nested
        # branch of the tree -- cannot be resolved without tracking indentation,
        # which is the parsing this rule refuses to do. So it is accepted when
        # something tracked ends with that path.
        #
        # Matched against `git ls-files` rather than a filesystem walk: `build/`
        # here holds dependency checkouts, peer firmware and every past bench
        # run, and a recursive glob through it takes minutes per unresolved
        # token. Tracked files are also the right set -- the layout describes
        # the repository, not whatever a build left behind.
        if any(p == bare or p.endswith("/" + bare) for p in _tracked_paths()):
            checked += 1
        else:
            errors.append(f"architecture.md's layout names {token!r}, which does not exist")
    print(f"R5: checked {checked} layout path(s), skipped {skipped} "
          f"entry(ies) it could not read as a single path")
    if verbose:
        print(f"    (a rising skip count means R5 is checking less than it looks)")
    return errors


def rule_6_no_stale_planned_markers() -> list[str]:
    """A "(planned, P<n>)" marker may not name a phase that is Complete.

    The marker means "this does not exist yet, and P<n> creates it". Once P<n>
    is Complete the marker is either a lie about the tree or a phase that did
    not do what it said, and both are worth a failing gate. It would have fired
    the moment P10 and P12 closed, which is when the two it now catches became
    wrong.
    """
    status = _phase_status()
    if not status:
        return ["docs/roadmap.md could not be read, so R6 cannot run"]

    errors: list[str] = []
    # Both spellings the documents actually use: "(planned, P12)" and
    # "**planned (P10)**". Bounded to a short window after the word, so a
    # sentence that merely mentions a phase some way after "planned" is not
    # swept in -- the marker is a terse annotation, not prose.
    pattern = re.compile(r"\bplanned\b[^\n]{0,24}?\b(P\d+[a-z]?)\b", re.I)
    for path in sorted(DOCS.rglob("*.md")):
        # The roadmap's own phase log records what was planned at the time and
        # is a historical record, not a claim about the tree today.
        if path.name == "roadmap.md":
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            for match in pattern.finditer(line):
                phase = match.group(1)
                if status.get(phase) == "Complete":
                    errors.append(
                        f"{path.relative_to(REPO).as_posix()}:{number} still marks "
                        f"something '(planned, {phase})' although {phase} is Complete: "
                        f"{line.strip()[:70]}")
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
        ("R5 layout tree exists", rule_5_layout_tree_exists(args.verbose)),
        ("R6 no stale (planned) markers", rule_6_no_stale_planned_markers()),
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

    print("documentation gate OK (R1-R6)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
