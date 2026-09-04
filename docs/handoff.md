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

### 2026-09-04 — P0: project scaffolding and quality infrastructure

**Status after this session:** P0 = `Complete`. Next phase is **P1 — Core types**.

**Completed.** All eight P0 tasks. The repository now builds a placeholder
library, runs three passing tests, and enforces every gate that has code to
enforce against. Open question O1 resolved: **Apache-2.0**.

**Changed.**
* `LICENSE` (Apache-2.0), `NOTICE`, `.editorconfig`, SPDX headers everywhere.
* `CMakeLists.txt`, `CMakePresets.json`, `cmake/{warnings,sanitizers,dependencies}.cmake`.
  Target-based throughout; strict flags live on `smply_internal_options`, linked
  `PRIVATE`, so consumers inherit nothing.
* `include/smply/version.hpp.in`, `src/version.cpp`, `tests/unit/`,
  `tests/consumer/` (the flag-leak guard).
* `.clang-format`, `.clang-tidy`, `tools/cppcheck-suppressions.txt`.
* `tools/`: `sources.sh`, `format.sh`, `lint.sh`, `coverage.sh`,
  `check_public_headers.py`, `check_deps.py`, `check_docs.py`, `verify_gates.sh`.
* `.github/workflows/ci.yml` — 10 jobs across 5 job groups.

**Remaining in this phase.** None.

**Discovered / follow-up.** Four items added to the roadmap's discovered-work
table (install/export vs. FetchContent for P18; R4's heuristic for P1; cppcheck
never actually executed yet; the MSVC-preset environment requirement). Three
technical findings are recorded in the roadmap's P0 outcome section — the most
important being that **`verify_gates.sh` caught my own flag-leak guard being
broken**: it checked `INTERFACE_COMPILE_OPTIONS` only, which is empty in exactly
the case that matters (linking the options target `PUBLIC` rather than
`PRIVATE`). A gate that cannot fail is worse than no gate, and that is why the
verification step is not optional.

**Caveats.**
* **Clang's sanitizer runtime is missing in the development container**
  (`libclang-rt-18-dev`), so `linux-clang-asan-ubsan` cannot be verified
  locally; a GCC sanitizer preset was added and *is* verified locally, and CI
  installs the Clang runtime explicitly.
* `cppcheck`, `lcov` and `gcovr` are absent locally. `tools/lint.sh` and
  `tools/coverage.sh` degrade with a message rather than failing, so a local
  run is weaker than CI. cppcheck 2.13 has now run in CI and reported nothing.
* The CMake floor is now **3.25** (was 3.24) for `FetchContent_Declare(SYSTEM)`.
* Coverage thresholds are deliberately not enforced yet; P13 turns them on.

**Docs updated.** `README.md` (status, build, checks, licence),
`docs/dependencies.md` (exact pins + hashes, licence, SYSTEM rationale),
`docs/quality-gates.md` (Clang 18, staged job set, coverage not yet enforced,
SYSTEM finding, §10/§11 reconciled with the implementation),
`docs/roadmap.md` (P0 Complete + outcome + 5 deviations, O1 resolved, 4
follow-ups), `CLAUDE.md` (SPDX convention, pre-finish checklist), this log.

**CI.** All 10 jobs green on the first push (run 33919323903), including both
MSVC jobs, which could not be verified locally. Two cosmetic follow-ups were
fixed afterwards: `tools/lint.sh` printed clang-tidy's "Optimized build." line
instead of its version, and `tools/cppcheck-suppressions.txt` used `//` rather
than cppcheck's documented `#` comment syntax (cppcheck tolerated it, so this
corrected a latent inaccuracy rather than a break).

**Recommended next.** **P1 — Core types.** Start with
`include/smply/detail/expected.hpp`, since everything else depends on
`Result<T>`; the `linux-gcc-fallback-expected` preset and CI job already exist
to test both backings. Then `error.hpp` (note `MgmtError` must carry the group
alongside `rc` — see `protocol-notes.md` §3), then `clock.hpp` and
`tests/support/manual_clock.hpp`. Watch for `check_docs.py` R4 firing on
templates and multi-line declarations; harden it rather than working around it.

### 2026-09-04 — P1: core types

**Status after this session:** P1 = `Complete`. Next phase is **P2 — SMP header
types and codec**.

**Completed.** The vocabulary every later phase builds on: `bytes.hpp`,
`group.hpp`, `error.hpp`, `detail/expected.hpp`, `result.hpp`, `clock.hpp`,
`limits.hpp`, `src/core.cpp`, `tests/support/manual_clock.hpp`, and 35 tests.
All gates green, clang-tidy clean, 13/13 gate self-checks passing.

