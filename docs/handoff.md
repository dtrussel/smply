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

Everything below is true of the code as it stands and has already cost a session
once. The log records *how* each was found; this list is what you need before
writing anything. Add to it when a discovery outlives its phase — and delete an
entry when it stops being true.

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
* **A bound that is not the tightest bound is not a bound.**
  `limits::kMaxCborNesting` was 16 against QCBOR's own cap of 15, so smply's
  documented limit never bound — and because `Reader::enter_map(key)`'s
  QCBOR-error path is deliberately non-sticky (it doubles as a probe for the
  optional `err` map that SMP v1 devices never carry), a too-deep document made
  the reader stop descending with `status()` **clean**. Silently missing fields,
  not a decode failure. It is 14 now: reaching the cap needs a document one
  level deeper than the cap, so equalling QCBOR's is not enough. When adding a
  limit, check what the layer *underneath* already enforces.

**Layering**

* Groups are thin. They allocate no sequence numbers, set no deadlines and
  interpret no `rc` — `SmpClient` has done all three before a response arrives.
  `src/groups/os/os_management.cpp` is the shape to copy.
* A callback never runs inside the call that started the operation — argument
  rejections and first-chunk failures included. `SmpClient::defer()` exists for
  exactly that.
* `SmpClientConfig::max_in_flight` is **1** by default; a second concurrent
  request fails with `InvalidState` rather than queueing.
* A response whose `seq` matches but whose group, command or op does not is
  discarded and the request left pending (ADR-0010). Do not "fix" this into a
  failure.

**Lifetime**

* **A transport, and anything a callback captures, must outlive the
  `SmpClient`** — and, since P10, the `ImageManagement` too, because an upload
  session lives there and its destructor completes the callback. Declare them
  *before* both. Includes a transport a rebind test introduces halfway through:
  `~SmpClient` detaches from whichever one it currently holds.

  Three separate bugs have come from this, each caught by only one compiler:
  getting it wrong shows up as "pure virtual method called" under GCC, or as a
  stack-use-after-scope under Clang's ASan, or as nothing at all.

**Protocol facts that bite**

* Where Zephyr's documentation and Zephyr's source disagree, **the source wins,
  and you must read both.** Reset's `force` is the case in point (PN §9, A15):
  the docs say integer, the server decodes a boolean and silently ignores
  anything else. Reading only the `.rst` yields a flag that never works.
* MCUmgr uses **two different hashes** — the upload `sha`, SHA-256 over the whole
  file, and image-state `hash`, MCUboot's `IMAGE_TLV_SHA` over header and body.
  Conflating them is the classic client bug, so they are different types:
  `Hash` (fixed 32 bytes) and `ImageHash` (32 **or 64**, because
  `IMAGE_SHA_LEN` is 64 for a SHA-512 bootloader). Do not merge them.
  `sha256(ImageSource&)` gives the first, `find_image_tlv_hash()` the second.
* **The upload server's `off` is authoritative in every direction** — larger
  than what was sent, smaller, or zero (PN §6 rule 5). Never compute
  `next_off = off + sent`. Two consequences that look like bugs and are not: an
  upload can complete on its *first* packet when the device already holds the
  image (rule 9a), and a retransmitted *final* chunk is answered `off == 0`
  because the server has already reset its session (rule 9b).
* **A retransmission repeats the payload, not the message.** The sequence number
  must differ — the timeout retired the old one, and a reply carrying it would
  be discarded as late (PN §4).
* **`match` is only meaningful if a full 32-byte `sha` was sent** (PN §6, rule
  9c). The final-chunk check compares against the stored `sha` zero-padded, with
  no length guard, so a trimmed or absent `sha` makes a perfectly good upload
  report `match: false` — which rule 9 says means failure. Always send all 32
  bytes.
* **The slot flags of a trial boot read backwards** (PN §7). After a test swap
  the image that is *running* reports `active` with **no** `confirmed`, and the
  slot holding the fallback reports `confirmed`. Flags are derived from the swap
  type, not stored, so there is no "pending bit" to consult instead.
* **Confirming a slot that is not the running one is denied** by an ordinary
  build (`IMAGE_CONFIRMATION_DENIED`). The portable way to make an image
  permanent is test → reset → confirm. Meanwhile re-requesting a swap that is
  *already* scheduled is a success that does nothing, so an idempotent retry
  after a lost response is safe; any other change is `ALREADY_PENDING`.
* **"Absent means false" is only half the rule for image-state flags.** The
  specification says a false flag is omitted; the server omits it only under
  `CONFIG_MCUMGR_GRP_IMG_FRUGAL_LIST` and otherwise sends it explicitly. Absent
  ⇒ false; present ⇒ whatever it says. Never read "key present" as "true".
* **A group-scoped error code often does not reach a v1 client** (PN §9, A16).
  The server may translate it onto `mcumgr_err_t` and rebuild the response, so
  `image_error()` returning `nullopt` for a real image failure is normal. Check
  `smp_error()` as well, and never treat the absence as a malformed reply.
* **The MCUboot TLV trailer is not what the design document implies** (PN §7,
  written from `bootutil_tlv_iter_begin()`): `it_tlv_tot` includes its own
  four-byte area header, `ih_protect_tlv_size` must equal the protected area's
  `it_tlv_tot` exactly, and the two areas are walked as one contiguous run. A
  scan also cannot spin — every advance is at least the four-byte entry header —
  so `limits::kMaxImageTlvs` bounds work, not termination.

**Before you trust a green run**

* **A failed build leaves the previous test binary in place**, so `ctest` then
  reports the *old* suite passing. Check the build's exit status separately;
  never read "N tests passed" as evidence anything was rebuilt. This has bitten
  in four consecutive phases.
* **Build every preset.** `cmake --list-presets` shows **seven** Linux ones and
  **all seven link here**, `linux-clang-asan-ubsan` included.

  This corrects a caveat that stood from P1 to P12 and shaped three phases of
  work. It said Clang's compiler-rt "is not installable in this container", so
  dangling callback captures were a **CI-only** bug class. That was never true:
  `libclang-rt-18-dev` is an ordinary Ubuntu package — the very one
  `.github/workflows/ci.yml` has been installing for its sanitizers job all
  along. The install fails with a 404 on a stale index and succeeds after
  `apt-get update`, which is presumably how the original conclusion was reached.
  **Run `apt-get update` before believing any "not installable" claim**, and
  build the Clang sanitizer preset locally: it is no longer CI-only, and it is
  the one that catches lifetime bugs GCC's ASan does not report at all.

  **The two MSVC jobs remain CI-only**, and MSVC is a *third* opinion, not a
  rounding error on the other two. Its `/w14242` rejected
  `std::pair<std::uint16_t, std::uint8_t>{0, 6}` in P12 — the `int` literals
  narrow inside pair's constructor template, where the "constant that fits"
  exemption no longer applies — after both GCC and Clang compiled it silently.
  An aggregate `struct` with the same two fields is fine, because aggregate
  initialisation from constant expressions that fit is not narrowing. **Prefer
  a named aggregate to a `std::pair` of narrow integers**, and expect the
  Windows jobs to find something the Linux ones did not.

  The six that do link still disagree with each other, in both directions.
  `-Wuseless-cast` is GCC-only and rejects a `static_cast` between
  `std::uint64_t` and `std::size_t` — the same type on a 64-bit host, a real
  narrowing on a 32-bit one; `image::narrow<To>()` in
  `src/image/source_reader.hpp` is the way round it. Clang in turn rejects
  things GCC compiles happily: `std::vector<std::pair<std::string, T>>` inside
  `T` is undefined, because a `std::pair` of an incomplete type is, while
  `std::vector<T>` inside `T` is specifically allowed.
