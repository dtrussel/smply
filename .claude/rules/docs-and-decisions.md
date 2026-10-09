---
paths:
  - "docs/**"
  - "*.md"
---

# Documentation and decisions

The rules `tools/check_docs.py` enforces are listed in its docstring and in
`docs/quality-gates.md` §11. Run it after any documentation change.

* ADRs live in `docs/decisions/ADR-<NNNN>-<slug>.md`, use the
  Status · Context · Decision · Alternatives considered · Consequences format,
  and are listed in `docs/decisions/README.md`. An accepted ADR is superseded by
  a new one, never edited; the README says which status notes are allowed.
* `docs/roadmap.md` is a backlog: done items are deleted, open-question IDs
  (`O<n>`) are stable, and every backlog row has a "When".
* Living documents describe the present: no development-phase IDs (`P<n>`).
* Protocol findings go in `docs/protocol-notes.md` with their primary source.
