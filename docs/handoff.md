# Agent session handoff

Implementation spans many independent sessions with **no shared conversational
memory**. The repository is the only authoritative project state. This file
defines the protocol and holds the log.

---

## Start of session

1. **Read [`roadmap.md`](roadmap.md)** — "Current state" names the next phase.
2. **Read the newest entry in the [session log](#session-log)** below.
3. **Read [`architecture.md`](architecture.md)**, and the sections of
   [`design.md`](design.md) and [`protocol-notes.md`](protocol-notes.md) that
   the phase references.
4. **Read the ADRs the phase depends on** ([`decisions/`](decisions/)). Do not
   re-decide anything an accepted ADR settles — if it must change, follow
   [§ Changing an architectural decision](#changing-an-architectural-decision).
5. **Check the code and CI**: build the project, run `ctest`, confirm the
   working tree matches what the log claims.
6. **Confirm the phase's prerequisites are met.** If they are not, the phase is
   `Blocked`: say so in the roadmap and pick up the blocking phase instead.
7. Refine the phase plan if the code has moved on since it was written — and
   write the refinement into the roadmap before starting.

## During the session

* **Stay inside the current phase.** Work found outside it goes in
  [`roadmap.md`](roadmap.md) § "Discovered follow-up work", not into this
  change.
* Write tests alongside the implementation, not after.
* Avoid unrelated refactoring. If something nearby is wrong, record it as
  follow-up work.
* Record any newly discovered protocol behaviour in
  [`protocol-notes.md`](protocol-notes.md) **as you find it** — that file exists
  so nobody rediscovers the same thing twice.
* Keep the change reviewable. A phase that is growing past ~1000 lines of diff
  is a sign it should be split; split it in the roadmap rather than pushing on.

## End of session — the checklist

Do all of these. A phase is not complete because the code exists.

1. `cmake --build` succeeds for every relevant target.
2. `ctest --output-on-failure` is green.
3. Formatting (`clang-format --dry-run --Werror`) and static analysis
   (clang-tidy, cppcheck) are clean.
4. Sanitizer build passes where applicable; coverage checked against the phase's
   gate.
5. [`roadmap.md`](roadmap.md) updated: phase status, completed work, remaining
   work, discovered follow-ups, deviations from the original plan, and the
   "Current state" table at the top.
6. [`architecture.md`](architecture.md) / [`design.md`](design.md) /
   [`api.md`](api.md) updated wherever the implementation changed structure,
   behaviour or signatures.
7. New or changed ADRs written for significant decisions.
8. Unresolved issues written down — in the roadmap's open questions or as
   follow-up work.
9. **Append a session-log entry below.**
10. Verify the Definition of Done
    ([`quality-gates.md`](quality-gates.md) §12).

A phase may only be marked `Complete` when its "Remaining work" is empty and its
exit criteria are met. Partial progress is `In Progress` with the remaining work
spelled out — that is a perfectly good outcome for a session.

## Changing an architectural decision

Never drift. If implementation shows an ADR is wrong:

1. Identify the conflict precisely — which ADR, which claim, what contradicts it.
2. Evaluate the consequences for the other components and phases.
3. Write a **new ADR** that supersedes the old one; set the old one's status to
   `Superseded by ADR-NNNN`. Do not edit the old decision's body.
4. Update [`architecture.md`](architecture.md) and [`design.md`](design.md).
5. Update the affected roadmap phases.
6. *Then* implement.

If the conflict is not clearly resolvable within the session, stop and record it
as an open question rather than guessing — a documented open question is far
cheaper than silent divergence.

## Session-log entry template

```markdown
### <date> — <phase ID>: <short title>

**Status after this session:** <phase> = <Planned|In Progress|Blocked|Complete>

**Completed.** …
**Changed.** … (files, structure, APIs)
**Remaining in this phase.** …
**Discovered / follow-up.** …
**Caveats.** … (anything surprising, fragile, or assumed)
**Docs updated.** …
**Recommended next.** <phase ID and first task>
```

---

## Session log

Newest last.

### 2026-09-04 — Planning session: architecture and roadmap

**Status after this session:** P0 = `Planned` (no phase started; this session
produced no code by design).

**Completed.** Verified the protocol against primary sources — Zephyr `main`
documentation and headers for SMP framing, OS and Image management groups, and
the `img_mgmt` server implementation, plus MCUboot `main` for the image format
and swap semantics. Findings recorded in
[`protocol-notes.md`](protocol-notes.md) with per-fact source attribution.
Produced the architecture, detailed design, public API proposal, testing
strategy, quality gates, security analysis, dependency inventory, the 19-phase
roadmap, and 13 ADRs.

**Changed.** Created the `docs/` tree and `README.md`. No source code, no build
system — those are P0.

**Remaining in this phase.** N/A (planning is done).

**Discovered / follow-up.** Two protocol facts that a naïve implementation gets
wrong and that shaped the design:
* The upload response's `off` is **authoritative**; on a mismatch the server
  answers with **success** and the offset it expects, and it may answer `0` at
  any time, which requires re-sending a complete first packet
  ([`protocol-notes.md`](protocol-notes.md) §6 rules 5 and 7).
* MCUmgr uses **two different SHA-256 values** — the upload `sha` (whole file)
  and image-state `hash` (`IMAGE_TLV_SHA256`, header + body). Conflating them is
  the classic client bug (§6, §7).
Also: the server validates the MCUboot magic and requires ≥ 32 bytes in the
first chunk, which puts a hard floor on chunk size (§6 rules 2–3).

**Caveats.**
* `docs.zephyrproject.org` was unreachable from this environment; the same
  documents were read from their reStructuredText sources in the Zephyr
  repository, which is the same content and arguably more authoritative. Sources
  are listed in [`protocol-notes.md`](protocol-notes.md) §1.
* Everything in [`api.md`](api.md) is a *proposal*. It is concrete enough to
  validate the architecture but has never been compiled. Expect it to change
  during P1–P12, and update it when it does.
* The QCBOR choice ([ADR-0007](decisions/ADR-0007-cbor-library.md)) is based on
  its documented API and licence, not on hands-on integration. P5 should confirm
  the spiffy-decode API is as ergonomic as assumed and supersede the ADR if not
  — TinyCBOR is the designated fallback.
* Six open questions are parked in [`roadmap.md`](roadmap.md); O1 (licence)
  should be settled in P0.

**Docs updated.** All of them — created.

**Recommended next.** **P0**, starting with open question O1 (licence), then the
top-level CMake and the `smply_internal_options` target. Prove each CI gate
fails on a deliberate violation before moving on; every later phase depends on
those gates actually working.