* **Never edit a source file while a background build is running.** Doing it
  once cost an hour: the objects came out mixed, `cmake --build` then reported
  **success** because Ninja saw nothing newer than what it had, and the binary
  segfaulted in one preset only. This is the stale-binary trap wearing a
  disguise — a green build that is not a build of the tree you have. If a
  preset fails in a way that makes no sense, `rm -rf build` before believing
  it.
* **Read the uncovered-line list, not the percentage.** In P8 the gap between
  92 % and 95 % was a dozen genuinely reachable bounds checks, not the
  unreachable guards the number suggested. In P9 it exposed something worse:
  three tests that passed **without reaching the check they were named after**,
  because a fixture-builder convenience kept two fields agreeing. *A
  malformation knob must change exactly one field, or it cannot express an
  inconsistency.*

**Tooling**

* **Install `cppcheck`, `gcovr` and `libclang-rt-18-dev` first**, after an
  `apt-get update`. The first two fail soft: `tools/lint.sh` skips cppcheck silently, and
  `tools/coverage.sh` falls back to plain `gcov`, whose branch metric is not
  comparable. `apt-get install -y cppcheck && pip install gcovr`.
* Coverage means exactly what `tools/coverage.sh` reports (gcovr with
  `--exclude-throw-branches`). The same objects move ~12 points under a
  different flag. Running gcovr by hand, put the search path **first**:
  `--txt <build-dir>` takes the directory as that option's output file and
  fails — the mistake that silently disabled `coverage.sh` from P0 to P7.
* **The thresholds are enforced from P13**: CI runs
  `tools/coverage.sh <build-dir> --enforce`, which fails below 85 % line or
  75 % branch and **refuses to run at all without gcovr** rather than falling
  back to a different measurement. `tools/verify_gates.sh` now proves the
  reporter rejects, accepts and refuses, so it can no longer be silently inert.
* **Delete the `.gcda` files before re-measuring.** Building over an existing
  coverage build prints `libgcov profiling error: … overwriting an existing
  profile data with a different checksum` and then mixes counts from two
  versions of the code. `find <build-dir> -name '*.gcda' -delete`, then re-run
  the tests. The warning scrolls past in build output; the number that follows
  looks perfectly ordinary.
* **`LCOV_EXCL_LINE` excludes the line it sits on and nothing else.** Not the
  block it introduces, and *not* if it is on a comment line above the code —
  there it is silently ignored. For a multi-line guard use `LCOV_EXCL_START` /
  `LCOV_EXCL_STOP`, and put the `STOP` inside the guard when the `else` arm is
  the ordinary path.
* clang-tidy over the full tree takes minutes. Run it in the background and
  collect the result rather than blocking on it — but **not at the same time as
  `tools/verify_gates.sh`**. That script points the scratch build at the real
  tree's `build/linux-clang/_deps` cache, and rewriting a header clang-tidy has
  mmapped kills it with a **bus error** that looks like a compiler crash and is
  not one. **cppcheck now parses the
  Catch2 suites** — `tools/lint.sh` gives it the include paths and
  `-UCATCH_CONFIG_DISABLE -UCATCH_CONFIG_PREFIX_ALL`, without which it explores
  a configuration where `TEST_CASE` is undefined and reports a `syntaxError` at
  the first one. A full cppcheck pass over `include src tests` takes about three
  minutes; run it in the background too.
* **The fuzz targets are not part of `ctest`.** `cmake --preset
  linux-clang-fuzz`, then run a target with its corpus directory as the
  argument. Give it a *copy* of `tests/fuzz/corpus/<target>/` unless you mean to
  grow the committed corpus: libFuzzer writes what it discovers into the
  directory it is given.
* `??>` in a C++ string literal is a **trigraph**, and `-Werror` rejects it. The
  device's `<???>` version placeholder needs a raw string literal.

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

### 2026-09-05 — P8: image management group

**Status after this session:** P8 = `Complete`. Next phase: **P9 — MCUboot image
file handling and SHA-256**.

**Completed.** `include/smply/groups/image.hpp` and
`src/groups/image/image_management.cpp`: image state read and write, erase, slot
info, plus `ImageHash`, `ImageVersion`, the `ImageState` accessors and
`ImageError`/`image_error()`. 69 new tests (308 total). All gates green across
the five Linux presets.

Request vectors are hand-derived from the CBOR grammar, as in P7. Responses are
built by an **independent** encoder written in the test file from RFC 8949's head
encoding, sharing no code with `src/cbor/`, so a decoder bug cannot be cancelled
out by a matching encoder bug — and `golden_state_response()` is written out byte
by byte and asserted against that builder, which is what keeps the builder
honest. Copy the pattern in P10; it is the only way a response-decoding suite
proves anything.

**Protocol work.** Six findings, all from reading `img_mgmt.c`,
`img_mgmt_state.c`, `img_mgmt_util.c`, `img_mgmt_priv.h` and `smp.c` rather than
`smp_group_1.rst`. All are in [`protocol-notes.md`](protocol-notes.md) §6/§7,
with four new sources (S14–S17). The roadmap's P8 outcome lists them; the three
that will bite a later phase are in the caveats below.

**Changed.** Six deviations from `api.md`'s proposal, in the roadmap's P8
outcome. The one that reaches other phases: **the device-reported hash is its own
type, `ImageHash`, not `Hash`.**

**Remaining in this phase.** None.

**Caveats — read these before P9, P10 and P12.**

* **There are two hash types now, and that is deliberate.** `Hash` is the fixed
  32-byte SHA-256 smply computes over the whole firmware file — the upload
  `sha`. `ImageHash` is what a *device* reports for a slot: MCUboot's
  `IMAGE_TLV_SHA`, which is 32 bytes normally and **64 under
  `CONFIG_MCUBOOT_BOOTLOADER_USES_SHA512`**, so it carries its own length. P9
  computes the first and reads the second out of a file's TLVs; P12 compares
  them. `ImageHash::from(const Hash&)` is the one legitimate crossing. Do not
  "simplify" them back into one type — the whole point is that the classic
  MCUmgr client bug now fails to compile.
