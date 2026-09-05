# Implementation roadmap

**This file is the project's execution state.** A new session determines what to
do next by reading it. Update it in the same change as the work
([ADR-0013](decisions/ADR-0013-living-documentation.md)).

Status values: `Planned` · `In Progress` · `Blocked` · `Complete`.

## Current state

| | |
| - | - |
| **Next phase to work on** | **P7 — OS management: reset, params, echo** |
| Last completed phase | P6 — `SmpClient`: correlation, timeouts, cancellation |
| Blocked phases | none |
| Open decisions | O1 resolved (Apache-2.0). Five remain — see [§ Open questions](#open-questions) |

## Phase summary

| ID | Title | Status | Depends on |
| -- | ----- | ------ | ---------- |
| [P0](#p0) | Project scaffolding and quality infrastructure | **Complete** | — |
| [P1](#p1) | Core types: `Result`, `Error`, `Clock`, bytes | **Complete** | P0 |
| [P2](#p2) | SMP header types and codec | **Complete** | P1 |
| [P3](#p3) | Streaming SMP message reassembly | **Complete** | P2 |
| [P4](#p4) | Transport abstraction and `FakeTransport` | **Complete** | P1 |
| [P5](#p5) | CBOR façade and MCUmgr error extraction | **Complete** | P1 |
| [P6](#p6) | `SmpClient`: correlation, timeouts, cancellation | Complete | P2–P5 |
| [P7](#p7) | OS management: reset, params, echo | Planned | P6 |
| [P8](#p8) | Image management: state read/write, erase, slot info | Planned | P6 |
| [P9](#p9) | MCUboot image file handling and SHA-256 | Planned | P1 |
| [P10](#p10) | Image upload state machine | Planned | P8, P9 |
| [P11](#p11) | `ServerSimulator` and component test harness | Planned | P7, P8, P10 |
| [P12](#p12) | `FirmwareUpdater` orchestration | Planned | P11 |
| [P13](#p13) | Fuzzing, hardening and coverage push | Planned | P10 |
| [P14](#p14) | `Dispatcher` and the portable example | Planned | P12 |
| [P15](#p15) | WinRT BLE transport | Planned | P14 |
| [P16](#p16) | WinRT BLE DFU example application | Planned | P15 |
| [P17](#p17) | Hardware interoperability suite | Planned | P16 |
| [P18](#p18) | Packaging, install/export and 1.0 review | Planned | P17 |

Phases P1–P13 are portable and can be developed and verified entirely on Linux.
P15–P17 require Windows; P17 additionally requires hardware.

---

<a id="p0"></a>
## P0 — Project scaffolding and quality infrastructure

**Status: Complete** (2026-09-04)

**Objective.** A repository that builds an empty library, runs an empty test
suite, and enforces every quality gate — so that from P1 onwards no session has
to think about infrastructure.

**Scope.** Top-level `CMakeLists.txt` with the options from
[ADR-0011](decisions/ADR-0011-build-and-dependencies.md); `CMakePresets.json`
matching the CI matrix; `cmake/warnings.cmake` and `cmake/sanitizers.cmake`;
`.clang-format`, `.clang-tidy`, `.gitignore`, `.editorconfig`; `LICENSE`;
QCBOR and Catch2 wired via `FetchContent` with exact pins; the
`smply_internal_options` and `smply::smply_internal` targets; a placeholder
`smply::smply` with one trivial header and one trivial test; GitHub Actions
workflows for the full matrix in [`quality-gates.md`](quality-gates.md) §1;
`tools/check_public_headers.py`, `tools/check_docs.py`, `tools/check_deps.py`,
`tools/format.sh`.

**Out of scope.** Any protocol code. Fuzz targets (P13 wires them; the CMake
option exists here but builds nothing). The WinRT target (P15) — but the
`SMPLY_BUILD_WINRT` option and the `core-without-winrt` CI job exist now.

**Prerequisites.** None.

**Tasks.**
1. Decide and add the licence (see open question O1) — blocks nothing else.
2. Top-level CMake, options, presets, namespaced alias targets.
3. Warning and sanitizer interface targets; verify flags reach smply's targets
   and **not** a consumer (add a `tests/consumer/` mini-project that links
   `smply::smply` with `-Wall` only and must build clean).
4. Dependency wiring with `OVERRIDE_FIND_PACKAGE` and pins; verify
   `SMPLY_USE_SYSTEM_QCBOR=ON` path.
5. Formatting, clang-tidy and cppcheck configuration + local scripts.
6. The four `tools/check_*.py` gates.
7. CI workflows for all nine matrix jobs; confirm each actually fails when it
   should (introduce a deliberate violation, watch it fail, revert).

**Files.** `CMakeLists.txt`, `CMakePresets.json`, `cmake/*`, `.clang-format`,
`.clang-tidy`, `.github/workflows/*`, `tools/*`, `LICENSE`,
`include/smply/version.hpp`, `tests/unit/CMakeLists.txt`, `tests/consumer/*`.

**Tests.** One trivial test proving Catch2 + CTest work in every matrix job.

**Docs.** `README.md` build instructions; `dependencies.md` gains the exact pins
and versions; this phase's status.

**Gates.** All of §1–§4, §10, §11 in [`quality-gates.md`](quality-gates.md)
green. Coverage and fuzz gates are configured but not yet meaningful.

**Acceptance.** `cmake --preset linux-clang && cmake --build --preset
linux-clang && ctest --preset linux-clang` succeeds on a clean clone; every CI
job is green; each gate has been observed to fail on a deliberate violation.

**Exit.** A later phase never has to touch build infrastructure to add a file.

### Outcome

**Completed.** All eight tasks. Licence resolved to Apache-2.0 (O1). Build
system, presets, both gate-carrying interface targets, placeholder library with
three passing tests, formatting and static-analysis configuration, four `tools/`
gate scripts, coverage script, CI, and `tools/verify_gates.sh` — which
demonstrates **13 gates each rejecting a deliberate violation** on a throwaway
copy of the tree.

**Remaining in this phase.** None.

**Deviations from the original plan.**

1. **6 of the 9 CI jobs specified in `quality-gates.md` §1 landed here**, plus
   three that were not in the original matrix (`linux-gcc-fallback-expected`,
   `gates`, `gate-self-check`). `windows-winrt` needs the P15 target;
   `linux-clang-fuzz-smoke` and `nightly-fuzz-soak` need P13's fuzz targets.
   Those three rows are now marked *(P13)* / *(P15)* in `quality-gates.md`.
2. **Coverage is measured but not enforced.** Thresholds against a placeholder
   library are meaningless; `tools/coverage.sh` reports, CI publishes the
   artefact, and enforcement switches on in P13 as §6 always anticipated.
3. **Clang 18, not Clang 17** — 18.1.3 is what the toolchain provides. The
   minimum supported Clang stays 14 (ADR-0001).
4. **CMake floor raised 3.24 → 3.25** for `FetchContent_Declare(SYSTEM)`. See
   the discovered-work note below; 3.25 is from 2022 and is available
   everywhere the project targets.
5. **A GCC sanitizer job was added** alongside the Clang one. GCC's and Clang's
   sanitizers do not diagnose identically, and Clang's runtime
   (`libclang-rt-*-dev`) is absent in some environments where GCC's is present.

**Discovered.** Three findings worth recording, all from making the gates
actually run:

* **Dependency headers must be `SYSTEM`.** Without it, clang-tidy attributes
  findings inside Catch2's headers to *our* test files through macro expansion:
  140 errors from two trivial `TEST_CASE`s. With `SYSTEM`, zero — and, notably,
  **no clang-tidy check had to be disabled**, which the plan had expected to be
  necessary.
* **The flag-leak guard was itself broken, and `verify_gates.sh` caught it.**
  Checking only `INTERFACE_COMPILE_OPTIONS` misses the likelier mistake:
  linking `smply_internal_options` `PUBLIC` leaves that property empty and
  leaks the flags through `INTERFACE_LINK_LIBRARIES` instead. Both are now
  checked, at configure time and by compiling `tests/consumer/`. This is
  precisely the class of silent-green failure the verification step exists to
  find.
* **Sanitizer *link* options legitimately propagate to consumers** of an
  instrumented static library and must not be treated as a leak; only *compile*
  options are held back.

---

<a id="p1"></a>
## P1 — Core types

**Status: Complete** (2026-09-04) · **Depends on:** P0

**Objective.** The vocabulary types every other phase uses.

**Scope.** `result.hpp` (`Result<T>`, `SMPLY_EXPECTED`, and
`detail/expected.hpp`), `error.hpp` (`ErrorCode`, `MgmtError`, `Error`,
`to_string`), `clock.hpp` (`Clock`, `system_clock`, `Duration`, `TimePoint`),
`bytes.hpp`, `limits.hpp` (the table in [`architecture.md`](architecture.md) §9),
and `tests/support/manual_clock.hpp`.

**Out of scope.** `std::error_code` interop. Logging.

**Tasks.** Implement the `expected` fallback to the subset in
[ADR-0002](decisions/ADR-0002-result-and-error-type.md); make `Error` a
comparable value type; ensure `to_string` never allocates for the common case;
add a build option `SMPLY_FORCE_FALLBACK_EXPECTED` so both configurations are
testable everywhere.

**Files.** `include/smply/{result,error,clock,bytes,limits}.hpp`,
`include/smply/detail/expected.hpp`, `tests/unit/test_result.cpp`,
`tests/unit/test_error.cpp`, `tests/support/manual_clock.hpp`.

**Tests.** `expected` semantics: value/error construction, move-only payloads,
`value_or`, no accidental copies, no UB on `error()` of a value state (returns a
diagnosable failure in debug). `Error` equality, `MgmtError` round-trip,
`to_string` for every `ErrorCode`. `ManualClock` monotonicity.

**Docs.** [`api.md`](api.md) §`result.hpp`/`error.hpp` reconciled with reality.

**Gates.** Build matrix (incl. the forced-fallback configuration), warnings,
clang-tidy, format, tests.

**Acceptance.** Both `expected` backings pass an identical test suite.

**Exit.** No later phase defines its own error or time type.

### Outcome

**Completed.** `bytes.hpp`, `group.hpp`, `error.hpp`, `detail/expected.hpp`,
`result.hpp`, `clock.hpp`, `limits.hpp`, `src/core.cpp`, plus
`tests/support/manual_clock.hpp` and 35 tests across `test_result.cpp`,
`test_error.cpp` and `test_clock.cpp`. All gates green; clang-tidy clean with no
check disabled beyond the one documented below.

**Remaining in this phase.** None.

**Deviations from the original plan.**

1. **`Group` moved into P1** as its own header `include/smply/group.hpp`,
   rather than living in `smp/header.hpp` (P2) as `api.md` had it. `MgmtError`
   needs it — an SMP v2 code is meaningless without the group it is scoped to —
   so the error model cannot be built without it. It is genuine core vocabulary,
   used by the error model, the group clients and the request router alike;
   `smp/header.hpp` will include it in P2. `api.md` and `architecture.md`
   updated. No ADR contradicted, so none superseded.
2. **A C++23 CI job was added** (`linux-gcc-cxx23-std-expected`). This is what
   makes the phase's own acceptance criterion testable: under the C++20 baseline
   `std::expected` does not exist, so `SMPLY_FORCE_FALLBACK_EXPECTED` alone
   tests the same backing twice and the standard path is never exercised.
   C++20 remains the baseline (ADR-0001) — `SMPLY_CXX_STANDARD` merely makes 23
   buildable.
3. **`performance-enum-size` disabled** in `.clang-tidy`, with the reason
   recorded in `quality-gates.md` §3: underlying types here follow the wire
   format, and `Group` must be `uint16_t`.
4. **`src/core.cpp`** holds the out-of-line definitions for `system_clock`,
   `group_name` and both `to_string` overloads, rather than one file per header.
   Splitting three small functions across three translation units would be
   noise; revisit if it grows.

**Discovered.** Three findings, two of them defects in work from this phase and
P0 that the gates caught:

* **The hand-written `expected` had an exception-safety hole**, surfaced by
  clang-tidy's analyser. Changing which union member is active means destroying
  one object and constructing another; if that construction throws, the object
  holds neither and the destructor then destroys something that was never built.
  It is now backed by `std::variant` with both alternatives required to be
  nothrow-move-constructible, so the valueless state is unreachable and there is
  no hand-managed lifetime at all. That also cleared every analyser finding —
  they were all rooted in the union, not false positives to be silenced.
* **`check_docs.py` R4 had two real bugs**, both found by
  `tools/verify_gates.sh` rather than by review. Its `detail`-namespace
  exemption compared brace depth against zero, which — since the enclosing
  `namespace smply` never closes until EOF — exempted *everything* after the
  first `detail` block; and it did not match the nested `namespace smply::detail`
  form at all. Separately, hardening it to look past `template<...>` lines made
  it skip any line starting with `[[`, swallowing complete declarations like
  `[[nodiscard]] const char* version() noexcept;`. All three are fixed and the
  gate self-check passes 13/13 again.
* **YAML folded scalars have no comments.** A `#` inside `.clang-tidy`'s
  `Checks:` block becomes part of the check list rather than a comment, which
  silently broke a suppression. Noted in `quality-gates.md` §3.

---

<a id="p2"></a>
## P2 — SMP header types and codec

**Status: Complete** (2026-09-04) · **Depends on:** P1

**Objective.** Encode and decode the 8-byte SMP header, correctly and
defensively.

**Scope.** `smp/header.hpp` (`Operation`, `Version`, `Group`, `Header`,
`kHeaderSize`), `src/smp/codec.cpp`.

**Out of scope.** CBOR, reassembly, anything stateful.

**Tasks.** Implement per [`protocol-notes.md`](protocol-notes.md) §2: byte-0
bit packing, big-endian fields, reserved-bit and version validation, unknown
groups round-tripping, `flags` preserved rather than rejected.

**Files.** `include/smply/smp/header.hpp`, `src/smp/codec.cpp`,
`tests/unit/test_smp_codec.cpp`, `tests/support/message_builder.hpp`.

**Tests.** Golden vectors (hand-computed from the spec table) for at least
`(Read, V1, Os, 0)`, `(WriteResponse, V2, Image, 1)` and a maximal header;
round-trip over generated combinations; reserved bits set ⇒ `MalformedMessage`;
version `0b10`/`0b11` ⇒ `UnsupportedSmpVersion`; explicit assertion that
`length`/`group` are big-endian on a little-endian host.

**Docs.** [`api.md`](api.md) `smp/header.hpp`; any spec ambiguity found →
[`protocol-notes.md`](protocol-notes.md).

**Gates.** All P1 gates.

**Acceptance.** Golden vectors pass byte-for-byte; no `reinterpret_cast` in the
implementation.

**Exit.** No other file in the repository manipulates header bytes.

### Outcome

**Completed.** `include/smply/smp/header.hpp`, `src/smp/codec.cpp`,
`tests/support/message_builder.hpp` and `tests/unit/test_smp_codec.cpp`
(18 new cases, 53 total). Five hand-computed golden vectors, round-trip over a
generated field space, and rejection cases for reserved bits, reserved versions,
undefined operations and short buffers. No `reinterpret_cast`; byte order is
explicit shifts throughout, so the encoding does not depend on the host's.

**Remaining in this phase.** None.

**Deviations from the original plan.**

1. **`Group` was already delivered in P1**, so this phase's scope line naming it
   was satisfied on arrival; `smp/header.hpp` includes `smply/group.hpp`.
2. **Two small additions beyond the stated scope**: `Header::total_size()`,
   which puts the `kHeaderSize + length` message-boundary rule in one place
   rather than leaving P3 to open-code it, and a dynamic-span `decode_header`
   overload, which is the form the reassembler will actually call. Also
   `operation_name()` for diagnostics, matching `group_name()` from P1.
3. **Response-operation mapping was deliberately left out.** `Read -> ReadResponse`
   is correlation policy, not header encoding; it belongs with the
   pending-request table in P6.

**Discovered.**

* **`clang-analyzer-optin.core.EnumCastOutOfRange` is unusable for this
  codebase** and is now disabled with the reason recorded in
  `quality-gates.md` §3. It assumes an enumeration's valid values are exactly
  its enumerators, which is false for a wire-format enumeration: `Group` is open
  by design, so decoding one is a `static_cast` from an arbitrary 16-bit value
  by construction. It had already needed per-site suppression in P1's tests and
  fired again in two more places here; a third round of NOLINTs would have been
  the wrong answer. The per-site suppressions it forced in `test_error.cpp` were
  removed.

---

<a id="p3"></a>
## P3 — Streaming SMP message reassembly

**Status: Complete** (2026-09-04) · **Depends on:** P2

**Objective.** Turn an arbitrary byte stream into complete SMP messages, within
hard bounds.

**Scope.** `src/smp/assembler.{hpp,cpp}` (internal), per
[`design.md`](design.md) §2.

**Out of scope.** Transport (P4), correlation (P6).

**Tasks.** Implement the loop; the compacting buffer with a read cursor; the
two bounds (`max_smp_payload`, `max_assembly_bytes`); `reset()`.

**Files.** `src/smp/assembler.{hpp,cpp}`, `tests/unit/test_assembler.cpp`.

**Tests.** The **fragmentation invariant** — identical output for whole,
byte-at-a-time, every fixed fragment size 1–64, and seeded random cuts. Multiple
messages in one `feed()`. Truncated tail buffered then completed. Oversized
`length` ⇒ `MessageTooLarge` with no allocation growth (assert on
`buffered()` and on a counting allocator). `reset()` discards partial state.

**Docs.** [`design.md`](design.md) §2 reconciled.

**Gates.** All P2 gates + ASan/UBSan.

**Acceptance.** The invariant test passes for every fragmentation pattern; peak
buffer never exceeds the configured bound under adversarial input.

**Exit.** The only reassembly implementation in the repository.

### Outcome

**Completed.** `src/smp/assembler.{hpp,cpp}` and
`tests/unit/test_assembler.cpp` (19 new cases, 72 total, all passing under
ASan/UBSan). The fragmentation invariant is checked against whole delivery,
byte-at-a-time, every fixed fragment size 1–64, oversized fragments and eight
seeded random cut patterns.

**Remaining in this phase.** None.

**Deviations from the original plan.**

1. **No read cursor and no compaction.** The planned design was one growing
   buffer with a cursor, compacted past the halfway mark. The implementation
   buffers **at most one message** and parses directly out of the caller's
   bytes when nothing is held. This is simpler *and* strictly better bounded:
   with append-then-parse, a caller passing a megabyte in one `feed()` would
   buffer the whole megabyte before anything checked it, whereas here nothing is
   appended beyond a `total_size()` that was validated first. The fast path is
   also zero-copy, which the cursor design could not be. `design.md` §2 is
   rewritten to describe what exists, with the reasoning.
2. **The `max_buffer` check runs before waiting**, not after the bytes arrive.
   Checking afterwards leaves the stream stalled forever waiting for data that
   would be refused on arrival. There is a test named for this.
3. **`capacity()` instead of a counting allocator.** The plan called for a
   counting allocator to prove no allocation growth. Exposing `capacity()` (and
   `peak_buffered()`) tests the same property more directly and with far less
   machinery: the assertion is that a hostile declared length leaves capacity
   untouched, which is the thing actually worth guaranteeing.
4. **A re-entrancy guard was added.** Not in the plan, but a sink calling
   `feed()` from inside `on_message()` would mutate the buffer its own payload
   points into. Three lines turn a use-after-free into `InvalidState`.

**Discovered.**

* **Internal headers needed an include path.** `src/` was on the include path
  for tests (via `smply::smply_internal`) but not for smply's own sources, so
  `#include "smp/assembler.hpp"` did not compile. Added as a `PRIVATE` include
  directory on the `smply` target — consumers still cannot see it, and the
  public-header gate still keeps internal headers out of `include/`.
* Four clang-tidy findings on new code, all real and all fixed: an implicit
  widening multiplication in `limits.hpp` (`16U * 1024U` computed in `unsigned`
  then widened), a non-const RAII guard, a parameter name that disagreed between
  declaration and definition, and unnamed parameters in a test sink.

---

<a id="p4"></a>
## P4 — Transport abstraction and `FakeTransport`

**Status: Complete** (2026-09-05) · **Depends on:** P1

**Objective.** Freeze the transport contract and provide the test double every
later phase depends on.

**Scope.** `include/smply/transport.hpp`; `tests/support/fake_transport.{hpp,cpp}`
with the full injection and fault API in [`testing.md`](testing.md) §2.

**Out of scope.** Any real transport.

**Tasks.** Implement the interfaces; write the normative contract into
[`design.md`](design.md) §9 as the header's documentation; build `FakeTransport`
including `deliver_split_at`, `set_busy`, `raise_transport_error`, `disconnect`.

**Files.** `include/smply/transport.hpp`, `tests/support/fake_transport.*`,
`tests/unit/test_fake_transport.cpp`.

**Tests.** `FakeTransport` itself: records whole messages; each delivery mode
produces the expected `on_bytes` call pattern; `close()` prevents further
callbacks; borrowed-buffer lifetime is respected (verified under ASan by
delivering from a temporary).

**Docs.** [`api.md`](api.md) transport section; [`design.md`](design.md) §9.

**Gates.** All P3 gates.

**Acceptance.** The test double can express every scenario listed in
[`testing.md`](testing.md) §2 — verified by writing one test per scenario, even
if trivial.

**Exit.** The contract is stable; changing it later requires superseding
[ADR-0005](decisions/ADR-0005-transport-abstraction.md).

### Outcome

**Completed.** `include/smply/transport.hpp` with the normative contract written
into the header itself, `tests/support/fake_transport.{hpp,cpp}`, and
`tests/unit/test_fake_transport.cpp` (27 new cases, 99 total). Every scenario in
[`testing.md`](testing.md) §2 has a test, including the ones that need no API —
"no response" is not calling `deliver()`, and malformed input is an ordinary
byte buffer.

**Remaining in this phase.** None.

**Deviations from the original plan.**

1. **`FakeTransport` enforces the contract, it does not merely implement it.**
   Delivering after `disconnect()` or `close()` is suppressed and counted via
   `suppressed_deliveries()`. A double more permissive than the real interface
   would let tests pass by depending on callbacks a real transport is forbidden
   to make — which is worse than having no double, because it fails only on
   hardware.
2. **A `smply_test_support` static library** was created rather than adding the
   double to the unit-test executable's source list. P11's `ServerSimulator`
   and P12's component tests build a second executable against the same doubles,
   so the target is needed anyway and costs nothing now.
3. **A composition test was added** (`FakeTransport` → `MessageAssembler`),
   which is beyond the phase's stated scope. The two are the halves of the
   inbound path and everything from P6 depends on them fitting together;
   proving it here is far cheaper than discovering a mismatch inside the client.
4. **Extra inspection accessors** beyond the sketch: `send_count()`,
   `last_sent()`, `clear_sent()`, `on_bytes_calls()`, `connected()`.
   `clear_sent()` in particular lets a later test assert on the commands issued
   by one phase of a workflow without counting everything before it.

**Discovered.** One clang-tidy finding, fixed:
`std::exchange`-ing an engaged `std::optional` defeats
`bugprone-unchecked-optional-access`; the explicit
check-move-reset form is both clearer and analysable.

---

<a id="p5"></a>
## P5 — CBOR façade and MCUmgr error extraction

**Status: Complete** (2026-09-05) · **Depends on:** P1

**Objective.** Bounded, non-allocating CBOR encode/decode, with QCBOR hidden.

**Scope.** `src/cbor/{reader,writer,backend_qcbor,mgmt_error}.*` per
[`design.md`](design.md) §3.

**Out of scope.** Group-specific shapes (P7, P8).

**Tasks.** Implement `Writer` over a caller-owned buffer with a sticky error;
`Reader` with map-key getters returning `std::optional`, array iteration by
callback, bounded nesting; `extract_mgmt_error()` handling v1 flat `rc`, v2
`err:{group,rc}`, and v2-with-flat-`rc` ([`protocol-notes.md`](protocol-notes.md) §3).

**Files.** `src/cbor/*`, `tests/unit/test_cbor_reader.cpp`,
`tests/unit/test_cbor_writer.cpp`, `tests/unit/test_mgmt_error.cpp`.

**Tests.** Round-trips; absent key ⇒ `nullopt` not error; wrong type ⇒ sticky
error not a wrong value; over-nesting ⇒ error; **every prefix** of a valid
encoding decodes as an error and never crashes; all four `MgmtError` shapes;
writer buffer exhaustion ⇒ error from `finish()`.

**Docs.** [`design.md`](design.md) §3;
[`dependencies.md`](dependencies.md) pin confirmed.

**Gates.** All P4 gates. `check_public_headers.py` must confirm QCBOR is absent
from `include/`.

**Acceptance.** No QCBOR symbol reachable from a public header; no allocation
attributable to device-supplied sizes.

**Exit.** All later CBOR work uses the façade only.

### Outcome

**Completed.** `src/cbor/{cbor.hpp,reader.cpp,writer.cpp,mgmt_error.*}` and 50
new tests (149 total). QCBOR appears in exactly three files and no public
header; the API-discipline gate confirms it.

**Remaining in this phase.** None.

**ADR-0007 validated.** The decision was taken on QCBOR's documented API rather
than hands-on use, and said to supersede it in favour of TinyCBOR if the
assumption failed. It held: the spiffy-decode map-key getters, the sticky error
model, and — most importantly — a distinguishable `QCBOR_ERR_LABEL_NOT_FOUND`
are exactly what "absent is not an error" needs. The ADR's Status line records
the validation.

**Deviations from the original plan.**

1. **No separate `backend_qcbor.*` file.** The plan listed one, but `reader.cpp`
   and `writer.cpp` *are* the backend, and a third file would only have held
   includes. The seam that matters — no QCBOR in any public header, one
   replaceable pair of translation units — is intact.
2. **Named `put_uint`/`put_int`/… rather than an overloaded `value()`.** CBOR
   distinguishes unsigned from negative integers on the wire and MCUmgr fields
   have specific types, so overload resolution picking for us could silently
   emit a different encoding than the protocol asks for. Worth the extra
   verbosity at call sites.
3. **`for_each_map_in_array` takes a mandatory element cap** rather than relying
   on a configured default. The caller knows what it can hold; making the bound
   explicit at the call site means it cannot be forgotten.
4. **`smply::smply_internal` now propagates QCBOR's include path.** Internal
   headers under `src/cbor/` name QCBOR, so testing them needs its headers.
   `smply::smply` still keeps QCBOR `PRIVATE`, which is what the acceptance
   criterion and the gate care about.

**Discovered.**

* **Nesting is bounded twice, and QCBOR's bound is the binding one.** QCBOR
  enforces a compile-time `QCBOR_MAX_ARRAY_NESTING` of 15, below
  `limits::kMaxCborNesting` (16), so the façade's own counter never fires first
  on hostile input. Deep input is rejected either way, so this is documented
  rather than "fixed" — but the configured limit should not be read as the
  operative one.
* **`bugprone-unchecked-optional-access` found a real readability problem** in
  `reader.cpp`: the failure path stored an error and then dereferenced
  `error_` separately, relying on a postcondition a reader had to take on
  trust. Replaced with a `record()` helper that stores *and returns* the
  failure, which is clearer and analysable. The same check produces only false
  positives in test code, where `REQUIRE(x.has_value())` is opaque to it, so it
  is disabled for `tests/` alone via `tests/.clang-tidy` — with the reason
  written there.

---

<a id="p6"></a>
## P6 — `SmpClient`

**Status: Complete** · **Depends on:** P2, P3, P4, P5

**Objective.** The request lifecycle: sequence allocation, correlation,
timeouts, cancellation, disconnection, statistics.

**Scope.** `include/smply/smp_client.hpp`, `src/smp/client.cpp`, per
[`design.md`](design.md) §4 and
[ADR-0010](decisions/ADR-0010-request-correlation.md).

**Out of scope.** Any management group.

**Tasks.** Pending-request table (bounded by `max_in_flight`); generation-tagged
`RequestHandle`; retired-sequence ring; sequence allocator skipping pending and
retired; `poll()`/`next_deadline()`; `rebind_transport()`; re-entrancy-safe
completion; `Stats`; destructor cancelling pending requests.

**Files.** `include/smply/smp_client.hpp`, `src/smp/client.cpp`,
`tests/unit/test_smp_client.cpp`.

**Tests.** The full list in [`testing.md`](testing.md) §3 "SmpClient" —
correlation tuple, wrong seq/group/command dropped, duplicate dropped, late
response after timeout not attributed to a reused seq, timeout at the exact
deadline under `ManualClock`, cancel-once semantics, stale handle is inert,
disconnect fails all pending, destruction with pending requests, `TransportBusy`
surfaced, callback-starts-request re-entrancy.

**Docs.** [`api.md`](api.md) `smp_client.hpp`; [`design.md`](design.md) §4.

**Gates.** All P5 gates + ASan/UBSan.

**Acceptance.** Every listed test passes; branch coverage of `src/smp/` ≥ 90 %.

**Exit.** Groups can be written without touching correlation or timing.

### Outcome

**Completed.** `include/smply/smp_client.hpp`, `src/smp/client.cpp` and 55 new
tests (204 total). Branch coverage of `src/smp/` is 96.2 % against the ≥ 90 %
acceptance criterion, and line coverage 98.2 %, measured with gcovr excluding
throw branches — see the metric caveat in Discovered below.

**Remaining in this phase.** None.

**Deviations from the original plan.**

1. **Callbacks are deferred, not immediate.** The plan (and `design.md` §4 as
   written) had `cancel()` complete a request "immediately" and `request()`
   report a rejection by returning. Both now queue the callback for the next
   `poll()`. The reason is the invariant every layer above depends on:
   `handle = client.request(...)` must have assigned before any callback can
   observe the handle. Completing inline makes the assignment happen *after* the
   callback that wants to read it. `design.md` §4 was rewritten to match.
   The destructor is the one exception — it has no later `poll()` — and says so.
2. **`Stats` is a free-standing `SmpClientStats` returned by const reference**,
   not a nested type returned by value, and carries three counters the plan did
   not name: `late`, `mismatched` and `cancelled`. Each exists because a test
   needs to distinguish *deliberately dropped* from *silently mishandled*, and
   "unmatched" alone conflates three different reasons for dropping a message.
3. **`SmpClient` inherits `TransportListener` privately.** Only a transport
   should be able to feed the client bytes; nothing above it has any business
   synthesising `on_bytes()`.
4. **A mismatched response leaves the request pending.** `design.md` §4 already
   argued for this, but the text still carried the rejected alternative
   mid-sentence ("complete with `UnexpectedResponse`… **no**:"). Rewritten as a
   decision rather than a visible deliberation.

**Discovered.**

* **A dead bound, found by chasing coverage rather than assuming it.** P6's
  ≥ 90 % criterion was initially missed at 88.1 %. Locating the uncovered
  branches turned up a genuine defect: `request()` checked the payload against
  both `max_smp_payload` and the largest encodable length, and the second test
  could never fire, because `max_smp_payload` is a `uint16_t`. Removed, and
  replaced with a `static_assert` that will fail if the field is ever widened.
  Nine further tests took `src/smp/` to 95 %. Had the criterion been waived, the
  dead branch would have survived.
* **Branch coverage is not one number.** For the identical `src/smp/` object
  files, gcovr reports 76.6 % counting throw branches and 96.2 % with
  `--exclude-throw-branches`: a ~20-point swing from metric definition alone,
  because every potentially-throwing call contributes two branches that a test
  suite with no exceptions can never take. [`quality-gates.md`](quality-gates.md)
  §6 states a threshold without saying which metric it means. That must be
  pinned down before P13 turns enforcement on, or the gate will mean whichever
  tool CI happens to run.
* **`src/cbor/` is at 80.9 % branch coverage**, below the ≥ 90 % elevated gate
  `quality-gates.md` §6 lists for it. Not P6's code and not yet enforced, but it
  will block P13 as things stand.

* **A transport must outlive every client bound to it, and nothing said so.**
  `~SmpClient` and `rebind_transport()` both call `Transport::set_listener()` on
  a transport they are letting go of. Three rebind tests declared the
  replacement transport *after* the fixture, so it was destroyed first; GCC
  turned that into `pure virtual method called` while Clang silently did not.
  A latent use-after-free, found only because the phase's gates run both
  compilers. The requirement is now stated on `SmpClient` and on
  `rebind_transport()`, and the tests declare their transports first. The
  transport contract in `transport.hpp` describes the *listener* outliving the
  transport but not the reverse; see the follow-up item below.

---

<a id="p7"></a>
## P7 — OS management group

**Status: Planned** · **Depends on:** P6

**Objective.** Reset, MCUmgr parameters, echo.

**Scope.** `include/smply/groups/os.hpp`, `src/groups/os/*`.

**Out of scope.** DFU policy around reset (P12); `boot_mode`.

**Tasks.** Encode/decode per [`protocol-notes.md`](protocol-notes.md) §5,
including `force`; `mcumgr_parameters` with `ENOTSUP` surfaced as an ordinary
`ProtocolError` for the caller to treat as non-fatal; echo (small, and it is the
easiest end-to-end smoke test against real hardware).

**Files.** `include/smply/groups/os.hpp`, `src/groups/os/os_management.cpp`,
`tests/unit/test_os_group.cpp`.

**Tests.** Request encodings byte-for-byte; `buf_size`/`buf_count` decode;
`ENOTSUP` path; reset with and without `force`; malformed responses bounded.

**Docs.** [`api.md`](api.md) `groups/os.hpp`.

**Gates.** All P6 gates.

**Acceptance.** Encodings match hand-computed vectors from the spec.

---

<a id="p8"></a>
## P8 — Image management: state, erase, slot info

**Status: Planned** · **Depends on:** P6

**Objective.** Everything in group 1 except upload.

**Scope.** `include/smply/groups/image.hpp` (types + non-upload methods),
`src/groups/image/image_management.cpp`.

**Out of scope.** Upload (P10).

**Tasks.** `ImageState`/`ImageSlot`/`SetStateRequest`/`SlotInfo` decoding per
[`protocol-notes.md`](protocol-notes.md) §6 with the "absent means false" and
"absent image ⇒ 0" rules; helper accessors (`active_slot`, `find_by_hash`,
`secondary`); element and string caps; `ImageVersion::parse`.

**Files.** as above, `tests/unit/test_image_group.cpp`.

**Tests.** Golden responses from the spec examples; single-image response
without `"image"`; empty `images` array (valid, not an error); all-flags-absent
entry; hostile responses (huge array, oversized hash, oversized version string)
⇒ bounded error; `set_state` encoding with and without `hash`; erase with the
long timeout.

**Docs.** [`api.md`](api.md) `groups/image.hpp`.

**Gates.** All P7 gates.

**Acceptance.** Every optional field's absence is handled per spec, with a test
naming the rule.

---

<a id="p9"></a>
## P9 — MCUboot image file handling and SHA-256

**Status: Planned** · **Depends on:** P1

**Objective.** Read the firmware file safely, and produce the two hashes the
protocol needs.

**Scope.** `include/smply/image_source.hpp`, `src/image/{mcuboot_header,tlv,
sha256}.*`, per [`design.md`](design.md) §7 and
[ADR-0009](decisions/ADR-0009-mcuboot-boundary.md).

**Out of scope.** Signature verification, decryption, file I/O beyond
`MemoryImageSource` (the application provides its own source).

**Tasks.** Field-by-field header decode (no struct cast); vendored SHA-256 with
NIST vectors; streaming `sha256(ImageSource&)`; hardened TLV scan (bounded
`it_tlv_tot`, strictly-positive advance, iteration cap); `encrypted` flag.

**Files.** as above, `tests/unit/test_mcuboot_header.cpp`,
`tests/unit/test_sha256.cpp`, `tests/unit/test_tlv.cpp`, `tests/data/*`.

**Tests.** Golden headers; wrong magic; truncated; absurd sizes. NIST SHA-256
vectors plus a large streaming case exercised at every buffer boundary. TLV:
valid protected + unprotected, `it_tlv_tot` beyond EOF, zero-length TLV
(termination), missing `IMAGE_TLV_SHA256`, encrypted image short-circuit.

**Docs.** [`api.md`](api.md) `image_source.hpp`; the two-hash distinction
restated where it is used.

**Gates.** All P8 gates + ASan/UBSan.

**Acceptance.** SHA-256 matches NIST vectors; the TLV scanner provably
terminates on arbitrary input (fuzzed in P13).

---

<a id="p10"></a>
## P10 — Image upload state machine

**Status: Planned** · **Depends on:** P8, P9

**Objective.** Correct, resumable, bounded image upload — the protocol core of
the product.

**Scope.** `src/groups/image/upload_session.*` (pure functions) and
`ImageManagement::upload`/`resume`/`UploadHandle`, per
[`design.md`](design.md) §6 and
[ADR-0008](decisions/ADR-0008-upload-state-ownership.md).

**Out of scope.** Reset/reconnect orchestration (P12).

**Tasks.** Chunk sizing including exact first-packet CBOR overhead and the
32-byte floor; first-packet reconstruction whenever `off == 0` or
`first_packet_pending`; the full response table (authoritative `rsp.off`,
restart on zero, rewind/no-progress budget, overrun, missing `off`, `match`);
timeout retransmission of the identical request; bounded retries and restarts;
progress from `confirmed_off` only; cancellation.

**Files.** as above, `tests/unit/test_upload_session.cpp`,
`tests/unit/test_upload_driver.cpp`.

**Tests.** The full table-driven list in [`testing.md`](testing.md) §3
"UploadSession" — 15+ named cases, each traceable to a rule in
[`protocol-notes.md`](protocol-notes.md) §6. Plus driver-level tests over
`FakeTransport`: retransmission after timeout sends byte-identical bytes;
cancellation completes exactly once.

**Docs.** [`design.md`](design.md) §6 reconciled; any newly discovered server
behaviour → [`protocol-notes.md`](protocol-notes.md).

**Gates.** All P9 gates. **Branch coverage of `upload_session.*` ≥ 90 %.**

**Acceptance.** Every row of the response table has a named test. No code path
computes `next_off` arithmetically.

**Exit.** Upload works standalone (`UploadOnly`) before any DFU orchestration
exists.

---

<a id="p11"></a>
## P11 — `ServerSimulator` and the component harness

**Status: Planned** · **Depends on:** P7, P8, P10

**Objective.** A deterministic in-memory MCUmgr device, so the DFU state machine
can be developed against realistic behaviour without hardware.

**Scope.** `tests/support/server_simulator.*` with the `ServerConfig` in
[`testing.md`](testing.md) §2; `tests/component/` scaffolding.

**Out of scope.** `FirmwareUpdater` itself (P12).

**Tasks.** Implement groups 0 and 1 to the letter of
[`protocol-notes.md`](protocol-notes.md), including offset correction, resume by
`sha`, forced restart, `match` on the final chunk, reset-then-drop, and a
simulated reboot that performs an MCUboot-like swap and updates slot state
(including the revert-if-unconfirmed rule). Support v1/v2, missing optional
commands, single-image mode and response delays.

**Files.** `tests/support/server_simulator.*`,
`tests/component/test_round_trip.cpp`, `tests/component/CMakeLists.txt`.

**Tests.** The simulator itself: an upload driven by `ImageManagement` completes
and the simulated flash content equals the source byte-for-byte; each configured
deviation (v2, no params, no slot info, no image check) behaves as specified;
the simulated reboot/revert rules match [`protocol-notes.md`](protocol-notes.md) §7.

**Docs.** [`testing.md`](testing.md) §2 reconciled with the real `ServerConfig`.

**Gates.** All P10 gates.

**Acceptance.** A full upload through the real client stack into the simulator
reproduces the source image exactly, in both SMP versions.

---

<a id="p12"></a>
## P12 — `FirmwareUpdater` orchestration

**Status: Planned** · **Depends on:** P11

**Objective.** The end-to-end update, including the reset/reconnect protocol
with the application.

**Scope.** `include/smply/dfu/firmware_updater.hpp`,
`src/dfu/update_state_machine.*` (pure), `src/dfu/firmware_updater.cpp`, per
[`design.md`](design.md) §8.

**Out of scope.** Any connection management.

**Tasks.** The pure `(state, event) → (state, effects)` machine covering every
state and every row of the failure/recovery table; the three `UpdateMode`s; the
event stream (`DisconnectExpected`, `ReconnectRequired`, `Progress`,
`StateChanged`, `Finished`); `resume_after_reconnect()` and
`reconnect_failed()`; `UpdateReport`; `RolledBack` detection.

**Files.** as above, `tests/unit/test_update_state_machine.cpp`,
`tests/component/test_firmware_update.cpp`.

**Tests.** Unit: every transition and every recovery row, driven directly.
Component (over the simulator): every scenario in
[`testing.md`](testing.md) §4 — clean update in all three modes, disconnect at
each 10 % boundary then resume, forced restart, already-present, already-active,
invalid image, lost reset response, `EBUSY` then `force`, rollback observed,
confirmation denied, cancellation in every non-terminal state, and all of it
repeated with `ServerConfig::version = V2` and with params unsupported.

**Docs.** [`design.md`](design.md) §8 diagram and table reconciled;
[`api.md`](api.md) DFU section; [`architecture.md`](architecture.md) if
component boundaries moved.

**Gates.** All P11 gates. **Branch coverage of `src/dfu/` ≥ 90 %.**

**Acceptance.** All component scenarios pass with no wall-clock dependence.

**Exit.** The portable product is functionally complete; everything after this
is hardening, platform work and packaging.

---

<a id="p13"></a>
## P13 — Fuzzing, hardening and coverage

**Status: Planned** · **Depends on:** P10 (may run in parallel with P12)

**Objective.** Prove the untrusted-input surface is safe, and close coverage
gaps.

**Scope.** All seven fuzz targets in [`testing.md`](testing.md) §5 with seeded
corpora; the ASan/UBSan and fuzz-smoke CI jobs made blocking; a review of every
robustness rule in [`design.md`](design.md) §11 against the actual code; the
coverage gates turned on.

**Out of scope.** New features.

**Tasks.** Write the targets and corpora; run a real soak (≥ 2 h per target
locally) before declaring the phase done; fix every finding and commit its
reproducer; audit each limit in [`architecture.md`](architecture.md) §9 for an
actual enforcement site and a test; enable the coverage thresholds in CI.

**Files.** `tests/fuzz/*`, `tests/fuzz/corpus/*`, CI workflow updates.

**Tests.** The fuzz targets, plus a unit test per limit asserting the bound is
enforced (not merely documented).

**Docs.** [`security.md`](security.md) updated with anything the fuzzers
revealed; [`quality-gates.md`](quality-gates.md) coverage numbers made real.

**Gates.** Everything, with coverage and fuzz-smoke now blocking.

**Acceptance.** A 2-hour soak per target with no findings; every configured
limit has an enforcement test.

---

<a id="p14"></a>
## P14 — `Dispatcher` and the portable example

**Status: Planned** · **Depends on:** P12

**Objective.** Give adapter authors the marshalling helper, and prove the pump
model with a runnable program on Linux.

**Scope.** `include/smply/util/dispatcher.hpp`, `src/util/dispatcher.cpp`,
`examples/cli_dfu/` (a console DFU driver over a stub/loopback transport).

**Out of scope.** WinRT.

**Tasks.** Implement `Dispatcher` (mutex + queue + wake callback, MPSC,
`drain()` on the client context only); write the example as the canonical pump
loop shown in [`api.md`](api.md).

**Files.** as above, `tests/unit/test_dispatcher.cpp`, CI job for TSan on the
dispatcher test.

**Tests.** Multi-producer posting under TSan; `drain()` returns the count and
runs each closure once; `clear()` drops without running; a closure posted from
inside `drain()` runs on the next drain, not recursively.

**Docs.** [`api.md`](api.md) dispatcher section;
[`architecture.md`](architecture.md) §5 reference to it.

**Gates.** All P13 gates + the new TSan job.

**Acceptance.** The example performs a complete simulated update end to end.

---

<a id="p15"></a>
## P15 — WinRT BLE transport

**Status: Planned** · **Depends on:** P14 · **Requires Windows**

**Objective.** A reference `Transport` over C++/WinRT GATT.

**Scope.** `transports/winrt_ble/` → target `smply::winrt_ble`, per
[`design.md`](design.md) §10.

**Out of scope.** Scanning/pairing UI, connection policy (the example's job).

**Tasks.** Service and characteristic discovery by the UUIDs in
[`protocol-notes.md`](protocol-notes.md) §8; CCCD write + `ValueChanged`
subscription; fragmenting `send()` by `MaxPduSize − 3` using write-without-
response; `max_message_size()` as a configured cap; inbound copy → `Dispatcher`
→ `on_bytes()`; `ConnectionStatusChanged` → `on_disconnected()`;
`event_revoker`-based lifetime; a synchronous, idempotent `close()` after which
no callback can fire; MTU change handling.

**Files.** `transports/winrt_ble/*`, `tests/unit/test_winrt_ble_framing.cpp`
(the fragmentation maths is testable without a radio).

**Tests.** Pure-logic tests for fragment splitting at various MTUs (including
the minimum 23-byte ATT MTU) and for the `close()` state machine. Anything
requiring a radio is deferred to P17.

**Docs.** [`design.md`](design.md) §10 reconciled; adapter README covering
threading and shutdown obligations.

**Gates.** `windows-winrt` **and** `core-without-winrt` both green;
`check_public_headers.py` confirms no WinRT type in `include/smply/`.

**Acceptance.** The core still builds with `SMPLY_BUILD_WINRT=OFF` on all three
platforms; no `winrt::` symbol is reachable from a public header.

---

<a id="p16"></a>
## P16 — WinRT BLE DFU example application

**Status: Planned** · **Depends on:** P15 · **Requires Windows**

**Objective.** A usable console tool that performs a real update, and the
reference for how an application drives smply across a reboot.

**Scope.** `examples/winrt_ble_dfu/`.

**Out of scope.** A GUI.

**Tasks.** Scan/connect by name or address; build the client stack; the pump
loop; progress output; handle `ReconnectRequired` by reconnecting with backoff,
calling `rebind_transport()` then `resume_after_reconnect()`; clear exit codes
and error reporting.

**Files.** `examples/winrt_ble_dfu/*`, its README.

**Tests.** Manual, documented in the README; automated coverage arrives with P17.

**Docs.** README with usage; [`architecture.md`](architecture.md) §1 diagram
confirmed against the real example.

**Gates.** `windows-winrt` builds the example; format, warnings, clang-tidy.

**Acceptance.** The tool completes an update against a real device (recorded in
the P17 log if hardware is not yet available at this point).

---

<a id="p17"></a>
## P17 — Hardware interoperability suite

**Status: Planned** · **Depends on:** P16 · **Requires hardware**

**Objective.** Prove interoperability with a real Zephyr + MCUboot device, and
cross-check against established tooling.

**Scope.** `tests/hil/` (target `smply_hil`, `SMPLY_BUILD_HIL=ON`), the device
setup documentation, and the cross-check procedure in
[`testing.md`](testing.md) §6.

**Out of scope.** The PR gate — HIL never blocks a PR.

**Tasks.** Document the exact board, Zephyr revision, `smp_svr` configuration
and Kconfig snapshot; implement the case list; run the same updates with
Zephyr's supported `mcumgr-client` and compare image-state output and captured
HCI traces; record every divergence.

**Files.** `tests/hil/*`, `tests/hil/README.md`.

**Tests.** Clean update · interrupted upload then resume · resume after an
application restart · already present · corrupted image rejected · test boot
then confirm · test boot then reset without confirm ⇒ **rollback observed** ·
reset · erase · optional-command presence/absence.

**Docs.** **Every divergence from expectation becomes an entry in
[`protocol-notes.md`](protocol-notes.md) §9**, and if it changes behaviour, an
ADR. This is the phase most likely to invalidate an assumption — treat
assumption updates as the primary deliverable, not a side effect.

**Gates.** Nightly self-hosted job, advisory; failures open issues.

**Acceptance.** All cases pass against real hardware; smply and `mcumgr-client`
produce equivalent device state after the same operations.

---

<a id="p18"></a>
## P18 — Packaging, install/export and 1.0 review

**Status: Planned** · **Depends on:** P17

**Objective.** Make smply consumable, and confirm the documentation still
describes the software that exists.

**Scope.** Install/export targets, `smplyConfig.cmake`, versioning and SemVer
policy, `SECURITY.md`, `CHANGELOG.md`, SBOM generation, and a full read-through
of every document against the code.

**Tasks.** `install(TARGETS … EXPORT)` + config package; verify consumption from
an out-of-tree project in three ways (`find_package`, `add_subdirectory`,
`FetchContent`); generate the SBOM; write `SECURITY.md`; audit every diagram,
API example and protocol claim in `docs/` against the implementation and fix
drift; review all ADRs and mark any that reality superseded.

**Files.** `cmake/smplyConfig.cmake.in`, `CHANGELOG.md`, `SECURITY.md`,
`tests/consumer/*` extended, all of `docs/`.

**Tests.** The three consumption modes build and run a smoke program.

**Docs.** All of them — this phase is a documentation audit as much as a
packaging one.

**Gates.** Everything, plus a manual Definition-of-Done review
([`quality-gates.md`](quality-gates.md) §12) against the whole repository.

**Acceptance.** A fresh clone, consumed out-of-tree, builds and updates a device;
no document contains a claim contradicted by the code.

---

## Open questions

Tracked here until resolved; resolving one means an ADR or a doc update, not a
comment in code.

| ID | Question | Owner phase | Notes |
| -- | -------- | ----------- | ----- |
| ~~O1~~ | ~~Which licence for smply itself?~~ | ~~P0~~ | **Resolved in P0: Apache-2.0.** Permissive, proprietary-linking friendly, explicit patent grant. `LICENSE` + `NOTICE` added; every source file carries an SPDX identifier. |
| O2 | Should smply probe SMP v2 and fall back to v1? | after P17 | Deferred in [ADR-0010](decisions/ADR-0010-request-correlation.md); decide on HIL evidence. |
| O3 | Raise `max_in_flight` above 1 using `buf_count`? | after P17 | Needs HIL throughput measurements; would change retransmission reasoning. |
| O4 | Is `MemoryImageSource` enough, or is a `FileImageSource` wanted in the library? | P9 | Leaning: keep file I/O out of the core; the example provides one. |
| O5 | Multi-image (image ≥ 1) support in `UpdatePlan` — exercise it, or document as untested? | P12 | Representable already; the question is test/HIL coverage. |
| O6 | Expose a `std::error_code` interop layer? | after P12 | Only if a consumer asks. |

## Discovered follow-up work

Sessions append items here as they find them, with the phase that should absorb
them.

| Found in | Item | Absorb into |
| -------- | ---- | ----------- |
| P0 | `install(EXPORT)` for a target that links a `FetchContent`-provided static library needs care: `target_link_libraries(smply PRIVATE qcbor)` still records `$<LINK_ONLY:qcbor::qcbor>` in smply's interface, which `install(EXPORT)` rejects unless QCBOR is exported too. Options: require `find_package(qcbor)` for installed builds, or re-export. Install/export was already scoped to P18; this is the specific problem it must solve. | P18 |
| ~~P0~~ | ~~`check_docs.py` R4 is a line-based heuristic untested against templates and nested namespaces.~~ **Done in P1**: three bugs found and fixed (nested `detail` namespaces, brace-depth tracking, attribute-prefixed declarations). Still a heuristic; expect further hardening as headers grow. | — |
| P0 | `cppcheck` runs only in the `gates` CI job (it is absent from the development container), so a local `tools/lint.sh` is weaker than CI. Its first CI run reported nothing, which means the suppression list is also still largely unexercised. Revisit once there is real code. | P1 |
| P0 | The `windows-msvc` and `core-without-winrt` presets set `CMAKE_C_COMPILER=cl`, which requires a configured MSVC developer environment. Documented in the CI workflow; a developer configuring by hand outside a Developer Command Prompt will get a confusing failure. Consider a clearer diagnostic. | P18 |
| P1 | `Result` has no monadic operations (`and_then`, `transform`). std::expected has them, smply's subset does not, so using one would break the C++20 build. **P6 did not need them** — the client's chains are short and each step wants a different error message — so the ban stays explicit. Reconsider if P7–P12 start hand-rolling the same three-line unwrap. | P12 |
| P5 | The `Writer` has no way to write a nested map or an array under a key. Nothing in MCUmgr's *request* shapes needs one — every request smply sends is a flat map — so it was not built. If a future group needs it, add it rather than hand-rolling the bytes. | when needed |
| P5 | `for_each_map_in_array` visits map elements only. An array of scalars would need a separate visitor. No MCUmgr response in scope uses one. | when needed |
| P4 | `Transport` has no `connected()` query. The core learns about a lost link only through `on_disconnected()`. That is sufficient today — `SmpClient` tracks the state itself — but a transport that reconnects underneath the client would have no way to say so. Revisit if a self-healing transport is ever wanted. | P15 |
| ~~P3~~ | ~~The assembler's re-entrancy guard makes a re-entrant sink an error … the constraint should be stated in `SmpClient`'s documentation.~~ **Done in P6**: stated in `smp_client.hpp` under "Re-entrancy". | — |
| ~~P2~~ | ~~`Header` has no `is_response()` / `response_to()` helper.~~ **Done in P6**: both added to `smp/header.hpp`; the client correlates on `response_to(request.op)`. | — |
| P6 | Two obligations are documented on `SmpClient` but belong in the normative transport contract in `transport.hpp` and `design.md` §9, where a transport author will actually read them: (a) `Transport::send()` **must not** deliver inbound bytes before returning; (b) a transport must outlive every client bound to it, because the client detaches on destruction and on rebind. Consider instead making the contract symmetric — a transport that notifies its listener as it is destroyed would remove (b) entirely. | P15 |
| P6 | `quality-gates.md` §6 states branch-coverage thresholds without defining the metric. gcov "branches executed" and gcovr `--exclude-throw-branches` differ by ~25 points on the same objects. Pin the tool and the flag before enforcement switches on. | P13 |
| P6 | `src/cbor/` is at 80 % branch coverage against the ≥ 90 % elevated gate in `quality-gates.md` §6. Unenforced today; blocking at P13. | P13 |
| P1 | `to_string(const Error&)` allocates. The zero-allocation path is `to_string(ErrorCode)`, which callers must choose deliberately. If logging becomes hot, consider a caller-buffer overload. | P13 |
| P1 | Clang's `compiler-rt` is absent in the dev container, so `linux-clang-asan-ubsan` can only be verified in CI; `linux-gcc-asan-ubsan` covers sanitizers locally. | — (accepted) |
| P1 | **`verify_gates.sh` fixtures can rot silently.** Its R2 case injected its violation by rewriting `P1 ... Status: Planned`; completing P1 turned that into a no-op, so the gate went untested while the check still reported PASS. Fixed by appending a synthetic `P99` phase instead. When adding a case, make the violation independent of any real content that later work will change. | — (fixed) |
