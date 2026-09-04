# ADR-0013 — Documentation is part of the product

**Status:** Accepted (2026-09-04)

## Context

Implementation will span many independent coding-agent sessions with no shared
conversational memory. The dominant failure mode for such a project is not bad
code — it is **architectural drift**: a later session, lacking context,
re-decides something that was already settled, and the design document quietly
becomes a description of a plan nobody followed. Once that happens the
documentation is worse than useless, because it is confidently wrong.

## Decision

Documentation is a **deliverable, not planning residue**, governed by four
rules.

**1. Every session reads before it writes.** The handoff protocol
([`../handoff.md`](../handoff.md)) requires reading `roadmap.md`,
`architecture.md`, the relevant design sections and the applicable ADRs before
changing anything.

**2. A change that makes documentation inaccurate is not complete.** Docs are
updated in the *same* change as the code, never in a follow-up. Enforced
mechanically: `tools/check_docs.py` fails a PR that touches
`include/smply/`, `src/smp/`, `src/dfu/` or `src/groups/` without touching
`docs/`, unless the PR body carries `Docs-Impact: none` with a justification
([`../quality-gates.md`](../quality-gates.md) §11).

**3. Decisions are superseded, never edited.** An accepted ADR is immutable
except for its `Status:` line. Changing a decision means writing a new ADR that
supersedes the old one and updating the old one's status — so the *reasoning
that was rejected* stays visible and a future session cannot accidentally
re-litigate it. The escalation path when implementation contradicts an ADR is:
identify the conflict → evaluate consequences → write the superseding ADR →
update `architecture.md`/`design.md` → update affected roadmap phases → *then*
implement.

**4. The roadmap is execution state, not an estimate.** Phase status, completed
work, remaining work, discovered follow-ups and deviations are updated as work
happens. A phase may not be marked `Complete` while its "Remaining work" is
non-empty (also checked by `check_docs.py`).

Protocol findings get their own home: anything discovered about device or
specification behaviour goes into
[`../protocol-notes.md`](../protocol-notes.md) so it is never rediscovered.

## Alternatives considered

**Documentation in the code only (Doxygen).** Excellent for API reference,
useless for architecture, rationale and cross-cutting protocol behaviour — none
of which belongs in a header comment. Kept as a complement (public symbols are
documented in their headers, gated), not a replacement.

**A wiki or external design docs.** Drifts immediately, because it is not in the
diff and no gate can see it. Rejected: docs must live next to the source and
travel in the same commit.

**Docs written at the end of each phase.** The familiar compromise, and it fails
the same way every time — the session that has the context is the one that ends,
and the next session inherits a gap. Rejected in favour of same-change updates.

**No enforcement, just convention.** Conventions do not survive across sessions
with no shared memory. The mechanical gate is the point.

## Consequences

* Every implementation PR is expected to touch `docs/`. That is intended, not
  friction to be optimised away.
* The `Docs-Impact: none` escape hatch exists for genuine no-op changes and is
  visible in review, so its overuse is detectable.
* ADR count grows; ADRs are short and superseded ones are kept. The index in
  [`README.md`](README.md) carries the current status of each.
* The repository — not any conversation — is the authoritative project state.
  A fresh session with only `git clone` must be able to continue, and that is
  itself a testable property: it is the first item of the Definition of Done
  review in [`../quality-gates.md`](../quality-gates.md) §12.