* **"Absent means false" is only half the story.** The specification says the
  state flags are omitted when false; the *implementation* omits them only under
  `CONFIG_MCUMGR_GRP_IMG_FRUGAL_LIST`, and the default build sends every flag
  explicitly. A decoder that reads "the key is present" as "the flag is set"
  misreads every ordinary device. Absent ⇒ false; present ⇒ whatever it says.
* **An image-group error code often does not reach the client at all** (A16).
  Over SMP v1 — smply's default — a server with
  `CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL` translates the group code onto
  `mcumgr_err_t` and *rebuilds the whole response*, so `HASH_NOT_FOUND` arrives
  as `EUNKNOWN` and `NO_FREE_SLOT` as `EBADSTATE`. `image_error()` returning
  `nullopt` for a real image failure is therefore normal, not a malformed reply;
  check `smp_error()` too. P12 cannot build retry policy on the group code alone
  without opting into v2 (open question O2).
* **A device's version string is dotted and may not parse at all.**
  `img_mgmt_ver_str()` writes `"1.2.3"`, appending `".4"` only for a non-zero
  build number, and substitutes the literal `<???>` when it cannot format one.
  `ImageVersion::parse` accepts that, plus imgtool's `"1.2.3+4"`; `ImageSlot`
  keeps the raw string so a device that reports nonsense is still readable.
  (In C++ source that placeholder needs a raw string literal — `??>` is a
  trigraph, and `-Werror` rejects it.)
* **Slot info is advisory, and one bad flash area does not spoil it.** A slot
  entry may carry its own nested `"rc"` instead of a `"size"` when
  `flash_area_open()` fails — undocumented in the specification. It is nested,
  so it can never be confused with the message-level `rc`.

**One defect fixed outside the phase's own files.**
`cbor::Reader::for_each_map_in_array` rejected an array of **exactly**
`max_elements`: the cap was tested before entering an element, so the end of the
array was never looked for and the last legal element failed. An image-state
response with exactly `limits::kMaxImages` entries did not decode. Fixed in
`src/cbor/reader.cpp`; `test_cbor.cpp` gained the boundary case it never had —
the existing test used eight elements against a cap of three, which passes
either way. **The general lesson: a cap test that is comfortably over the cap
does not test the cap.** P10's chunk-size arithmetic has the same shape.

**On coverage.** `src/groups/image/image_management.cpp` is at 95 % line and
89 % branch; the core moved from 93.5 % / 80.9 % to 94.6 % / 82.4 %. What matters
is how it got there: the first measurement was 92 % / 79 %, and the gap was *not*
defensive code — it was a dozen reachable bounds checks that no test touched.
Reading the uncovered-line list found them; the percentage alone would not have.
`gcovr` is **not** installed in the container by default and `tools/coverage.sh`
silently falls back to plain `gcov`, whose branch metric is not comparable
(`pip install gcovr` first).

**Docs updated.** `protocol-notes.md` (S14–S17, §6 rewritten in five places, §7
hash table, A16), `api.md` (`groups/image.hpp` now Shipped, upload split into its
own Proposed subsection), `design.md` (§5 decoding rules, the two hash types, and
why a group code may not survive), `architecture.md` (§10 layout), `roadmap.md`
(P8 Complete with outcome, six deviations, four follow-ups, O2 gains A16 as
evidence, Current state), this log.

**Recommended next.** **P9 — MCUboot image file handling and SHA-256.** It is
independent of P8 (it depends only on P1), and P10 needs both. Start with the
header decode and the NIST vectors; the TLV scanner is where the hardening
matters, and `IMAGE_TLV_SHA512 = 0x12` exists alongside `IMAGE_TLV_SHA256 =
0x10`, so decide early whether P9 reads only the 32-byte one.

### 2026-09-05 — P9: MCUboot image file handling and SHA-256

**Status after this session:** P9 = `Complete`. Next phase: **P10 — Image upload
state machine**, which now has everything it needs.

**Completed.** `include/smply/image_source.hpp`,
`include/smply/mcuboot_image.hpp` and `src/image/` (five files): the
`ImageSource` seam, `MemoryImageSource`, the 32-byte header parse, streaming
SHA-256, and the TLV scan. 61 new tests (369 total). All gates green across the
five Linux presets.

**Protocol work.** §7 of [`protocol-notes.md`](protocol-notes.md) described the
TLV trailer in two sentences and cited only the MCUboot design document. It is
now written from `bootutil_tlv_iter_begin()`/`_next()` — the code that actually
enforces the layout — with three new sources (S18-S20). The roadmap's P9 outcome
lists all four findings; the ones that will bite later are in the caveats below.

**Changed.** Seven deviations, in the roadmap's P9 outcome. The two that reach
other phases: **two public headers instead of one**, and
**`find_image_tlv_hash` returns `std::optional<ImageHash>`**, not `Hash`.

**Remaining in this phase.** None.

**Caveats — read these before P10 and P12.**

* **The file's hash and the device's hash are different types, and P9 is where
  both become available.** `sha256(ImageSource&)` gives you `Hash` — 32 bytes
  over the whole file, the upload `sha`. `find_image_tlv_hash()` gives you
  `ImageHash` — `IMAGE_TLV_SHA256`/`384`/`512` over header and body, the thing
  `ImageSlot::hash` reports. P10 wants the first, P12 wants the second. They do
  not convert into each other and that is the point.
* **`it_tlv_tot` includes its own four-byte area header**, and
  `ih_protect_tlv_size` must equal the protected area's `it_tlv_tot` exactly.
  Both areas are walked as one contiguous run. Do not re-derive this from the
  MCUboot design document — it is not in there; PN §7 now has it from the
  scanner.
* **A TLV scan cannot spin.** Every advance is `4 + it_len`, so it is at least
  the entry header. The cap in `limits::kMaxImageTlvs` bounds work, not
  termination. ADR-0009 used to list "strictly-positive advance" as a
  safeguard; that has been corrected, because documenting a property as a guard
  sends the next reader looking for a bug that cannot exist.
* **`ImageSource` allows a short read only at the end of the image.** Everything
  in `src/image/` refuses one anywhere else with `InvalidArgument` rather than
  looping — a source returning one byte per call would otherwise turn a 16 MiB
  hash into sixteen million virtual calls. Keep that rule in P10's chunk reads.
* **An encrypted image is not scanned at all**, and that is `std::nullopt`
  rather than an error (A13). P12 has to treat "no hash to correlate" as a
  normal outcome, not a reason to refuse an update.

**Two tooling lessons, both already-known traps that bit anyway.**

