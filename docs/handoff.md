# Agent session handoff

Implementation spans many independent sessions with **no shared conversational
memory**. The repository is the only authoritative project state. This file
defines the protocol and holds the log.

---

## Start of session

1. **Read [`roadmap.md`](roadmap.md)** — "Current state" names the next phase.
2. **Read [§ Standing caveats](#standing-caveats)** below. It is the distilled
   version of every session's hard-won detail; the log is the history behind it.
3. **Read the newest entry in the [session log](#session-log)** below.
4. **Read [`architecture.md`](architecture.md)**, and the sections of
   [`design.md`](design.md) and [`protocol-notes.md`](protocol-notes.md) that
   the phase references.
5. **Read the ADRs the phase depends on** ([`decisions/`](decisions/)). Do not
   re-decide anything an accepted ADR settles — if it must change, follow
   [§ Changing an architectural decision](#changing-an-architectural-decision).
6. **Check the code and CI**: build the project, run `ctest`, confirm the
   working tree matches what the log claims.
7. **Confirm the phase's prerequisites are met.** If they are not, the phase is
   `Blocked`: say so in the roadmap and pick up the blocking phase instead.
8. Refine the phase plan if the code has moved on since it was written — and
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

## Standing caveats

Everything below is true of the code as it stands and has already cost a
session once. The log records *how* each was found; this list is what you need
before writing anything. Add to it when a discovery outlives its phase — and
delete an entry when it stops being true.

**Trusting device data**

* Bound every length before using it; never size an allocation on a number the
  device supplied.
* `cbor::Reader` distinguishes two things that look alike: a getter returning
  `std::nullopt` means the field was **absent**, which is normal — MCUmgr omits
  rather than sending zero or false. A wrong *type* poisons the reader and makes
  every field look absent. **Check `status()` before trusting any decoded
  struct**, or a malformed response reads as a successful one full of defaults.
* Decoded views (`std::string_view`, `ConstBytes`) point into the assembler's
  buffer and are valid only for the callback. Copy anything that outlives it.

**Layering**

* Groups are thin. They allocate no sequence numbers, set no deadlines and
  interpret no `rc` — `SmpClient` has done all three before a response arrives.
  `src/groups/os/os_management.cpp` is the shape to copy.
* A callback never runs inside the call that started the operation. When a group
  must reject an argument, queue the failure with `SmpClient::defer()` rather
  than invoking the callback inline.
* `SmpClientConfig::max_in_flight` is **1** by default; a second concurrent
  request fails with `InvalidState` rather than queueing.
* A response whose `seq` matches but whose group, command or op does not is
  discarded and the request left pending (ADR-0010). Do not "fix" this into a
  failure.

**Lifetime**

* A transport, and anything a callback captures, must **outlive** the
  `SmpClient`. Its destructor detaches from the transport *and* completes
  outstanding requests, so both are touched during destruction. Declare them
  before the client. Two separate bugs came from this, each caught by only one
  compiler.

**Protocol sources**

* Where Zephyr's documentation and Zephyr's source disagree, **the source wins,
  and you must read both.** Reset's `force` is the case in point (PN §9, A15):
  the docs say integer, the server decodes a boolean and silently ignores
  anything else. Reading only the `.rst` yields a flag that never works.
* MCUmgr uses **two different SHA-256 values** — the upload `sha` over the whole
  file, and image-state `hash` over `IMAGE_TLV_SHA256`. Conflating them is the
  classic client bug and first becomes reachable in P8/P9.

**Tooling traps**

* **A failed build leaves the previous test binary in place**, so `ctest` then
  reports the *old* suite passing. Check the build's exit status separately;
  never read "N tests passed" as evidence anything was rebuilt.
* **GCC's and Clang's sanitizers do not diagnose identically.** GCC's ASan does
  not report a dangling callback capture even with
  `-fsanitize-address-use-after-scope`. Clang's compiler-rt is not installable
  here, so that bug class is **CI-only**. A green local sanitizer run is
  necessary, not sufficient.
* **cppcheck IS installable** in the container (`apt-get install -y cppcheck`),
  despite an earlier note to the contrary — install it rather than discovering
  its findings in CI. It cannot parse some Catch2 files; see
  `tools/cppcheck-suppressions.txt` for what is suppressed and why.
* clang-tidy over the full tree now takes minutes. Run it in the background and
  collect the result, rather than blocking on it.
* Coverage means exactly what `tools/coverage.sh` reports (gcovr with
  `--exclude-throw-branches`). Quoting a branch percentage from a differently
  configured run is not comparable — the same objects move ~12 points.

---

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

### 2026-09-05 — P4: transport abstraction and `FakeTransport`

**Status after this session:** P4 = `Complete`. Next phase is **P5 — CBOR façade
and MCUmgr error extraction**.

**Completed.** `include/smply/transport.hpp` — the last public contract to
freeze before the client exists — plus `FakeTransport` and 27 test cases (99
total). All gates green, ASan/UBSan clean, 13/13 gate self-checks.

**Changed.** Four deviations, in the roadmap's P4 outcome. The one that matters:
`FakeTransport` enforces the contract rather than merely implementing it, and
counts suppressed callbacks.

**Remaining in this phase.** None.

**Discovered / follow-up.** One new follow-up (no `connected()` on `Transport`).
One clang-tidy finding fixed.

**Caveats — read these before P5, and especially before P6.**

* **The contract is now frozen.** `include/smply/transport.hpp` carries it as
  documentation, and `design.md` §9 is the same thing in a table. Changing it
  requires superseding
  [ADR-0005](decisions/ADR-0005-transport-abstraction.md) — do not quietly
  widen it because something is inconvenient in P6.
* **`MessageSink` (P3) and `TransportListener` (P4) are different interfaces at
  different layers.** `SmpClient` implements `TransportListener` publicly-ish
  (privately, per `api.md`) and `MessageSink` internally, and forwards
  `on_bytes()` into `MessageAssembler::feed()`. The composition test at the end
  of `test_fake_transport.cpp` shows the wiring.
* **Nothing may be delivered after `on_disconnected()`.** `FakeTransport`
  enforces this. If a P6 test sees `suppressed_deliveries() > 0`, the test is
  wrong, not the double.
* **`max_message_size() == 0` means "no opinion"**, not "zero bytes". The core
  falls back to its configured default. Do not treat 0 as a limit.
* **`TransportBusy` is a retry request, not a link failure.** The core does not
  queue; whoever knows what the message was for decides what to do.
* `send()` recording in `FakeTransport` copies, because the contract says the
  buffer is borrowed for the call only. Two tests deliver and send from
  temporaries specifically so ASan catches any retention.

**Docs updated.** `testing.md` (§2 `FakeTransport` reconciled with the shipped
double, including the enforcement behaviour and the scenarios needing no API),
`api.md` (transport section: `[[nodiscard]]` on `send`, `Disconnected`, and what
`close()` does not do), `roadmap.md` (P4 Complete with outcome, 4 deviations, 1
discovered item), this log. `design.md` §9 needed no change — the implementation
matched it.

**Recommended next.** **P5 — CBOR façade and MCUmgr error extraction.** This is
the first phase touching QCBOR, so verify early that its spiffy-decode API is as
ergonomic as [ADR-0007](decisions/ADR-0007-cbor-library.md) assumed — the ADR
names TinyCBOR as the fallback and says to supersede it if not. The subtle part
is not encoding but `extract_mgmt_error()`: it must handle all four shapes from
`protocol-notes.md` §3 (v1 flat `rc`, v2 `err:{group,rc}`, v2-with-flat-`rc`,
and success with neither), because an SMP v2 client is required to understand v1
errors too. Absent key must mean `nullopt`, not an error — MCUmgr omits fields
rather than sending false.

### 2026-09-05 — P5: CBOR façade and MCUmgr error extraction

**Status after this session:** P5 = `Complete`. Next phase is **P6 —
`SmpClient`: correlation, timeouts, cancellation**.

**Completed.** `src/cbor/` — the façade, the two backend translation units and
`mgmt_error` — plus 50 tests (149 total). All gates green, ASan/UBSan clean.

**ADR-0007 held.** QCBOR was chosen on its documented API rather than hands-on
use, and the ADR said to fall back to TinyCBOR if the assumption failed. It did
not: the spiffy-decode map-key getters and, critically, a distinguishable
`QCBOR_ERR_LABEL_NOT_FOUND` are exactly what "absent is not an error" requires.
The ADR's Status line records the validation; no supersession needed.

**Changed.** Four deviations, in the roadmap's P5 outcome. Notable: no separate
`backend_qcbor.*` file (reader/writer *are* the backend), and named `put_uint` /
`put_int` / … rather than an overloaded `value()`.

**Remaining in this phase.** None.

**Discovered / follow-up.** Two new follow-ups (the `Writer` cannot emit nested
maps or arrays; `for_each_map_in_array` handles maps only) — neither is needed
by anything in scope. Plus the nesting-bound finding below.

**Caveats — read these before P6, P7 and P8.**

* **`std::nullopt` means absent; `status()` means broken.** This distinction is
  the whole point of the façade. A getter returning `nullopt` is the *normal*
  case — MCUmgr omits fields rather than sending false or zero — whereas a
  wrong-typed field poisons the reader. **Always check `status()` before
  trusting a decoded struct**, or a malformed response will look like a
  successful one full of defaults. `extract_mgmt_error()` does this correctly;
  copy the pattern.
* **Views point into the response buffer**, which the assembler owns only for
  the duration of `on_message()`. `ImageState` and friends must copy every
  string and hash they keep. This is the same borrowed-buffer rule as P3 and P4,
  one layer up.
* **`extract_mgmt_error()` runs on every response, before group parsing.** It
  already handles all four shapes, so no group needs to think about `rc` versus
  `err` again — and none should re-implement it.
* **Nesting is bounded twice.** QCBOR's compile-time `QCBOR_MAX_ARRAY_NESTING`
  is 15, below `limits::kMaxCborNesting` (16), so QCBOR's bound fires first on
  hostile input. Do not read the configured limit as the operative one.
* **`bugprone-unchecked-optional-access` is disabled for `tests/` only**, via
  `tests/.clang-tidy`, because Catch2's `REQUIRE` is opaque to it. It stays on
  for `src/` and `include/`, where it earned its keep by finding a real
  readability problem in `reader.cpp`. Do not extend the exemption upward.
* Encoding buffers are caller-owned. P7 and P8 should size a request buffer on
  the stack; nothing in the façade allocates.

**Docs updated.** `design.md` (§3 rewritten around the shipped API, the
absent-versus-error rule, the double nesting bound, and what `mgmt_error`
guarantees), `dependencies.md` (QCBOR pin exercised, ADR assumption confirmed),
`decisions/ADR-0007` (Status line records validation), `roadmap.md` (P5 Complete
with outcome, 4 deviations, 2 discovered items), this log.

**Recommended next.** **P6 — `SmpClient`.** The pieces it composes all exist:
`MessageAssembler` (P3), `Transport`/`TransportListener` (P4) and the CBOR
façade (P5). Read [ADR-0010](decisions/ADR-0010-request-correlation.md) first —
correlation is on the full `(seq, group, command, op)` tuple, not `seq` alone,
and a mismatch **discards the message and leaves the request pending** rather
than failing it, because a stale message must never be able to complete a live
request. The retired-sequence set is what stops a late response being
mis-attributed after the 8-bit `seq` wraps. Add `response_to(Operation)` to
`smp/header.hpp` while you are there (a P2 follow-up), and note the P3 caveat:
a user callback dispatched from `on_message()` must not re-enter the assembler.

### 2026-09-05 — P6: `SmpClient`

**Status after this session:** P6 = `Complete`. Next phase: **P7 — OS
management group**.

**Completed.** `include/smply/smp_client.hpp` and `src/smp/client.cpp`: sequence
allocation, the pending-request table, correlation on the full
`(seq, group, command, op)` tuple, the retired-sequence ring, deadlines,
cancellation, disconnect and rebind, and counters. Plus 55 tests (204 total).
All gates green across `linux-gcc`, `linux-clang`, `linux-gcc-cxx23-std-expected`,
`linux-gcc-fallback-expected` and `linux-gcc-asan-ubsan`. Branch coverage of
`src/smp/` is 96.2 % against the phase's ≥ 90 % criterion.

**Changed.** Four deviations, detailed in the roadmap's P6 outcome. The one that
matters to every later layer: **callbacks are deferred, never immediate**.
`request()` and `cancel()` queue the callback for the next `poll()` so that
`handle = client.request(...)` has always assigned before anything can observe
the handle. `design.md` §4 was rewritten to match — it previously specified
immediate completion, and also still carried a rejected alternative mid-sentence.

**Remaining in this phase.** None.

**Discovered / follow-up.** Three items in the roadmap table. The important one
is below.

**Caveats — read these before P7 and P8.**

* **A transport must outlive every client bound to it, and so must anything a
  callback captures.** `~SmpClient` detaches from its transport *and* completes
  outstanding requests, so both are touched during destruction. Two separate
  bugs came out of this, each caught by exactly one compiler: the transport half
  by GCC (`pure virtual method called`), the capture half by Clang's ASan
  (stack-use-after-scope in 13 tests). **Declare transports and captures before
  the client.** Stated on `SmpClient`, its destructor and `rebind_transport()`,
  and at the test fixture's definition.
* **The two sanitizer jobs are not interchangeable.** GCC's ASan does not report
  the dangling-capture case even with `-fsanitize-address-use-after-scope` and
  `detect_stack_use_after_scope=1` — verified by running the failing test under
  both. `linux-clang-asan-ubsan` is CI-only here (no compiler-rt in the
  container), so this bug class cannot be caught locally at all. Treat a green
  local sanitizer run as necessary, not sufficient.
* **Groups get correlation and timing for free and must not reimplement
  either.** A group issues a `RequestSpec` and receives a `Result<RawResponse>`.
  It never sees a sequence number, never sets a deadline, and never decodes an
  MCUmgr error — `SmpClient::interpret()` has already turned one into an
  `ErrorCode::ProtocolError` carrying the `MgmtError` and any `rsn` text.
* **A response whose `seq` matches but whose group/command/op does not leaves
  the request pending.** It is counted as `mismatched` and discarded; the
  request times out normally. Do not "fix" this into a failure — completing a
  live request from a stale or hostile message is exactly what ADR-0010 forbids.
* **`RawResponse::payload` is borrowed for the callback only.** Same rule as
  P3–P5, one layer up: copy anything that must outlive the call.
* **`max_in_flight` is 1 by default.** A second concurrent request fails with
  `InvalidState` rather than queueing. P7 and P8 should issue one request at a
  time; the DFU machine in P12 sequences them.
* **`SmpClientStats` counters exist to be asserted on.** When a test expects a
  message to be dropped, assert *which* counter moved — `unmatched`, `late`,
  `mismatched` or `malformed`. That is the difference between a deliberate
  discard and a bug that happens to look quiet.

**Chasing coverage found a real defect.** The ≥ 90 % criterion was initially
missed at 88.1 %. Rather than waive it, the uncovered branches were located:
`request()` bounded the payload twice, and the second test could never fire
because `max_smp_payload` is a `uint16_t`. Removed, and replaced with a
`static_assert` that fires if the field is ever widened. Worth remembering the
next time a threshold is one point short.

**Caveat on the coverage number itself.** For the same objects, gcovr reports
76.6 % branch coverage counting throw branches and 96.2 % with
`--exclude-throw-branches`. `quality-gates.md` §6 does not say which it means.
Pin that down before P13 turns enforcement on. Related: `src/cbor/` sits at
80.9 %, below the ≥ 90 % elevated gate it is nominally held to.

**Docs updated.** `design.md` (§4 rewritten around the shipped API: deferred
callbacks, the receive path, lifetime), `api.md` (`smp_client.hpp` section
regenerated from the header), `roadmap.md` (P6 Complete with outcome, 4
deviations, 4 discovered items; three earlier follow-ups closed), this log.

**Recommended next.** **P7 — OS management group.** It is the first consumer of
everything below it and should read as thin: encode a request with
`cbor::Writer`, hand it to `SmpClient::request()`, decode the response with
`cbor::Reader` — checking `status()` before trusting the result, per the P5
caveat. Nothing in P7 should touch sequence numbers, deadlines or `rc` handling.
If it seems to need to, the seam is in the wrong place.

### 2026-09-05 — P7: OS management group

**Status after this session:** P7 = `Complete`. Next phase: **P8 — Image
management: state read/write, erase, slot info**.

**Completed.** `include/smply/groups/os.hpp` and
`src/groups/os/os_management.cpp`: reset, MCUmgr parameters, echo. Plus 35 tests
(239 total). All gates green across the five Linux presets. Every request vector
in the suite is hand-derived from the CBOR grammar and the field names in
[`protocol-notes.md`](protocol-notes.md) §5 — an encoder checked against its own
output proves nothing.

**Protocol work.** Verified before any code was written, adding source S13 (the
OS-group server implementation) to the inventory:

* **Echo had no recorded wire shape.** `{"d": str}` in, `{"r": str}` back. It is
  registered under *both* the read and write handler slots, which is why either
  op is legal; reset is write-only and mcumgr params read-only, so the wrong op
  on those returns `ENOTSUP` rather than an answer.
* **A15 — the documentation and the implementation disagree about `force`.**

**Changed.** Four deviations, in the roadmap's P7 outcome. The one that affects
every later group: **`SmpClient` gained a public `defer()`**. P6 had the
machinery but exposed no way for a layer above to use it, so a group rejecting
an argument would have had to invoke its callback inline — breaking the
invariant that a callback never runs inside the call that started the operation.

**Remaining in this phase.** None.

**Caveats — read these before P8, P10 and P12.**

* **A group is thin, and P8's should look like this one.** No sequence numbers,
  no deadlines, no `rc` handling: `SmpClient` has already done all three by the
  time a response arrives. If a group seems to need any of them, the seam is in
  the wrong place.
* **The four rules every group follows** are written out in
  [`design.md`](design.md) §5: encode into a stack buffer sized from the
  constant that bounds the input; check `cbor::Reader::status()` before trusting
  *any* decoded field; copy views before they escape the callback; never run a
  callback inside the call that started the operation — use `SmpClient::defer()`
  for an argument rejection.
* **Where Zephyr's docs and Zephyr's source disagree, the source wins — and you
  must read both.** A15 is the case in point: the specification says `force` is
  an integer, the server decodes a boolean, and it *discards the decode result*,
  so an integer is silently ignored and the reset proceeds unforced with nothing
  reported back. Reading only the `.rst` would have produced a client whose
  force flag never worked. P8 covers a much larger surface (§6); read
  `img_mgmt.c` alongside `smp_group_1.rst`, not instead of it.
* **Reset is acceptance, not completion.** The callback fires when the device
  took the command. Learning that it actually restarted means waiting for a
  transport disconnect, which is P12's job.
* **`smp_error()` is how a caller recognises an SMP-level code**, and it returns
  `nullopt` for a group-scoped `rc` so the two numbering spaces cannot be
  confused. P8 will want the *image* group's own `rc` enumeration alongside it —
  `img_mgmt_err_code_t` (S6) — and the same discipline applies: a group-scoped 3
  is not `MGMT_ERR_EINVAL`.
* **A failed build leaves the previous test binary in place**, so `ctest` cheerfully
  reports the *old* suite passing. This nearly hid a GCC-only compile error
  during P7 (`constexpr` on a function calling a non-`constexpr` accessor, which
  Clang accepts). Check the build's exit status separately from the test result;
  do not read "239 tests passed" as evidence that anything was rebuilt.

**On coverage.** `src/groups/os/` is at 71.6 % branch and 85.2 % line coverage,
and every uncovered line is one of eight deliberate invariant guards: three
encode-failure checks a `static_assert` proves unreachable, two `enter_map()`
checks `SmpClient::interpret()` already guarantees, and the `reject<>()`
instantiations only those can reach. They are kept rather than deleted — a
decoder that assumes its input was validated elsewhere is one refactor away from
trusting a device — but they inflate the branch denominator exactly as throw
branches do. That is now **two** categories a bare percentage cannot distinguish
from untested code, which P13 has to resolve before it turns enforcement on.

**Docs updated.** `protocol-notes.md` (S13, echo's wire shape, A15),
`design.md` (§5 rewritten: the four rules, and why reset is acceptance rather
than completion), `api.md` (`groups/os.hpp`, `SmpError`/`smp_error()`,
`Callback<T>`, `SmpClient::defer()`), `roadmap.md` (P7 Complete with outcome,
4 deviations, 2 discovered items, 2 follow-ups), this log.

**Recommended next.** **P8 — image management** (everything in group 1 except
upload). It is the first group with real decoding: `protocol-notes.md` §6 has
the "absent means false" and "absent image ⇒ 0" rules, and the element and
string caps in `limits.hpp` exist for it. Read §6 and §7 together first — the
two-hash distinction (upload `sha` over the whole file versus image-state `hash`
over `IMAGE_TLV_SHA256`) is the classic client bug, and P8 is where the wrong
one first becomes reachable.

### 2026-09-05 — Documentation pass: making the set ready for a cold start

**Status after this session:** no phase changed. P7 remains `Complete`; **P8** is
next. This was a review of the documentation set as a *new* session encounters
it, not phase work.

**Completed.** Read the docs in the order the protocol prescribes and fixed what
would have misled or blocked someone starting cold.

**Changed.**

* **`handoff.md` gained [§ Standing caveats](#standing-caveats)**, and the
  start-of-session list now points at it as step 2. Previously the detail that
  actually prevents mistakes was spread across eight session entries, and the
  protocol only told a new agent to read the newest one. The log stays as the
  history; the new section is what you need before writing code.
* **`architecture.md` §10 was wrong.** It listed `src/cbor/backend_qcbor.*`
  (which P5 explicitly decided not to create), `cmake/smplyConfig.cmake.in` and
  an `smply.hpp` umbrella that do not exist, while omitting `detail/expected.hpp`,
  `cbor.hpp`, `dependencies.cmake` and most of `tools/`. Everything not yet
  written is now marked **(planned)** with the phase that creates it.
* **`api.md` half describes shipped API and half a proposal**, with no way to
  tell which. A status table at the top now says, per header, which it is —
  shipped sections must match the code, proposed ones are sketches for the phase
  that implements them.
* **`quality-gates.md` §6 pins the coverage metric** to what `tools/coverage.sh`
  reports, and records the measured position per gate.
* **`CLAUDE.md`** points at the standing caveats and names the two traps that
  cost CI cycles this session.

**Two real defects found while checking the docs against reality.**

1. **`tools/coverage.sh` has produced no report at all since P0.** It passed
   `--txt "$BUILD_DIR"`, so gcovr treated the build directory as that option's
   output file and failed; the script exits 0 by design, so the CI coverage job
   stayed green while reporting nothing. Fixed. A gate that cannot fail is not a
   gate — and note that `verify_gates.sh` proves the *checkers* reject
   violations but does not cover this reporter.
2. **The coverage figures quoted in the P6 and P7 outcomes were measured with a
   flag the project's own script did not pass.** They happen to be right *now*
   only because §6 and the script were both pinned to
   `--exclude-throw-branches` in this pass. Before that, `tools/coverage.sh`
   would have reported 68.3 % branch coverage against a ≥ 75 % gate — a failure,
   not the pass the roadmap implied. Whole-core figures today: **93.5 % line,
   80.9 % branch**; `src/cbor/` at 80.9 % is the one area under its ≥ 90 %
   elevated gate.

**Discovered / follow-up.** Four table rows updated: two closed (the coverage
metric is now pinned; the stale claim that cppcheck cannot be installed here),
one reframed (invariant guards are a *task* — §6 already rules that such lines
carry an exclusion marker — not an open question), one added (the
`coverage.sh` bug).

**Docs updated.** `CLAUDE.md`, `handoff.md`, `architecture.md`, `api.md`,
`quality-gates.md`, `roadmap.md`, plus `tools/coverage.sh`.

**Recommended next.** **P8 — image management.** Its roadmap entry now carries an
Exit criterion and a "Start here" pointer. The short version: read
[`protocol-notes.md`](protocol-notes.md) §6 **and** §7 together before writing
anything — §7 holds the two-hash distinction that P8 is the first phase able to
get wrong — then copy the shape of `src/groups/os/os_management.cpp` and follow
the four rules in [`design.md`](design.md) §5.
