#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Documentation gate (docs/quality-gates.md section 11, ADR-0013).

Enforces that documentation stays a first-class product artefact:

  R1  code changes under include/smply, src/smp, src/dfu or src/groups must be
      accompanied by a docs/ change, unless the PR body carries
      'Docs-Impact: none' with a justification;
  R2  the roadmap is a backlog: its required sections exist, it carries no
      struck-through rows (done items are deleted, not struck), and every
      open-question ID (O<n>) a document cites is defined in its table;
  R3  every ADR referenced from a doc exists, every ADR has a valid Status, and
      every ADR is listed in the index, docs/decisions/README.md;
  R4  every public symbol declared in include/smply/ has a /// doc comment;
  R5  every path in architecture.md's repository-layout tree exists;
  R6  no development-phase ID (P<n>) in a living document -- the history is in
      git (ADR-0018). ADR bodies are exempt: they are immutable records.

R1 needs a diff base and, for the escape hatch, a pull-request body. Outside a
PR (a plain branch push, or a local run) neither exists; R1 is then skipped
with a message rather than failing. R2-R6 always run.

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


ROADMAP_SECTIONS = ("## Current state", "## Open questions", "## Backlog")

# An open-question ID as the documents cite it. The look-behind keeps compiler
# flags (-O2) and identifiers (IO3) out.
QUESTION_ID = re.compile(r"(?<![\w-])(O\d+)\b")


def rule_2_roadmap_consistency() -> list[str]:
    roadmap = DOCS / "roadmap.md"
    if not roadmap.is_file():
        return ["docs/roadmap.md is missing"]

    errors: list[str] = []
    text = roadmap.read_text(encoding="utf-8")
    lines = text.splitlines()

    for heading in ROADMAP_SECTIONS:
        if heading not in lines:
            errors.append(f"docs/roadmap.md has no '{heading}' section")

    # A finished item is deleted, not struck through (ADR-0018). Struck rows
    # are how the old roadmap grew to thousands of lines.
    for number, line in enumerate(lines, 1):
        if line.startswith("|") and "~~" in line:
            errors.append(f"docs/roadmap.md:{number} strikes a row through; delete it instead")

    # The open-questions table defines the IDs. It runs from its heading to the
    # next '## ' heading.
    defined: set[str] = set()
    in_questions = False
    for line in lines:
        if line.startswith("## "):
            in_questions = line == "## Open questions"
            continue
        row = re.match(r"^\|\s*(O\d+)\s*\|", line)
        if in_questions and row:
            defined.add(row.group(1))
    if not defined:
        errors.append("docs/roadmap.md defines no open questions (no '| O<n> |' rows)")
        return errors

    for path in _living_documents(include_adrs=True):
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            for match in QUESTION_ID.finditer(line):
                if match.group(1) not in defined:
                    errors.append(
                        f"{path.relative_to(REPO).as_posix()}:{number} cites open question "
                        f"{match.group(1)}, which docs/roadmap.md does not define")
    return errors


def _living_documents(include_adrs: bool) -> list[Path]:
    """Every Markdown document under docs/, plus the top-level ones."""
    docs = [p for p in sorted(DOCS.rglob("*.md"))
            if include_adrs or not p.name.startswith("ADR-")]
    docs += [p for p in (REPO / "README.md", REPO / "CHANGELOG.md", REPO / "SECURITY.md",
                         REPO / "CLAUDE.md") if p.is_file()]
    return docs


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

    # Every ADR must be listed in the index. The index is how a reader finds a
    # decision, and ADR-0017 went unlisted for a release with nothing noticing.
    index = DECISIONS / "README.md"
    listed = set(re.findall(r"\((ADR-\d{4})-[^)]*\.md\)", index.read_text(encoding="utf-8"))) \
        if index.is_file() else set()
    for adr_id in sorted(existing_ids - listed):
        errors.append(f"docs/decisions/README.md does not list {adr_id}")

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


# A layout-tree line: box-drawing glyphs, then everything after them.
#
# **Everything**, not the first token. It captured only the first token until
# P18, which meant that on a line like
#
#     ├── tests/support/    fake_transport.*  manual_clock.hpp  message_builder.hpp
#
# only `tests/support/` was ever looked at -- and most of section 10's tree
# names several files per line. Two of P18's audit findings were files listed
# in second position that do not exist, sitting under a gate reporting a pass.
# Worse, they were not counted as skipped either, so the skip count that exists
# to expose a narrowed rule could not see this one.
LAYOUT_LINE = re.compile(r"^[│|\s]*(?:├──|└──|\|--|`--)\s*(.*)$")

# A continuation line: the glyph column, then prose that continues the entry
# above. These carry file names too -- the second and third lines of the
# tests/hil/ entry name half a dozen -- but they also carry ordinary prose, so
# a token here is only checked when it *looks* like a repository path and
# resolves. An unresolvable token on a continuation line is counted as skipped
# rather than reported, because "run_hil.py supervises" is a sentence, not a
# listing, and demanding otherwise would make the rule unusable.
LAYOUT_CONTINUATION = re.compile(r"^[│|\s]{2,}(\S.*)$")