* **`-Wuseless-cast` is GCC-only**, and it rejected a `static_cast<std::size_t>`
  over a `std::uint64_t` — the same type on a 64-bit host, a real narrowing on a
  32-bit one. Clang built it happily; only the GCC preset caught it. The fix is
  a small `image::narrow<To>()` template, whose dependent conversion satisfies
  both. **Build every preset before believing a change compiles.**
* **A failed build leaves the previous test binary in place**, and `ctest`
  cheerfully reported 364 passing off a binary whose build had just failed.
  Third session in a row this has come up. Read the build's exit status
  separately, every time.

**On coverage, and on tests that pass for the wrong reason.** `src/image/` is at
98 % line and 93 % branch; the core moved to 95.4 % / 82.7 %. The three uncovered
lines are unreachable invariant guards, the same P13 exclusion-marker task as
before.

The uncovered-line list earned its keep more sharply than in P8. It showed three
TLV tests sitting on lines that had never executed — because one `ImageBuilder`
call set both `ih_protect_tlv_size` *and* the protected area's own `it_tlv_tot`,
so the two agreed and the check the tests were named after was never reached.
They passed, for a different reason than they claimed. The builder now has one
knob per field, and `testing.md` §3 says why. **When a fixture builder offers a
convenience that keeps two fields consistent, it cannot express an
inconsistency** — which is the only thing a malformed-input test is for.

Also: running `gcovr … --txt <build-dir>` by hand fails with "Is a directory" —
`--txt` eats the next argument as its output file. That is the exact bug that
silently disabled `tools/coverage.sh` from P0 to P7, and it is just as easy to
re-create on the command line. Put the search path first, or omit `--txt`.

**Docs updated.** `protocol-notes.md` (S18-S20, §7 rewritten), `api.md`
(`image_source.hpp` and the new `mcuboot_image.hpp`, both Shipped),
`design.md` (§7: the header split, the layout rules, why the cap is not a loop
guard, the `Version`/`ImageVersion` name fix), `architecture.md` (§10),
`dependencies.md` and `ADR-0009` (SHA-256 provenance; the decision is unchanged,
so both are amendments rather than a superseding ADR), `testing.md` (§3),
`quality-gates.md` (§6 measured position, and the `--txt` trap), `roadmap.md`
(P9 Complete with outcome, seven deviations, four follow-ups, O4 resolved,
Current state), this log.

**Recommended next.** **P10 — the image upload state machine**, the densest
phase in the project and the protocol core of the product. Read
[`design.md`](design.md) §6 and [`protocol-notes.md`](protocol-notes.md) §6's
twelve verified server rules together before writing anything; the rule that
shapes everything is **the server-returned `off` is authoritative — never
compute `next_off = off + sent`**. `upload_session.*` is specified as a pure
function precisely so the whole response table in
[`testing.md`](testing.md) §3 can be driven with no client, transport or clock.

### 2026-09-05 — P10: image upload state machine

**Status after this session:** P10 = `Complete`. Next phase: **P11 —
`ServerSimulator` and the component harness**.

**Completed.** `src/groups/image/upload_session.{hpp,cpp}` (pure decision
logic), `src/groups/image/upload_driver.{hpp,cpp}` (the I/O half), and
`ImageManagement::upload`/`resume`/`cancel` with the upload types in
`groups/image.hpp`. 70 new tests (439 total). All gates green across the five
Linux presets, and **the phase's own elevated gate is met**: `upload_session.*`
is at 94 % branch and 99 % line against the ≥ 90 % threshold.

Upload now works standalone, which was the phase's exit criterion: a caller can
push an image with no DFU orchestration in existence.

**Protocol work.** Three behaviours were missing from §6 and are now rules 5,
9a and 9b — the roadmap's P10 outcome has them in full. The two that will shape
P12 are in the caveats below.

**Changed.** Seven deviations, in the roadmap. The three that reach other
phases: **there is no `Restart` action**, **`UploadHandle` is a token rather
than an owner**, and **a disconnect completes the callback rather than silently
suspending**.

**Remaining in this phase.** None.

**Caveats — read these before P11 and P12.**

* **An upload can finish on the first packet, having sent no data** (§6 rule
  9a). Given a full 32-byte `sha`, the server checks whether the slot already
  holds this exact image and jumps to the end. P12 gets "is the device already
  holding this image?" for free — no image-state round trip needed — and P11's
  simulator must model it or the DFU tests will never exercise the path.
* **A retransmitted final chunk is answered with `off == 0`** (rule 9b), because
  the server resets its session on completion. That reads as "restart", which is
  correct: the first packet then completes via 9a in one round trip. The
  simulator should reproduce this, because it is the one place a *successful*
  upload can look like a failure.
* **The server's `off` is authoritative in every direction** — larger than what
  was sent, smaller, or zero. Nothing computes `next_off = off + sent`, and
  nothing should start.
* **A disconnect completes `on_done` with `Disconnected` and keeps the
  session.** `resume()` re-sends a first packet with the same `sha` and adopts
  whatever comes back. P12 drives that; it does **not** need to track the
  offset itself.
* **Adopting an offset from a first packet does not charge the no-progress
  budget.** A resume landing back where it started is a success, not a stall.
  This is not in design.md's original table; it is now.
* **`ImageManagement` is stateful from this phase on.** It is neither copyable
  nor movable, it must outlive its upload, and destroying it mid-upload
  completes the callback inline — the same exception `~SmpClient` makes.

**Three traps, all of which cost a cycle.**

* **Clang's ASan caught two tests that GCC's ran happily** — the CI-only bug
  class P1 recorded, seen for the first time. A second `Outcome` declared
  *after* the fixture is destroyed before `~ImageManagement` completes the
  upload still holding its callback. The rule now has two levels: whatever a
  callback touches must outlive **both** the client and the group. Declare it
  first, always.

  Chasing that also exposed a real invariant violation: a first-chunk read
  failure completed the callback **inside** `upload()`, which §5 rule 4
  forbids. `UploadDriver` now defers while `start()`/`resume()` is on the stack,
  and `upload()` returns an invalid handle in that case — the same shape
  `SmpClient::request()` has always had.

* **A retransmission repeats the *payload*, not the message.** The SMP header
  necessarily differs: the timeout retired the old sequence number, so a reply
  carrying it would be discarded as late (§4). The first version of the test
  asserted whole-message equality — the test was wrong about the protocol, not
  the code. Assert on payloads when you mean "the same request".
* **A `FakeTransport` declared *after* the fixture aborts on "pure virtual
  method called" — under GCC only.** `~SmpClient` detaches from the transport it
  currently holds, so every transport it was ever bound to must outlive it;
  `smp_client.hpp` says so and the resume test violated it. Clang ran the same
  UB without complaint. **Declare every transport before the client**, including
  the one a rebind test introduces halfway through.