**Changed.** Four deviations, all recorded in the roadmap's P1 outcome:
`Group` promoted to its own core header (the error model needs it, so it cannot
wait for P2); a C++23 CI job added so the `std::expected` backing is actually
exercised; `performance-enum-size` disabled with a written reason; and
`src/core.cpp` holding three small out-of-line definitions rather than one file
per header.

**Remaining in this phase.** None.

**Discovered / follow-up.** Three new items in the roadmap's follow-up table
(no monadic operations on `Result`; `to_string(const Error&)` allocates; Clang
sanitizers are CI-only here). The R4 follow-up from P0 is resolved.

**Caveats — read these before P2.**

* **`Result<T>` is a *subset* of `std::expected`.** There is no `value()` and
  there are no monadic operations. Under C++20 you get smply's own type, so
  anything outside the subset compiles nowhere useful; under the C++23 job it
  would compile and then break every other build. Use `has_value()`,
  `operator*`, `operator->`, `error()`, `value_or()`, and `fail()` to build
  failures.
* **`expected` is backed by `std::variant`, not a union.** The first version
  used a union and had a real exception-safety hole, which clang-tidy's analyser
  found. Both alternatives are `static_assert`ed nothrow-move-constructible so
  `valueless_by_exception` is unreachable. If you add a type to a `Result` that
  is not nothrow-move-constructible you will get a clear compile error — fix the
  type, do not remove the assertion.
* **`MgmtError` is not just an int.** `rc` is meaningless without `group` and
  `group_scoped`; an image-group rc 30 and an SMP v1 rc 30 are different errors.
  Build them with `MgmtError::smp()` / `MgmtError::scoped()` rather than
  aggregate-initialising, so the flag cannot disagree with the shape.
* **`Group` is open.** Any 16-bit value is legal; never reject one. It now lives
  in `smply/group.hpp`, so P2's `smp/header.hpp` should include that rather than
  redefining it as `api.md` originally showed.
* `tools/verify_gates.sh` earns its keep — it caught two R4 bugs this session
  that review had missed. Run it whenever you touch `tools/` or `cmake/`.
* **Its own fixtures can rot.** Completing P1 turned the R2 case's text
  substitution into a no-op, so that gate silently went unverified while still
  printing PASS; CI caught it. It now appends a synthetic `P99` phase. When you
  add a case, do not key the violation to real content that later work will
  change.

**Docs updated.** `api.md` (new `group.hpp` and `limits.hpp` sections; `error.hpp`
and `result.hpp` reconciled with the real signatures; `Group` removed from the
`smp/header.hpp` sketch), `architecture.md` (layout, core-types row, limits
pointer), `quality-gates.md` (the C++23 job, the `performance-enum-size`
rationale, the YAML folded-scalar trap, the two site-local suppressions),
`roadmap.md` (P1 Complete with outcome, 4 deviations, 3 discovered items),
this log.

**Recommended next.** **P2 — SMP header types and codec.** `include/smply/group.hpp`
already exists; `smp/header.hpp` adds `Operation`, `Version`, `Header` and
`kHeaderSize` and includes it. Start from the bit layout and the golden vectors
in `protocol-notes.md` §2 — byte 0 is `res(3) | version(2) | op(3)` MSB-first,
and every multi-byte field is big-endian. Decode must reject non-zero `res` and
version `0b10`/`0b11`, but preserve unknown `flags` rather than rejecting them.
Do not validate `length` there: bounds belong to the assembler in P3, which is
the only component that knows the configured limit.

### 2026-09-04 — P2: SMP header types and codec

**Status after this session:** P2 = `Complete`. Next phase is **P3 — Streaming
SMP message reassembly**.

**Completed.** `smp/header.hpp` (`Operation`, `Version`, `Header`,
`kHeaderSize`, `total_size()`), `src/smp/codec.cpp`, and
`tests/support/message_builder.hpp`. 53 tests total, all gates green.

**Changed.** Three deviations, all in the roadmap's P2 outcome: `Group` had
already landed in P1; `Header::total_size()` and a dynamic-span `decode_header`
overload were added because P3 needs both; response-operation mapping was
deliberately left to P6, where correlation lives.

**Remaining in this phase.** None.

**Discovered / follow-up.** `clang-analyzer-optin.core.EnumCastOutOfRange` is
now disabled project-wide — see the P2 outcome for why a wire-format enumeration
makes it unusable. One new follow-up: P6 should add the request-to-response
operation mapping to `smp/header.hpp`.

