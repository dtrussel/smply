# ADR-0018 — A maintenance process: a backlog, not a phase log

**Status:** Accepted (2026-09-24). Supersedes rule 4 of ADR-0013 and the
session-log half of its rule 1; ADR-0013's other rules stand.

## Context

ADR-0013 was written for building the library from nothing, one phase per
session, with no shared memory between sessions. It made two instruments
carry that project's state:

* **the roadmap as execution state** — each phase with a `Status` line and a
  "Remaining work" list that `check_docs.py` R2 checked, plus an "Outcome"
  written when the phase closed; and
* **the session log in `handoff.md`** — one entry per session, appended, never
  pruned.

Both did their job. Phases P0 to P20 are complete, and the library has been
built, run against hardware and packaged. The instruments have not adapted.
By the time the last phase closed, `roadmap.md` was about 3,300 lines and
`handoff.md` about 2,800. More than four fifths of each was history: outcome
narratives, struck-through follow-up rows, and session entries repeating what
the commits already say. A new session had to read past all of it to find the
twenty lines describing what is open. Phase identifiers had also spread into
roughly 150 code comments ("until P13's audit", "the P17 peer"), where they
explain the code's past rather than its present.

The failure ADR-0013 guarded against, one session re-deciding what another had
settled, is still possible. It is now more likely to come from a current
decision buried under history than from a missing one.

## Decision

**1. The roadmap is a backlog.** `docs/roadmap.md` holds four things:
* a short statement of current state;
* the work in progress, if there is any;
* the open questions (`O<n>`, whose IDs are stable because code and documents
  cite them);
* the open follow-up items, grouped by area.

When an item is done it is deleted, not struck through. What it was and why it
changed is in the commit that closed it.

**2. There is no session log.** `docs/handoff.md` becomes the contributor
guide: how to start, how to finish, how to change a decision, and the
**standing caveats**. The caveats state rules that are true of the code now,
with no record of how each was found. Every caveat must be current; when one
stops being true, delete it.

**3. History lives in git.** The per-phase outcomes and the session log are
deleted from the tree. The last commit that contains them is `97f1647`, and
`roadmap.md` says so. Commit messages carry the reasoning behind a change.

**4. Documents and code comments describe the present.** Living documents and
source comments do not cite development phases (`P<n>`). A comment explains
the invariant and the reason for it, and cites a stable anchor where there is
one: a protocol-notes finding (`A<n>`, `§n`), a threat (`T<n>`), an ADR or an
open question. ADRs are exempt, because their bodies are immutable and record
their own context. `check_docs.py` enforces this rule.

**5. ADR-0013's other rules stand, unchanged in substance:**
* documentation changes in the same commit as the code it describes, and R1
  enforces that;
* ADRs are superseded, never edited in place;
* protocol findings go in `protocol-notes.md`;
* the repository, not a conversation, is the project's state.

Rule 1, "every session reads before it writes", stays. What is read changes:
the roadmap's backlog and the contributor guide, not a session log.

## Alternatives considered

**Move the history to `docs/archive/`.** This keeps the history greppable in
the tree, but every document gate then needs an exemption for the archive, and
the archive still turns up in every search. `git show 97f1647:docs/roadmap.md`
gives the same text with neither cost.

**Condense the history in place.** A few lines per phase and the last few
session entries. This halves the size and keeps the problem: the documents
would still grow with every session and still mix what is true now with what
was once true.

**Keep the process, prune by hand occasionally.** This is what happened, and
the result was 6,000 lines. A process whose steady state is growth needs a rule
that deletes, not good intentions.

## Consequences

* **The unit of work is a backlog item, or a named piece of work** such as the
  quality review in `review-plan.md`. It is no longer a numbered phase. `R2`
  checks the backlog's structure instead of phase status lines: the required
  sections exist, and every `O<n>` a document cites is defined there.
* **Phase IDs in the ADRs stay** and read as history. `P17` in ADR-0015 means
  what it meant when that ADR was written, and `git log` can resolve it.
* **The gate that caught stale "(planned, P<n>)" markers is replaced**, because
  phases no longer exist. R6 now forbids phase IDs in living documents and
  source comments.
* **CLAUDE.md, `handoff.md`, `quality-gates.md` §11–§12 and the ADR index**
  are updated in the same change as this ADR.