**On coverage.** Whole core 95.6 % line, 82.3 % branch. `upload_session.*` meets
its own gate; the driver is at 94 % line, and its uncovered remainder is the
unreachable buffer guard plus two `!active_` re-entry guards. Reading the
uncovered-line list again paid for itself: it produced two tests worth having on
their own merits — a wrong-typed `off` (the reader poisons rather than reporting
absence) and a device reporting `buf_size == 0`.

**Docs updated.** `protocol-notes.md` (§6 rules 5, 9a, 9b, and the `sha` length
note), `design.md` (§6: no `Restart`, `record_sent`, the disconnect semantics,
where `server_buf_size` comes from, the retransmission's sequence number, the
first-packet row in the response table), `api.md` (the upload half of
`groups/image.hpp` now Shipped, and `SmpClient::transport_max_message_size()`),
`architecture.md` (§6 state ownership, §10 layout), `ADR-0008` (two amendments),
`testing.md` (§3 rewritten for both suites), `quality-gates.md`, `roadmap.md`
(P10 Complete with outcome, seven deviations, four follow-ups, Current state),
this log.

**Recommended next.** **P11 — `ServerSimulator` and the component harness.**
Everything below it is now testable in isolation; what nothing yet proves is the
*sequence* — that `FirmwareUpdater` will issue the right commands in the right
order. Model rules 9a and 9b in the simulator from the start: they are the two
places a correct client looks like a broken one, and a simulator that cannot
produce them will let P12 ship a plausible bug.

### 2026-09-06 — Documentation pass: bringing the set back level with the code

**Status after this session:** no phase changed. P10 remains `Complete`; **P11**
is next. This was an accuracy-then-clarity review of all ten documents against
the code as it stands after P8, P9 and P10, not phase work.

**Completed.** Read the doc set the way [ADR-0013](decisions/ADR-0013-living-documentation.md)
requires it to be true — every shipped section read next to the header it
describes, every example checked against the real signatures — and fixed twelve
defects. Three were substantive.

**The three that mattered.**

1. **`README.md` was ten phases stale.** It still said "Scaffolding complete
   (roadmap phase P0) … the library itself is still a placeholder", which is the
   first thing anyone reads. It now states P0–P10, says plainly what is *not*
   built (`FirmwareUpdater`, the simulator, fuzzing, the WinRT transport, the
   examples) and carries a working upload snippet — an upload is the thing the
   library can do today, and nothing showed it.
2. **`api.md`'s cancellation example did not compile.** It called
   `handle.cancel()`; `UploadHandle` is a token and the operation lives on the
   group, so it is `img.cancel(handle)`. The example now says why, because that
   shape is deliberate (ADR-0008) and will look like an oversight otherwise.
3. **Two ADR-0013 violations of the same kind: a shipped section that no longer
   matched its header.** `api.md`'s `limits.hpp` list was six constants short,
   and `architecture.md` §9 listed limits under snake_case names that have never
   existed in the code *and* implied every one was overridable through
   `SmpClientConfig` — five of them are not. Both are now tables generated from
   the real `kXxx` names, and §9 gained an "Override" column, which is the
   question a reader actually has.

**Changed.** The rest were clarity, each in service of a reader with a
particular question:

* **`handoff.md` § Standing caveats restructured.** It had accreted to the point
  of repeating itself — the transport-lifetime rule appeared under both
  *Lifetime* and *Tooling traps*, stated differently. The lifetime rule is now
  one entry with P10's second level ("must outlive the `SmpClient` — and, since
  P10, the `ImageManagement` too"); *Protocol sources* became **Protocol facts
  that bite** and is ordered by how often each one bites; *Tooling traps* split
  into **Before you trust a green run** (the three that make a green run a lie)
  and **Tooling** (setup). Nothing was dropped.
* **`architecture.md` §11** no longer describes `UpdatePlan` and the HIL suite as
  though they exist, and records what a disconnect does to an upload.
* **`quality-gates.md`** is "as of P10" rather than "from P0", and the reason
  coverage thresholds are not yet enforced is now the two real blockers
  (`src/cbor/` under its elevated gate; the missing exclusion markers) instead of
  "P0's placeholder library", which stopped being true at P1.
* **`design.md` §5**'s `ImageManagement` sketch carries the real
  `upload`/`resume`/`cancel` signatures.
* **`testing.md`** documents the image doubles P9 added — `ImageBuilder`,
  `FailingImageSource`, `ShortReadingImageSource` — which existed with no entry
  anywhere.
* **`roadmap.md`** gained a P11 **Start here** and an Exit criterion, and the
  open-question count is corrected (O4 was resolved in P9; four remain).
* **`CLAUDE.md`**'s "two ways this has gone wrong" is now three, adding gcovr's
  soft failure and "build every preset, not just one".

**Discovered / follow-up.** None new. No code changed: `format --check`, the
three `check_*.py` gates and all 439 tests pass, as they must for a
documentation-only change.

**Caveats.** The doc gate (`check_docs.py`, R1–R4) proves the *structure* — that
every ADR is referenced, every phase has an outcome, and so on. It cannot tell
that a shipped section has drifted from its header, which is exactly what two of
the three real defects were. **Read the header next to the section**; the gate
will not do it for you, and drift accumulates silently between phases.

**Docs updated.** `README.md`, `CLAUDE.md`, `api.md`, `architecture.md`,
`design.md`, `handoff.md`, `quality-gates.md`, `roadmap.md`, `testing.md`.

**Recommended next.** **P11 — `ServerSimulator` and the component harness**,
unchanged by this pass. Its roadmap entry now carries the Start here that P10's
outcome argued for: build rules 9a, 9b and offset correction into the simulator
from the beginning rather than adding them once a DFU test needs them.

### 2026-09-06 — P11: `ServerSimulator` and the component harness

**Status after this session:** P11 = `Complete`. Next phase: **P12 —
`FirmwareUpdater` orchestration**.

**Completed.** `tests/support/server_simulator.{hpp,cpp}` (a deterministic
in-memory MCUmgr device: groups 0 and 1, an MCUboot-like swap, and the awkward
answers), `tests/support/test_cbor.{hpp,cpp}` (an independent CBOR codec), and
`tests/component/` as a second executable — `harness.hpp`, `test_simulator.cpp`
and `test_round_trip.cpp`. 48 new tests (487 total). All gates green across the
six Linux presets that link here, and in CI.

**The acceptance criterion is met**: a full upload through the real client stack
into the simulator reproduces the source image byte for byte, in **both** SMP
versions — and still does across a forced restart, a device reboot mid-transfer,
a disconnect and resume, and an offset correction naming a position *ahead* of
what the client sent.

**Protocol work — four findings, three of which constrain P12.** The full text
is in [`protocol-notes.md`](protocol-notes.md) §§6-7 and the roadmap's P11
outcome; the ones to carry in your head:

