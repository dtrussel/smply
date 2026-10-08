# Domain docs

How the engineering skills read and write smply's domain documentation. Where
a skill's own text names a different location or format, this file wins.

## Before exploring, read these

* [`architecture.md`](../architecture.md) for the components and §10 for the
  layout, and the sections of [`design.md`](../design.md) for the code you are
  touching.
* The ADRs in the area, from the index in
  [`decisions/README.md`](../decisions/README.md).
* [`protocol-notes.md`](../protocol-notes.md) for anything about SMP, MCUmgr or
  MCUboot behaviour. Its findings (`A<n>`, `S<n>`, `§n`) are cited by ID.
* `GLOSSARY.md` at the repository root, **if it exists**. It does not yet.

## Vocabulary

The project's terms are defined where they are used: protocol terms in
`protocol-notes.md`, API types in [`api.md`](../api.md) and the public headers,
threats (`T<n>`) in [`security.md`](../security.md). Use those names exactly,
in test names, ticket titles and hypotheses. The two image hashes are the
example of why: the upload `sha` (`Hash`) and the image-state `hash`
(`ImageHash`) are different things with different types.

`GLOSSARY.md` is single-context, at the repository root, and created lazily by
`/domain-modeling` (directly, or through `/grill-with-docs`) the first time a
term is resolved. An entry is a one- or two-sentence definition that **points
at** the document defining the term; it never restates a protocol fact, a
limit or a signature. If the glossary and the defining document disagree, the
defining document is right and the glossary entry is the bug.

## Decisions

ADRs are **not** in `docs/adr/` and do not use the short format in
`domain-modeling/ADR-FORMAT.md`. They live in `docs/decisions/`:

* file name `ADR-NNNN-<slug>.md`, numbered from the highest existing one;
* the format `Status · Context · Decision · Alternatives considered ·
  Consequences`, with `**Status:** Proposed` until a human accepts it;
* a row in `docs/decisions/README.md`;
* an accepted ADR is never edited to change it. A new ADR supersedes it, and
  the old one's status becomes `Superseded by ADR-NNNN`. Then
  `architecture.md`, `design.md` and the roadmap are updated, and only then is
  the code changed (`docs/handoff.md`, "Changing an architectural decision").

`domain-modeling`'s test for when a decision deserves an ADR (hard to reverse,
surprising without context, a real trade-off) applies unchanged.

## Flag ADR conflicts

If your output contradicts an accepted ADR, say so explicitly and name it:

> _Contradicts ADR-0010 (one request in flight), but worth reopening because…_

Never resolve the conflict silently in code.