# ...and the token has to look like a path. Without this, R5 reads the borders
# of the *other* fenced diagrams in architecture.md as entries: a line such as
# "└─────┬────┘" matches the glyph pattern above and yields a "path" made
# entirely of box-drawing characters. Those lines are not layout entries at all,
# so they are neither checked nor counted as skipped -- inflating the skip count
# with them would hide a real narrowing of the rule, which the count exists to
# expose.
PATHISH = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.+@/{}*-]*$")

# Extensions the layout tree actually names. A token carrying one of these is
# meant to be a file, so failing to resolve it is an error even in second
# position; a token that does not is prose until proven otherwise. Keeping this
# an explicit list rather than "anything with a dot" is deliberate -- prose in
# the tree contains "e.g." and version numbers.
FILE_SUFFIXES = (
    ".cpp", ".hpp", ".h", ".c", ".cc", ".py", ".sh", ".md", ".in",
    ".txt", ".json", ".cmake", ".yml", ".yaml", ".conf",
)


def _looks_like_a_file(token: str) -> bool:
    return token.endswith(FILE_SUFFIXES)


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

    The tree is prose, not data, so this rule is deliberately conservative. It
    reads **every** path-shaped token on an entry line, not just the first, and
    **skips anything it cannot read as a single path** -- a brace expansion
    (`upload_session.{hpp,cpp}`), a glob (`update_state_machine.*`), or an entry
    marked `(planned`.

    The first token is special in one way: an entry line begins with the thing
    the line is about, so an unresolvable token *there* is an error. Later
    tokens on the same line, and tokens on the continuation lines under it, are
    a mix of file names and prose; those are reported only when they look like
    a repository path (a suffix, or a name with an extension the tree actually
    uses) and are counted as skipped otherwise. That asymmetry is what lets the
    rule read `fake_transport.*  manual_clock.hpp  message_builder.hpp` without
    tripping over `run_hil.py supervises the case suite`.

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
    entry_open = False
    for line in text.splitlines():
        if line.startswith("```"):
            in_fence = not in_fence
            entry_open = False
            continue
        if not in_fence:
            continue
        match = LAYOUT_LINE.match(line)
        if match:
            rest, first_is_entry = match.group(1), True
            # `entry_open` gates the continuation rule below, and it is set only
            # for a line whose first token is path-shaped. architecture.md's
            # *other* fenced diagrams draw boxes with the same glyphs -- a line
            # like "└─────────────┘" matches LAYOUT_LINE and yields a "path" of
            # box-drawing characters -- and opening an entry on one of those
            # would make the prose inside that diagram look like a file listing.
            entry_open = bool(PATHISH.match(rest.split()[0])) if rest.split() else False
            if not entry_open:
                continue
        else:
            continuation = LAYOUT_CONTINUATION.match(line) if entry_open else None
            if not continuation:
                continue
            rest, first_is_entry = continuation.group(1), False
        for index, token in enumerate(rest.split()):
            is_entry_name = first_is_entry and index == 0
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
            # A leaf named without its directories -- `header.hpp` inside a
            # nested branch of the tree -- cannot be resolved without tracking
            # indentation, which is the parsing this rule refuses to do. So it
            # is accepted when something tracked ends with that path.
            #
            # Matched against `git ls-files` rather than a filesystem walk:
            # `build/` here holds dependency checkouts, peer firmware and every
            # past bench run, and a recursive glob through it takes minutes per
            # unresolved token. Tracked files are also the right set -- the
            # layout describes the repository, not whatever a build left behind.
            if any(p == bare or p.endswith("/" + bare) for p in _tracked_paths()):
                checked += 1
            elif is_entry_name or _looks_like_a_file(bare):
                errors.append(
                    f"architecture.md's layout names {token!r}, which does not exist"
                )
            else:
                # Prose. "supervises", "reproducible", "PR" and the like all
                # reach here; so would a real path spelled in a way the tree
                # does not use elsewhere, which is the cost of not demanding
                # that a documentation tree be machine-readable.
                skipped += 1
    print(f"R5: checked {checked} layout path(s), skipped {skipped} "
          f"entry(ies) it could not read as a single path")
    if verbose:
        print(f"    (a rising skip count means R5 is checking less than it looks)")
    return errors


# A development-phase ID: P0 ... P20, P14a. The library was built in numbered
# phases and the living documents used to be written in terms of them; they
# now describe the present, and the history is in git (ADR-0018).
PHASE_ID = re.compile(r"\bP\d{1,2}[a-z]?\b")


def rule_6_no_phase_ids() -> list[str]:
    errors: list[str] = []
    for path in _living_documents(include_adrs=False):
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            match = PHASE_ID.search(line)
            if match:
                errors.append(
                    f"{path.relative_to(REPO).as_posix()}:{number} names development phase "
                    f"{match.group(0)}; describe the present and leave the history to git")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", help="git ref to diff against for R1")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    all_errors: list[tuple[str, list[str]]] = [
        ("R1 docs accompany code", rule_1_docs_accompany_code(resolve_base(args.base), args.verbose)),
        ("R2 roadmap is a backlog", rule_2_roadmap_consistency()),
        ("R3 ADR integrity", rule_3_adrs()),
        ("R4 public symbols documented", rule_4_public_symbols_documented()),
        ("R5 layout tree exists", rule_5_layout_tree_exists(args.verbose)),
        ("R6 no development-phase IDs", rule_6_no_phase_ids()),
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