* **`match` is only meaningful with a full 32-byte `sha`** (new rule 9c). The
  final-chunk check has no length guard and compares against the stored `sha`
  zero-padded, so trimming or omitting it makes a good upload report
  `match: false`.
* **Slot flags are derived from the swap type, and a trial boot reads
  backwards**: the running image is `active` with **no** `confirmed`, and the
  slot holding the fallback is the `confirmed` one. That is how `RolledBack`
  gets detected, and there is no stored flag to consult instead.
* **Confirming a slot that is not running is denied by default.** P12 cannot
  implement "confirm immediately" by confirming the uploaded image before the
  swap; the portable order is test → reset → confirm.
* The swap-type state machine is now written down (§7), from MCUboot's
  `boot_swap_tables`, `boot_swap_type_multi()` and `boot_set_next()` — a new
  source, S21.

**Changed.** Seven deviations, all in the roadmap. Three reach other phases:
`ServerConfig` has **no SMP version** (the client chooses it, and a server
answers in the version it was asked in); scripted faults are **methods, not
config fields**, so a config stays a description of a device; and
`answer_offset_once()` changes only the *answer*, never the flash, which is what
keeps the byte-exact comparison meaningful.

**Remaining in this phase.** None.

**Caveats — read these before P12.**

* **Where reuse is safe, and where it is not.** The simulator hashes with
  `image::Sha256` and that is fine: SHA-256 is pinned by the published FIPS
  180-4 vectors, so a bug there fails `test_sha256.cpp` first and cannot cancel
  out. It does **not** use `cbor::Reader`/`Writer`, because those are anchored
  only by hand-built vectors in the unit tests — encode and decode with them on
  both ends of a round trip and a symmetric bug proves only that smply agrees
  with itself, which is precisely what this phase's acceptance criterion exists
  to catch. Apply the same test to anything else you are tempted to share with
  a double: *what is it anchored to?*
* **The simulator is not a `Transport`.** It watches `FakeTransport::sent()` and
  answers from `pump()`, because `Transport::send()` may not deliver inbound
  bytes before returning — replying inline re-enters reassembly, which the
  assembler refuses. Two mechanics inside `pump()` are easy to get wrong and
  are commented as such: snapshot `sent().size()` at entry (client callbacks
  send the next chunk *during* the loop), and copy each message before
  dispatching it (the same growth reallocates the vector it was borrowed from).
* **A test driving the simulator must not call `clear_sent()`**, and every
  transport the client is ever bound to must be declared before the fixture —
  including the replacement link a reconnect test introduces.
* **`run_until()` has an iteration budget, not a timeout.** A state machine that
  stops making progress fails in bounded time instead of hanging CI. Keep it
  that way.
* **48 component tests moved `src/` coverage by nothing at all** (95.6 % line,
  82.3 % branch, unchanged from P10). They cover *sequences*, which line
  coverage cannot see. Do not read a flat coverage number as a measure of what
  they are worth — in either direction.

**Two traps, one of them new.**

* **Never edit a source file while a background build is running.** The objects
  came out mixed, `cmake --build` reported **success** because Ninja saw nothing
  newer than what it had, and the binary segfaulted under one preset only. This
  is the stale-binary trap in disguise: a green build that is not a build of the
  tree you have. `rm -rf build` before believing a nonsensical failure.
* **Clang rejects `std::vector<std::pair<std::string, T>>` inside `T`** — a
  `std::pair` of an incomplete type is undefined — while GCC compiles it
  happily. `std::vector<T>` inside `T` is the one nesting the standard allows.
  The CBOR map is stored flattened as key, value, key, value instead, which is
  how CBOR encodes one anyway.

**Docs updated.** `protocol-notes.md` (rule 9c, the §7 swap and flag tables, the
set-state refusals, S21 in the inventory), `testing.md` (§2 `ServerConfig`
reconciled with the real one and the pump loop documented, §4 rewritten to
separate what ships now from what waits for P12), `architecture.md` (§10
layout), `quality-gates.md` (measured at P11, and why the numbers did not move),
`roadmap.md` (P11 Complete with outcome, four findings, seven deviations, three
new follow-ups and one closed; a **Start here** for P12), this log.

**Recommended next.** **P12 — `FirmwareUpdater` orchestration.** Its roadmap
entry now carries a Start here, and the short version is that two of P11's
findings change the design before it is written: `ConfirmImmediately` cannot
work by confirming before the swap, and `RolledBack` is detected from the
inverted flags of a trial boot rather than from anything named "pending". Write
the pure state machine first, as P10 did, and drive it into the simulator from
`tests/component/` — `harness.hpp` already has the fixture and the bounded loop.

### 2026-09-06 — P12: `FirmwareUpdater` orchestration

**Status after this session:** P12 = `Complete`. **The portable product is
functionally complete.** Next phase: **P13 — fuzzing, hardening and the
coverage push**.

**Completed.** `include/smply/dfu/firmware_updater.hpp`,
`src/dfu/update_state_machine.{hpp,cpp}` (the pure machine) and
`src/dfu/firmware_updater.cpp` (the effects), plus
`tests/unit/test_update_state_machine.cpp` and
`tests/component/test_firmware_update.cpp`. 72 new tests (559 total). All gates
green across the six Linux presets that link here, and in CI.

**The gate is met**: `src/dfu/` at **93 % branch** against ≥ 90 %, and the whole
core rose to **96.3 % line, 84.3 % branch**.

**The decision that had to be made first — [ADR-0014](decisions/ADR-0014-confirmation-is-the-applications-call.md).**
`UpdateMode::ConfirmImmediately` could not do what `api.md` promised, and the
replacement P11 sketched was wrong too: it assumed the default flow had a pause
to remove, when `Confirming` followed `VerifyingBooted` automatically. That made
the two modes identical and left `TestThenConfirm` naming a test nothing
performed. **The default now stops at `AwaitingConfirmation`** and the
application calls `confirm()`; `ConfirmImmediately` runs the same sequence
without asking.

**Changed.** Seven deviations, all in the roadmap. The ones that reach other
phases: the new public `confirm()` / `AwaitingConfirmation` /
`ConfirmationRequired`; `UpdateReport` gained `revert_pending` and
`upload_skipped` and lost the two retry counters nothing filled in; and
`target_hash` is an `ImageHash`, not a `Hash` — `api.md` had the wrong one of
the two hashes P8 deliberately made separate types.

**Remaining in this phase.** None.

**Caveats — read these before P13.**

* **A fault injector that cannot name its target will hit the wrong one.**
  `ServerSimulator::fail_next()` failed "the next image-group command", and the
  group's read and write share command 0 — so two tests that meant to refuse a
  *set-state* were silently refusing the *get-state* before it. Both were green
  without ever reaching the path they are named after. It now takes an
  `Operation` filter. This is the same trap P9 hit with a fixture builder, in a
  different costume; assume any injector without a precise target is hitting
  something adjacent.