**Caveats — read these before P3.**

* **`decode_header` does not validate `length`, on purpose.** It reports what
  the wire says. A device claiming 60 000 bytes decodes fine; rejecting that is
  P3's job, because the assembler is the only component that knows the
  configured limit. `tests/support/message_builder.hpp` has
  `make_raw_message()` specifically for constructing that case.
* **`Header::total_size()` is the message-boundary rule.** Use it in the
  assembler rather than writing `8 + length` again.
* **Unknown flags and unknown groups are preserved, never rejected.** Both have
  tests asserting it. Do not "tidy" either into a validation error.
* `make_message()` fixes the length field to match the payload;
  `make_raw_message()` leaves it as stated. Reach for the second whenever you
  are testing hostile input.

**Docs updated.** `api.md` (`smp/header.hpp` reconciled with the real
signatures, including the two additions and the deliberate non-validation of
`length`), `architecture.md` (layout line), `quality-gates.md` (the
`EnumCastOutOfRange` rationale; the per-site suppression note narrowed to the
one that remains), `roadmap.md` (P2 Complete with outcome, 3 deviations, 1
discovered item), this log.

**Recommended next.** **P3 — Streaming SMP message reassembly.**
`src/smp/assembler.{hpp,cpp}` per `design.md` §2. The algorithm is short; the
value is in the tests. The invariant to build first is that byte-at-a-time,
whole-message, fixed-size-fragment and randomised-cut delivery all produce
identical output — `FakeTransport` does not exist yet (P4), so drive
`MessageAssembler::feed()` directly. Enforce both bounds
(`limits::kMaxSmpPayload`, `limits::kMaxAssemblyBuffer`) and assert the buffer
never exceeds them under adversarial input, using `make_raw_message()`.

### 2026-09-04 — P3: streaming SMP message reassembly

**Status after this session:** P3 = `Complete`. Next phase is **P4 — Transport
abstraction and `FakeTransport`**.

**Completed.** `src/smp/assembler.{hpp,cpp}` and 19 test cases (72 total). All
gates green, including ASan/UBSan and 13/13 gate self-checks.

**Changed.** Four deviations, all in the roadmap's P3 outcome. The substantive
one: the assembler buffers **at most one message** and parses directly out of
the caller's bytes when nothing is held, rather than using the planned growing
buffer with a read cursor. `design.md` §2 has been rewritten to describe what
exists and why it is better bounded.

**Remaining in this phase.** None.

**Discovered / follow-up.** `src/` had to be added as a `PRIVATE` include
directory on the `smply` target so its own sources can include internal
headers. One new follow-up for P6, about the re-entrancy guard.

**Caveats — read these before P4.**

* **Payload spans are borrowed for the duration of `on_message()` only**, and on
  the fast path they point into the *caller's* buffer, not the assembler's.
  `SmpClient` must copy anything it retains past the callback.
* **A sink must not call `feed()` re-entrantly.** The assembler returns
  `InvalidState` rather than corrupting its buffer, but the real answer is not
  to do it. Relevant when P6 dispatches user callbacks from `on_message()`.
* **Malformed framing is terminal for the stream, by design.** SMP has no sync
  word, so there is nothing to resynchronise on. The assembler resets and
  returns the error; the caller is expected to drop the connection. Do not add
  a "skip a byte and retry" recovery — it would resynchronise onto attacker-
  chosen boundaries.
* **`reset()` must be called on connect and disconnect.**
  `SmpClient::rebind_transport()` owes this, so a truncated message cannot bleed
  across a reconnect.
* The assembler holds no opinions the codec does not: unknown groups and flags
  pass straight through, with a test asserting it.

**Docs updated.** `design.md` (§2 rewritten: the two paths, the ordering of the
bound check, why one-message buffering beats a cursor, the failure and
lifecycle rules), `roadmap.md` (P3 Complete with outcome, 4 deviations, 2
discovered items), this log. `api.md` unchanged — the assembler is internal.

**Recommended next.** **P4 — Transport abstraction and `FakeTransport`.**
`include/smply/transport.hpp` is the last public contract to freeze before the
client exists, so get it right: the normative wording is `design.md` §9 and the
rationale is ADR-0005. `FakeTransport` should be able to express every scenario
listed in `testing.md` §2 — write one test per scenario even where trivial,
because P6 through P12 all depend on those being expressible. Note that
`MessageSink` (P3) and `TransportListener` (P4) are different interfaces at
different layers; `SmpClient` will implement both.