* **The default update mode does not finish on its own.** An application that
  ignores `ConfirmationRequired` parks in `AwaitingConfirmation` forever. That
  is deliberate (ADR-0014) and it is the one place where the obvious default is
  not the passive one — expect it to surprise someone, and keep the example in
  `api.md` showing the handler.
* **A `FakeTransport` is terminally disconnected once dropped**, as a real link
  is. An update that goes round the reconnect loop twice — an interrupted upload
  does, once for the transfer and once for the reset — needs a fresh transport
  per cycle. `tests/component/test_firmware_update.cpp`'s `Application` helper
  takes a list of spares for exactly that.
* **`Planning` is a real state**, entered with `Effect::Continue` which the
  driver feeds straight back into `advance()`. A decision that needs no I/O
  would otherwise be invisible, and a state you cannot observe is a state you
  cannot test.

**One trap, and it is the standing one.**

* **The first coverage measurement was 81 %, against a 90 % gate — and the gap
  was one branch repeated.** The "event not legal in this state" fall-through
  was exercised in one state out of fourteen, because the test that covered it
  named a single state. Looping it over every state took `src/dfu/` from 81 % to
  90 % by itself. The percentage said "write more tests"; the uncovered-branch
  list said exactly which one, and it was a real hole — a state that silently
  swallowed a stray event would have been a live bug. **Read the list.**

**Docs updated.** `ADR-0014` (new) and the ADR index, `design.md` (§8 diagram,
modes, event table and failure table), `api.md` (the DFU section now Shipped,
with the new state, event and method, and an example that handles them),
`architecture.md` (§10: `src/dfu/` no longer planned), `testing.md` (§3 and §4
for both new suites), `quality-gates.md` (measured at P12, the elevated row now
naming `src/dfu/`, and why the first measurement missed), `roadmap.md` (P12
Complete with outcome, seven deviations, three new follow-ups, Current state),
`README.md` (P0–P12, and an example that is now a whole update), this log.

**Recommended next.** **P13 — fuzzing, hardening and the coverage push.** Two of
its inputs are now concrete and both are in the follow-up table: `src/cbor/` is
still the one directory below its elevated gate at 82 %, and the invariant
guards that need coverage-exclusion markers now include six in
`FirmwareUpdater` — the `life.expired()` checks, of which only one is reachable
without contriving the exact request in flight. The fuzz targets are specified
in `testing.md` §5 and none is built.

### 2026-09-06 — P13: fuzzing, hardening and the coverage push

**Status after this session:** P13 = `Complete`. Next phase: **P14 —
`Dispatcher` and the portable example**.

**Completed.** `tests/fuzz/` with all seven targets from `testing.md` §5, their
corpora and a `linux-clang-fuzz` preset; `tests/unit/test_limits.cpp` (new, one
case per `architecture.md` §9 constant); the coverage thresholds enforced;
`linux-clang-fuzz-smoke` in `ci.yml` and a scheduled `nightly-fuzz.yml`. 31 new
tests (**590** total, as `ctest` counts them), green across **all seven**
Linux presets.

**The audit found a real, silent defect.** `limits::kMaxCborNesting` was 16
against QCBOR's own cap of 15, so the documented bound was never the effective
one — and `Reader::enter_map(key)`'s QCBOR-error path is deliberately
non-sticky, because it doubles as a probe for the optional `err` map that SMP v1
devices never carry. A document nested deeper than QCBOR allows therefore made
the reader stop descending with `status()` **clean**: silently missing fields,
not a decode failure, and invisible to a caller following the house rule of
"check `status()` at the end". It is **14** now — equalling QCBOR's cap is not
enough, because reaching smply's needs a document one level deeper than it, and
at fifteen that document is one QCBOR refuses first.

**Changed.** `kMaxCborNesting` 16 → 14. 14 invariant guards across
`os_management.cpp`, `image_management.cpp`, `upload_driver.cpp` and
`upload_session.cpp` wrapped in coverage-exclusion markers. `tools/coverage.sh`
enforces (and refuses to run without gcovr). `tools/verify_gates.sh` covers the
coverage *reporter* — the one gate it never checked, and the one that was
silently inert from P0 to P7. `tools/lint.sh` gives cppcheck include paths and
`-U` on two Catch2 option macros, retiring the `syntaxError:tests/*`
suppression. No public API changed.

**Remaining in this phase.** None.

**Soak.** ~20 minutes per target locally, two waves on four cores: 416 M
executions in total, **no findings**. Deviates from the roadmap's "≥ 2 h per
target, once" by agreement with the user — a standing `nightly-fuzz-soak` job
outlives a one-off measurement, and the CI matrix had reserved the name since
P0. The corpora were then merged (`-merge=1`), which cut 570 inputs to 476 while
keeping the named hand-seeded ones; the smoke job replays all of them in about
70 seconds.

**Caveats — read these before P14.**

* **A bound that is not the tightest bound is not a bound.** See the nesting
  finding above. When adding a limit, check what the layer *underneath* already
  enforces — and check whether the failure path is sticky, because an
  unenforced bound plus a non-sticky error is silence, not an error.
* **`LCOV_EXCL_LINE` excludes only the line it is on**, and is silently ignored
  on a comment line above the code. Marking the `if` of a guard leaves its body
  in the denominator, which is most of what the exclusion was for. Use
  `LCOV_EXCL_START` / `STOP`, and put the `STOP` *inside* the guard where the
  `else` arm is the ordinary path (`upload_driver.cpp` is the example).
* **Stale `.gcda` files survive a rebuild** and mix counts from two versions of
  the code. `libgcov profiling error: … different checksum` scrolls past in
  build output and the number that follows looks ordinary. Delete them and
  re-run the tests before measuring.
* **libFuzzer writes into the corpus directory you give it.** Both CI jobs copy
  `tests/fuzz/corpus/` out of the tree first; do the same by hand unless you
  mean to grow the committed corpus.
* **A cppcheck `syntaxError` may be a configuration it invented.** The P7 note
  blamed missing include paths; supplying them changed nothing. cppcheck
  explores Catch2's own option macros, and in
  `CATCH_CONFIG_DISABLE;CATCH_CONFIG_PREFIX_ALL` there is no `TEST_CASE` to
  parse. Before suppressing a parser complaint, print the configurations it is
  checking.
* **Three of the seven targets go through a live `SmpClient`**, because the
  decoders they exercise are file-local. That is deliberate and better — it
  fuzzes framing, correlation and decode together — but it makes those targets
  two orders of magnitude slower than the flat ones (0.86 M runs against 213 M
  in the same 20 minutes). Budget accordingly.

**The phase's own new tests tripped the oldest caveat in this file.**
`test_limits.cpp` declared the vector its callback captures *after* the fixture,
so `~SmpClient`'s `fail_all()` ran the callback over freed memory. Six other
cases in the same file had the same shape and happened not to dangle only
because their request completed first. It was invisible to five of the seven
presets and reported by both sanitizer presets — the ones the corrected
compiler-rt caveat has just made runnable locally, which is the first time this
class of bug has been caught before CI. **Declare captured state before the
fixture**, always, not only when you can see the dangling path.

**One thing that was expected and did not happen.** The P12 follow-up predicted
five of six `life.expired()` guards in `FirmwareUpdater` would be unreachable
and need exclusion markers. All six are covered: the destructor completes
outstanding work, and the tests destroy the updater mid-flight. The row is
closed as *not needed*, not as done — a prediction about coverage is not
evidence about it.

**Docs updated.** `security.md` (T3, and a new section on what the audit found),
`testing.md` (§5 rewritten against the shipped targets, with the CI jobs and the
corpus policy), `quality-gates.md` (§1 both fuzz rows live, §3 the cppcheck
change, §6 rewritten — thresholds enforced, both blocking questions settled,
measured at P13, and the traps), `architecture.md` (§9 `kMaxCborNesting`),
`limits.hpp` and `src/cbor/cbor.hpp` (the two findings, at the point of use),
`roadmap.md` (P13 Complete with outcome and five deviations, eight follow-up
rows closed, four filed, Current state), this log.

**Recommended next.** **P14 — `Dispatcher` and the portable example.** Nothing
from this phase blocks it. Worth knowing going in: the elevated per-directory
coverage gates are *measured, not enforced* (`--enforce` applies only the two
whole-core thresholds), so a directory can fall below 90 % branch with CI green
— check §6's table by hand when P14 adds `src/util/`. `Dispatcher` is the first
component with threads, so it is also the first place the "no threads in the
core" rule in `CLAUDE.md` has to be read carefully: it lives under
`include/smply/util/`, and P14's own scope says the TSan job comes with it.

### 2026-09-06 — P14a: `Dispatcher`, the client-context check and the upload-skip fix

**Status after this session:** P14a = `Complete`. **P14 was split**; next phase:
**P14b — the portable example**.

**Completed.** `include/smply/util/dispatcher.hpp` and `src/util/dispatcher.cpp`
as the separate target `smply::util`; `SMPLY_ASSERT_CLIENT_THREAD()` in
`src/detail/client_thread.hpp`, applied at eight entry points in `SmpClient`;
`UploadResult::already_present` plumbed through to
`UpdateReport::upload_skipped`; a `linux-clang-tsan` preset and CI job. 14 new
tests (**604** total), green on all nine presets. `src/util/` measures 100 %
line and 100 % branch; the whole core is unchanged at 98.4 % / 87.5 %.

**The phase was split, and that is the main thing to know.** P14 was the helper
*and* the example. The helper, its tests and the upload fix came to ~860 lines
before any documentation; the example needs a stub device, and a stub device
needs a **CBOR codec**, because smply's façade is internal (`src/cbor/`) and an
example links only the public target — roughly another 700 lines. Together that
is over twice the ~1000-line ceiling this file sets. The split is also the
useful one: **P15 needs the `Dispatcher`, not the CLI.**

**Changed.** `UploadResult` gains a public field. `CLAUDE.md` rule 7 reworded:
it said "no threads in `include/smply/` or `src/`", which this phase contradicts
literally — the real rule, in `architecture.md` §5, is that the *core* starts no
threads and ships `Dispatcher` as a separate target `libsmply` never links.
`check_docs.py`'s phase regex extended to `P\d+[a-z]?`.

**Remaining in this phase.** None.

**Caveats — read these before P14b.**

* **A pattern that stops matching does not announce itself.**
  `check_docs.py` split phases on `^##\s+(P\d+)\s*[—-]`, which does not merely
  miss `P14a` — it fails to match the heading, so R2 skips the section entirely
  and still reports a pass. Splitting the phase would have quietly disabled the
  gate for both halves. This is the third time this shape of bug has appeared
  (the P1 `verify_gates.sh` fixture, the P9 fixture builder): **when you change
  what something is named, check what was matching the old name.**
* **TSan does not always say "data race".** Removing the lock from
  `Dispatcher::post()` corrupts the allocator before the race is reported, so
  TSan says `allocation-size-too-big`. Do not read the absence of the words
  "data race" as the absence of a finding.
* **`LCOV`-style negative controls are cheap and worth it.** Both new behaviours
  were verified by breaking them and watching exactly the named assertion fail.
  The rule-9a component test failed on its own assertion with its other ten
  still passing, which is what says the test reaches the code rather than the
  setup.
* **Never run `tools/lint.sh` and `tools/verify_gates.sh` at the same time.**
  Already in the caveats from P13; it cost time again this session to remember
  why. `verify_gates.sh` points its scratch build at the real tree's
  `build/linux-clang/_deps`, and rewriting a header clang-tidy has mmapped kills
  it with a bus error that looks like a compiler crash.
* **`OsManagement` and `ImageManagement` are not pimpl'd.** That is why the
  client-context guard lives in `SmpClient` alone: a debug-only member in a
  public class definition changes its size between Debug and Release, and a
  consumer who mixes them gets an ODR violation rather than a diagnostic. Worth
  remembering for any future debug-only state.
* **The example is going to need a CBOR codec of its own.** `tests/support/`
  cannot be reused: `server_simulator.cpp` includes the private
  `image/sha256.hpp`, `smply_test_support` links `smply::smply_internal`, and
  examples build when `SMPLY_BUILD_TESTS` is `OFF`. P14b's roadmap entry
  records the way round it — answer no `match` (A6), and parse the uploaded
  image with the *public* `parse_mcuboot_header()` and `find_image_tlv_hash()`.

**Docs updated.** `api.md` (dispatcher Shipped and matching the header,
`UploadResult`, and a preamble that no longer claims any proposals remain),
`architecture.md` (§5 and the §10 layout), `design.md` (§6 rule-9a row, §8
`upload_skipped`, and a new marshalling subsection under §9),
`quality-gates.md` (§1 the TSan row, §7 TSan is real now and how it was verified
to fire), `testing.md` (§3 `Dispatcher`'s required coverage),
`dependencies.md` (`Threads::Threads`, and why `check_deps.py` does not see it),
`CLAUDE.md` (rule 7), `roadmap.md` (the split, P14a Complete with outcome and
five deviations, P14b planned, three new follow-ups), `README.md`, this log.

**Recommended next.** **P14b — the portable example.** Its roadmap entry carries
the four pieces and the sizing facts behind them. Two things worth deciding
early: the example generates a throwaway MCUboot image so it can run with no
arguments in CI (nothing in `examples/` may depend on `tests/`), and the
device's own thread is what makes `Dispatcher` and the TSan job earn their
keep — a single-threaded example would demonstrate neither.
