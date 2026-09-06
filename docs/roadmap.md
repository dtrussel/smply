# Implementation roadmap

**This file is the project's execution state.** A new session determines what to
do next by reading it. Update it in the same change as the work
([ADR-0013](decisions/ADR-0013-living-documentation.md)).

Status values: `Planned` · `In Progress` · `Blocked` · `Complete`.

## Current state

| | |
| - | - |
| **Next phase to work on** | **P13 — Fuzzing, hardening and coverage push** |
| Last completed phase | P12 — `FirmwareUpdater` orchestration |
| Shipped so far | SMP codec · reassembly · transport contract · CBOR façade · `SmpClient` · OS group · **the whole image group, upload included** · MCUboot image parsing, SHA-256 and TLV scan · a simulated device and a component suite that drives the real stack into it · **`FirmwareUpdater`: the end-to-end update, reset and reconnect included**. 559 tests, 11 CI jobs green. **The portable product is functionally complete.** |
| Blocked phases | none |
| Open decisions | **Four open** — O2, O3, O5, O6. O1 (licence) and O4 (`FileImageSource`) are resolved. See [§ Open questions](#open-questions) |

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
| [P7](#p7) | OS management: reset, params, echo | Complete | P6 |
| [P8](#p8) | Image management: state read/write, erase, slot info | Complete | P6 |
| [P9](#p9) | MCUboot image file handling and SHA-256 | Complete | P1 |
| [P10](#p10) | Image upload state machine | Complete | P8, P9 |
| [P11](#p11) | `ServerSimulator` and component test harness | Complete | P7, P8, P10 |
| [P12](#p12) | `FirmwareUpdater` orchestration | Complete | P11 |
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

**Status: Complete** (2026-09-05) · **Depends on:** P2, P3, P4, P5

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

* **Whatever a callback captures must outlive the client too, and CI caught it
  where local runs could not.** `~SmpClient` completes outstanding requests, so
  a callback runs *during* destruction. Most tests declared their `Outcome`
  after the fixture, so it died first and the callback wrote through a dangling
  pointer — 13 tests, a stack-use-after-scope reported by Clang's ASan and by
  nothing else. GCC's ASan does not detect it even with
  `-fsanitize-address-use-after-scope` and `detect_stack_use_after_scope=1`
  (verified, not assumed), so the two sanitizer jobs are not interchangeable and
  the P1 note claiming otherwise is corrected below. Captures are now declared
  before the fixture, with the reason written at the fixture's definition, and
  the requirement is stated on `SmpClient` and its destructor.

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

**Status: Complete** (2026-09-05) · **Depends on:** P6

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

### Outcome

**Completed.** `include/smply/groups/os.hpp`, `src/groups/os/os_management.cpp`
and 35 new tests (239 total). Every request vector in the suite is derived from
the CBOR grammar and the field names in `protocol-notes.md` §5, not copied from
the writer's output -- an encoder that agrees with itself proves nothing. All
gates green across the five Linux presets.

**Remaining in this phase.** None.

**Protocol work.** Two facts were verified before any code was written, and
`protocol-notes.md` gained a source (S13, the OS-group server implementation):

* **Echo had no recorded wire shape.** Now documented: `{"d": str}` in,
  `{"r": str}` back, registered under *both* the read and write handler slots,
  which is why either op is legal. Reset is registered write-only and mcumgr
  params read-only, so the wrong op on those yields `ENOTSUP` rather than an
  answer.
* **A15 -- the specification and the server disagree about `force`.** The docs
  say `(int)`, "force reset if value > 0"; the server decodes it with
  `zcbor_bool_decode` and then *discards the decode result*, so an integer is
  silently ignored and the reset proceeds unforced with nothing to tell the
  client its intent was dropped. Checked at `main`, `v3.7.0` and `v3.5.0`.
  smply sends a boolean, and omits the key when not forcing.

**Deviations from the original plan.**

1. **`SmpClient` gained a public `defer()`.** P6 had the machinery but no way
   for a layer above to use it, and without it a group rejecting an argument
   would have to invoke the callback inline -- breaking the invariant that a
   callback never runs inside the call that started the operation, before the
   `RequestHandle` has been assigned. The internal deferral was generalised from
   "a callback plus an error" to arbitrary work; nothing else changed.
2. **`Callback<T>` lives in `result.hpp`, not in each group's header.**
   `api.md` declared it in both `groups/os.hpp` and `groups/image.hpp`, which
   would be a redeclaration once both are included.
3. **`SmpError` and `smp_error()` were added to `error.hpp`.** The phase's own
   task list requires `ENOTSUP` to be "surfaced for the caller to treat as
   non-fatal", and a caller can only do that if it can name the code without a
   magic 8. `smp_error()` returns `nullopt` for a group-scoped `rc`, so the two
   numbering spaces cannot be confused.
4. **`ResetOptions::boot_mode` was not implemented.** `api.md` proposed it; the
   phase scope puts it out of scope, so the field is absent rather than present
   and ignored.

**Discovered.**

* **A second category of branch that no test can reach.** `src/groups/os/` sits
  at 71.6 % branch and 85.2 % line coverage, and *every* uncovered line is one
  of eight deliberate invariant guards: three encode-failure checks that a
  `static_assert` proves unreachable, two `enter_map()` checks that
  `SmpClient::interpret()` already guarantees, and the `reject<>()`
  instantiations only those can reach. They are kept rather than deleted -- a
  decoder that assumes its input was validated elsewhere is one refactor away
  from trusting a device -- but they inflate the branch denominator exactly as
  throw branches do. That makes the P13 metric question sharper: a bare
  percentage cannot distinguish an untested path from an assertion.
* **`constexpr` on a public accessor is compiler-dependent.** `smp_error()` was
  written `constexpr` and calls `Error::mgmt()`, which is not. Clang accepted
  it; GCC rejected it under `-Winvalid-constexpr`. Caught only because the phase
  gate runs both -- and nearly missed, because a failed build leaves the previous
  test binary in place and `ctest` then reports the *old* suite passing. Build
  failures must be checked separately from test results.

---

<a id="p8"></a>
## P8 — Image management: state, erase, slot info

**Status: Complete** (2026-09-05) · **Depends on:** P6

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

**Exit.** The DFU machine can read and set image state without knowing how a
response is encoded, and P10 has the types its upload needs.

**Start here.** Read [`protocol-notes.md`](protocol-notes.md) §6 *and* §7
together before writing anything — §7 holds the two-hash distinction (upload
`sha` over the whole file versus image-state `hash` over `IMAGE_TLV_SHA256`),
which first becomes reachable in this phase and is the classic client bug.
`src/groups/os/os_management.cpp` is the shape to copy; the four rules every
group follows are in [`design.md`](design.md) §5.

### Outcome

**Completed.** `include/smply/groups/image.hpp`,
`src/groups/image/image_management.cpp` and 69 new tests (308 total): image
state read and write, erase, slot info, `ImageVersion::parse`, the accessors,
`ImageHash`, and `ImageError`/`image_error()`. All gates green across the five
Linux presets.

Request vectors in the suite are hand-derived from the CBOR grammar; responses
are built by a small **independent** encoder in the test file, sharing no code
with `src/cbor/`, so a decoder bug cannot be cancelled out by a matching encoder
bug. `golden_state_response()` is written byte by byte and asserted against that
builder, which is what keeps the builder itself honest.

**Remaining in this phase.** None.

**Protocol work.** Six findings, all from the implementation rather than the
specification, recorded in [`protocol-notes.md`](protocol-notes.md) §6/§7 with
four new sources (S14 `img_mgmt_state.c`, S15 `img_mgmt_util.c`,
S16 `img_mgmt_priv.h`, S17 `smp.c`) and a widened S10, whose entry described only
the upload handler although it also holds erase, slot info and the error
translation:

1. **The image-state `hash` is 32 *or* 64 bytes.** `IMAGE_SHA_LEN` is 64 under
   `CONFIG_MCUBOOT_BOOTLOADER_USES_SHA512` (S16). A client hard-coding 32 cannot
   read such a device's state at all.
2. **A false flag is normally *sent*, not omitted.** The "absent means false"
   rule the specification states is only the frugal-list build (S14); the
   default encodes every flag. A client reading "key present" as "true" would
   misread every ordinary device.
3. **`"version"` may be the literal `"<???>"`** when the server cannot format
   one (S14), so a version string is not guaranteed to parse.
4. **The device's version string is dotted**: `img_mgmt_ver_str()` (S15) writes
   `"1.2.3"` and appends `".4"` only for a non-zero build number. `api.md`
   proposed parsing imgtool's `"1.2.3+4"`, which is the *input* spelling.
5. **Slot-info slot entries can carry their own `"rc"`** in place of `"size"`
   when `flash_area_open()` fails — absent from the specification entirely.
6. **A16: an image-group error code often does not reach a v1 client.** The
   server translates it onto `mcumgr_err_t` and rebuilds the response (S10,
   S17), losing the detail: `HASH_NOT_FOUND` arrives as `EUNKNOWN`,
   `NO_FREE_SLOT` as `EBADSTATE` — which is where the specification's erase note
   comes from. It also explains why `image_error()` returning `nullopt` has to
   be documented as normal rather than as a malformed reply.

**One defect found and fixed outside this phase's files.**
`cbor::Reader::for_each_map_in_array` rejected an array of **exactly**
`max_elements`: it tested the cap before entering an element, so it never looked
for the end of the array and failed on the last legal one. A response holding
exactly `limits::kMaxImages` entries did not decode. The check now happens after
a successful `EnterMap`, and `test_cbor.cpp` gained the boundary case the
original suite never had — it tested 8 elements against a cap of 3, which passes
either way. This is a P5 bug, but a P5 bug that made P8's own element cap wrong,
so it is fixed here rather than deferred.

**Deviations from the original plan.**

1. **`ImageHash` is a new type; `std::optional<Hash>` would not have worked.**
   `api.md` sketched the device-reported hash as `Hash`, a fixed 32-byte array,
   which cannot hold the 64 bytes a SHA-512 bootloader reports (finding 1). It
   is now a bounded value carrying its own length, capped by a new
   `limits::kMaxImageHashLength`. The happy side effect is that the two hashes of
   protocol-notes §7 are now different C++ types, so the classic MCUmgr client
   bug fails to compile.
2. **`SlotDescriptor::size` is optional and `open_error` was added**, because a
   slot entry may report an open failure instead of a size (finding 5). Treating
   a missing `size` as malformed would reject an otherwise good response from a
   device with one unreadable flash area.
3. **`erase()` takes an `EraseOptions`** rather than `api.md`'s bare
   `std::optional<std::uint32_t>`, mirroring `ResetOptions` from P7 and letting
   a caller raise the long timeout for slow flash without an API change.
4. **`ImageError` and `image_error()` were added**, the group-scoped counterpart
   of P7's `SmpError`/`smp_error()`, and they live in the group header rather
   than in `error.hpp` because the codes are meaningless outside group 1.
5. **`ImageVersion::parse` accepts three spellings**, not `api.md`'s one: the
   device's `"1.2.3"` and `"1.2.3.4"`, and imgtool's `"1.2.3+4"`. `to_string()`
   emits the device's form, so a round trip is stable.
6. **`set_state` rejects a test with no hash locally**, with `InvalidArgument`
   via `SmpClient::defer()`. The device answers `IMG_MGMT_ERR_INVALID_HASH` for
   exactly this, and saying so without a round trip is both clearer and cheaper.

**On coverage.** Measured with `tools/coverage.sh` as configured (gcovr,
`--exclude-throw-branches`): `src/groups/image/image_management.cpp` is at
**95 % line and 89 % branch**, and the whole core moved from 93.5 % / 80.9 % to
**94.6 % line / 82.4 % branch**. Every uncovered line is one of the invariant
guards P7 described — four encode-failure checks the `static_assert` on
`kRequestBufferSize` proves unreachable, two `enter_map()` checks
`SmpClient::interpret()` already guarantees, one `status()` check with nothing
left that can poison the reader, and the `reject<>()` instantiations only those
can reach. Same category, same P13 exclusion-marker task; no new policy question.

The number is worth stating because the first measurement was **92 % line, 79 %
branch**, and the gap was not defensive code: it was a dozen genuinely reachable
bounds checks — every "out of 32-bit range" path in slot info, the wrong-typed
`splitStatus`, the empty callback — that no test exercised. Reading the
uncovered-line list rather than the percentage is what found them.

---

<a id="p9"></a>
## P9 — MCUboot image file handling and SHA-256

**Status: Complete** (2026-09-05) · **Depends on:** P1

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

### Outcome

**Completed.** `include/smply/image_source.hpp`,
`include/smply/mcuboot_image.hpp`, `src/image/` (five files) and 61 new tests
(369 total): the `ImageSource` seam, `MemoryImageSource`, the header parse,
SHA-256, and the TLV scan. All gates green across the five Linux presets.

**Remaining in this phase.** None.

**Protocol work.** `protocol-notes.md` §7 previously described the trailer in
two sentences and cited only S12, MCUboot's design document. It is now written
from the scanner — `bootutil_tlv_iter_begin()`/`_next()` — with three new
sources (S18 `bootutil/image.h`, S19 `tlv.c`, S20 `image_validate.c`). Four
rules were not recorded anywhere and are not in the design document either:

1. **`it_tlv_tot` includes its own area's four-byte header**, and
   `ih_protect_tlv_size` must equal the protected area's `it_tlv_tot`
   **exactly** — MCUboot refuses the image otherwise. A scanner treating either
   as a payload size lands four bytes short on every signed image.
2. **The two areas are walked as one contiguous run**: iteration starts inside
   the protected area and steps over the unprotected area's own header on the
   way past.
3. **The hash TLVs are `IMAGE_TLV_SHA256`, `SHA384` and `SHA512`**, all legal in
   the unprotected area (S20's `allowed_unprot_tlvs`), and never in the
   protected one — the hash covers the protected TLVs.
4. **The zero-length-TLV infinite loop this file and `testing.md` both warned
   about cannot happen.** Every advance is `4 + it_len`, so it is at least the
   entry header and the offset strictly increases. The iteration cap is still
   worth having, but it bounds *work*, not termination. ADR-0009's consequence
   listing "strictly-positive advance" as a safeguard has been corrected: it is
   a property of the format, and documenting it as a guard leaves the next
   reader hunting a bug that cannot exist.

**Deviations from the original plan.**

1. **Two public headers, not one.** `image_source.hpp` is the interface an
   application *implements*; `mcuboot_image.hpp` is the functions it *calls*.
   `api.md` had them together. An application writing a custom source has no
   reason to see the parsing API.
2. **`find_tlv_sha256` became `find_image_tlv_hash`, returning
   `std::optional<ImageHash>`.** `api.md` proposed `std::optional<Hash>`, 32
   bytes and SHA-256 only — which cannot represent what a SHA-512 bootloader
   signs, and would need a conversion at exactly the seam where the two hashes
   get confused. It now returns whichever of the three hash TLVs is present, at
   its own length, directly comparable with `ImageSlot::hash`.
3. **`McubootImageInfo` gained `protected_tlv_size`.** The TLV layout cannot be
   resolved without it, and `design.md` §7's field list omitted it.
4. **SHA-256 is smply's own Apache-2.0 code, not a vendored public-domain
   file.** ADR-0009's *decision* — no crypto library dependency — is unchanged,
   so this is an amendment to its Consequences and to `dependencies.md`'s row
   rather than a superseding ADR. It keeps every source file under the
   project's SPDX identifier and `NOTICE` free of an attribution entry for 150
   lines of a published standard. Correctness is pinned by the NIST vectors
   either way.
5. **No `tests/data/`.** Fixtures are built in code by
   `tests/support/image_builder.hpp`, with one hand-written header literal
   asserted against the builder. The roadmap named binary fixtures; opaque blobs
   are unreviewable in a diff and would need CMake plumbing for no gain.
6. **`design.md` §7 named the version field `Version`**, which is smply's SMP
   protocol-version enum. It is `ImageVersion`, P8's type. Fixed rather than
   propagated.
7. **A fifth file, `src/image/source_reader.hpp`.** The header parser and the
   TLV scanner need the same bounded-read and little-endian-load helpers; one
   definition of "a short read is a broken source" is better than two.

**Two things worth knowing.**

* **`-Wuseless-cast` is GCC-only and it rejected `static_cast<std::size_t>` over
  a `std::uint64_t`** — the same type on a 64-bit host, a real narrowing on a
  32-bit one. Clang built it happily, so this was found only by the GCC preset.
  The fix is a small `image::narrow<To>()` template: the conversion is then
  dependent, which satisfies both, and it says at the call site that the value
  was checked first. A green Clang build remains necessary, not sufficient.
* **A failed build leaves the previous test binary in place**, and it happened
  again here: `ctest` reported 364 passing off a binary whose build had just
  failed on a dangling builder member. The build's exit status has to be read
  separately, every time.

**On coverage.** `src/image/` is at **98 % line and 93 % branch**; the whole core
moved from 94.6 % / 82.4 % to **95.4 % / 82.7 %**. The three uncovered lines are
`read_exact` and `ImageHash::from` failure guards inside the TLV entry loop that
the preceding bounds checks make unreachable — the same invariant-guard category
P7 recorded, and the same P13 exclusion-marker task.

Reading the uncovered-line list rather than the percentage paid for itself
again, and more sharply than in P8: it showed that **three TLV tests were
passing without reaching the check they were named after**, because one builder
call set both `ih_protect_tlv_size` and the protected area's own `it_tlv_tot`
and so kept them agreeing. The builder now has one knob per field. A green test
that cannot fail is worth no more than a gate that cannot fail.

---

<a id="p10"></a>
## P10 — Image upload state machine

**Status: Complete** (2026-09-05) · **Depends on:** P8, P9

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

### Outcome

**Completed.** `src/groups/image/upload_session.{hpp,cpp}` (the pure decision
logic), `src/groups/image/upload_driver.{hpp,cpp}` (the I/O half),
`ImageManagement::upload`/`resume`/`cancel` and the upload types in
`groups/image.hpp`. 70 new tests (439 total). All gates green across the five
Linux presets.

**The phase's own gate is met**: `upload_session.*` is at **94 % branch and
99 % line**, against the ≥ 90 % elevated threshold — the first time that gate
has applied to new code. The single uncovered line is the probe-encode guard
that `kProbeBufferSize` makes unreachable.

**Remaining in this phase.** None.

**Protocol work.** Three behaviours were missing from
[`protocol-notes.md`](protocol-notes.md) §6, all read out of `img_mgmt.c` and
`zephyr_img_mgmt.c` (S10, S11):

1. **An upload can finish on the first packet, having transferred nothing.**
   With `off == 0` and a **full 32-byte** `sha`, the server runs
   `flash_img_check()` over the target slot before writing; if the image is
   already there it answers `off == len`, `match == true` and resets. Recorded
   as rule 9a. It also means a full `sha` answers "does the device already hold
   this image?" without the image-state round trip P12 would otherwise need.
2. **A retransmitted final chunk is answered with `off == 0`, not `off == len`**
   (rule 9b), because the server resets its session on completion and then
   inspects the duplicate against a cleared one. Read as rule 7 demands — restart
   with a first packet — that is self-correcting via 9a, in one round trip.
   Without `CONFIG_IMG_ENABLE_IMAGE_CHECK` it re-uploads instead, which is why a
   restart budget is needed either way.
3. **A device that forgot the session** (`area_id == -1`) answers a continuation
   with `off == 0`, so "the device rebooted mid-upload" needs no separate rule.
   Noted on rule 5.

Also recorded: the server "accepts SHA trimmed to any length", but only does the
resume match and the already-present check at the full 32 bytes.

**Deviations, all recorded in the docs they contradict.**

1. **There is no `Restart` action.** A restart is "set `first_packet_pending`,
   zero `confirmed_off`, then send"; a separate action would let a driver handle
   it and forget to send, which is a hang rather than an error. ADR-0008
   amended.
2. **`UploadHandle` is a generation-tagged token, not an owner.** `api.md` said
   the handle owned the upload and abandoned it on destruction; ADR-0008 says
   the state lives in `ImageManagement`. Both cannot hold. A handle carrying an
   owner pointer would *dangle* rather than go inert, which is the opposite of
   what `RequestHandle` was built to guarantee — so the operations are methods
   on `ImageManagement` and the handle carries only a generation.
3. **A disconnect completes `on_done` with `Disconnected` and keeps the
   session.** `design.md` §6 said the session was "suspended" and implied no
   callback, which breaks the exactly-once promise: a caller who never resumes
   would wait forever. §6 reconciled.
4. **`server_buf_size` is a caller-supplied `UploadOptions` field.** It belongs
   to the OS group, and `NotSupported` from that command is a normal answer to
   fall back from (A8); keeping that fallback in one place beats saving the
   caller a line and beats making group 1 depend on group 0.
5. **Adopting an offset from a first packet never charges the no-progress
   budget.** A resume that correctly lands back on the offset it already had
   would otherwise look like a stalled server. Not in the original table.
6. **`SmpClient` gained `transport_max_message_size()`.** Chunk sizing needs it
   and only the client knows which transport is bound — `rebind_transport()` can
   change the answer.
7. **A fourth pure function, `record_sent`.** `plan_next` is `const` per
   ADR-0008, so the driver has to tell the state what actually went out; that is
   what makes a byte-identical retransmission possible.

**Three things worth knowing.**

* **Clang's ASan failed two tests that GCC's ASan passed** — the CI-only bug
  class P1 recorded, encountered for the first time. A second `Outcome`
  declared after the fixture is destroyed before `~ImageManagement` completes
  the upload that still holds its callback. The lifetime rule now has two
  levels: a callback's captures must outlive **both** the client and the group.
  Chasing it surfaced a genuine defect too — a first-chunk read failure
  completed the callback *inside* `upload()`, which `design.md` §5 rule 4
  forbids. The driver now defers while `start()`/`resume()` is on the stack, and
  `upload()` returns an invalid handle, matching `SmpClient::request()`.
* **A retransmission repeats the *payload*, not the message.** The SMP header
  necessarily differs: the timeout retired the old sequence number, so a reply
  carrying it would be discarded as late (§4). The first version of the test
  asserted whole-message equality and was wrong about the protocol, not about
  the code.
* **A test violated the transport lifetime rule and only GCC noticed.** A
  `FakeTransport` declared *after* the fixture is destroyed before the client
  still bound to it, and `~SmpClient` then calls a pure virtual on it. Clang's
  build ran it happily; the GCC coverage build aborted. The rule is stated in
  `smp_client.hpp` — "declaring the transport before the client is enough" — and
  the test now says why in a comment.

**On coverage.** Whole core **95.6 % line, 82.3 % branch**;
`src/groups/image/` at 95 % line and 86 % branch, with `upload_session.*` at the
94 % branch its own gate requires. The uncovered remainder is the familiar
invariant-guard set plus two `!active_` re-entry guards in the driver.

---

<a id="p11"></a>
## P11 — `ServerSimulator` and the component harness

**Status: Complete** (2026-09-06) · **Depends on:** P7, P8, P10

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

**Exit.** Every layer below `FirmwareUpdater` is exercised end to end, so P12
can be developed against behaviour rather than against mocks.

**Start here** *(written before the phase; kept as the record of what it was
built for)*. The simulator is only worth having if it can produce the responses
that make a *correct* client look broken, so build these three in from the start
rather than bolting them on:

* **The already-present completion** (PN §6 rule 9a): given a full 32-byte
  `sha`, answer the first packet with `off == len` and `match == true`. Without
  it, P12 will never exercise "the device already holds this image", which is
  the shortest path through the whole update.
* **The lost-final-chunk answer** (rule 9b): after a completed upload the server
  resets, so a duplicate final chunk gets `off == 0` — not `off == len`. This is
  the one place a successful upload looks like a restart.
* **Offset correction** (rule 5): answer a wrong offset with *success* and the
  offset the server actually wants, including one **larger** than the client
  sent.

Read [`protocol-notes.md`](protocol-notes.md) §6 as the specification for the
simulator — it is written as server behaviour, which is exactly what this phase
implements. The client half of each rule already has a test in
`tests/unit/test_upload_session.cpp`; the simulator is the other side of the
same table.

### Outcome

**Completed.** `tests/support/server_simulator.{hpp,cpp}` (the device),
`tests/support/test_cbor.{hpp,cpp}` (an independent CBOR codec),
`tests/component/` as a second executable with `harness.hpp`,
`test_simulator.cpp` and `test_round_trip.cpp`. 48 new tests (487 total). All
gates green across the six Linux presets that link here, and in CI.

**Acceptance met.** A full upload through the real client stack into the
simulator reproduces the source image byte for byte, in **both** SMP versions,
and the same holds across a forced restart, a device reboot mid-transfer, a
disconnect-and-resume, and an offset correction that names a position *ahead*
of what the client sent.

**Remaining in this phase.** None.

**Protocol work.** The upload rules in §6 held as recorded, and are now
implemented from the server side. Four findings are new, three of which change
what P12 can do:

1. **`match` on the final chunk is computed even without a usable `sha`, and is
   then `false`** (new rule 9c). The final-chunk check has no length guard: it
   compares the flashed hash against the stored `sha` **zero-padded to 32
   bytes** (S10). Trim the `sha`, or omit it, and a perfectly good upload
   reports `match: false` — which rule 9 says means failure. Sending the full 32
   bytes is not merely better, it is required for `match` to mean anything.
2. **Slot flags are derived from the swap type, not stored** (§7, new table,
   from S14). The `REVERT` row is the signature of a trial boot and is easy to
   read backwards: the image that is *running* reports `active` with **no**
   `confirmed`, and the slot holding the fallback reports `confirmed`.
3. **Confirming a slot that is not the running one is denied by default**
   (`IMAGE_CONFIRMATION_DENIED`, unless
   `CONFIG_MCUMGR_GRP_IMG_ALLOW_CONFIRM_NON_ACTIVE_SLOT`). ⇒ **P12 cannot
   implement "confirm immediately" by confirming the uploaded image before the
   swap.** The portable order is test → reset → confirm, which is the safe one
   anyway. §6's warning about confirming a pending image describes a build
   option, not the default.
4. **The swap-type state machine is now written down** (§7, from S21's
   `boot_swap_tables`, `boot_swap_type_multi()` and `boot_set_next()`), together
   with the two other set-state refusals: test-on-the-running-slot, and
   `ALREADY_PENDING` for any change while a swap is scheduled — where
   re-requesting exactly what is already scheduled is a **success that does
   nothing**, so an idempotent retry is safe.

**Deviations, all recorded in the docs they contradict.**

1. **`ServerConfig` has no SMP version.** `testing.md` §2's sketch had one, but
   the version on the wire is the client's to choose and a real server answers
   in the version it was asked in. What a device does choose is whether it
   *translates* an image-group code for a v1 request (A16), which is now
   `translate_v1_errors`. Both suites run against both versions by setting
   `SmpClientConfig::smp_version`.
2. **Scripted faults are methods, not config fields.** A `ServerConfig` that
   carried "restart at offset N" would stop being a description of a device and
   become a test script. `answer_offset_once()`, `fail_next()`,
   `drop_next_response()` and `reset_busy_once()` live on the simulator instead.
3. **`answer_offset_once()` changes only the answer, never the flash.** An
   injection that also moved the device's own offset would model a server that
   lies about what it holds, and the byte-exact comparison could then never
   pass. As it stands a client that follows the correction is put right on the
   next round trip and still flashes a correct image, while one that computes
   its own offsets flashes a corrupt one — which is precisely the asymmetry the
   acceptance test needs.
4. **The simulator is not a `Transport`.** It watches `FakeTransport::sent()`
   and answers from `pump()`, because `Transport::send()` may not deliver
   inbound bytes before returning; replying inline would re-enter reassembly,
   which the assembler refuses.
5. **The simulator's own tests live in `tests/component/`**, not in
   `tests/unit/` beside `test_fake_transport.cpp`. They share a fixture with the
   tests that use it, and the unit executable stays about `src/`.
6. **`ImageState::operator==` was not added.** The P8 follow-up row offered it
   "when needed"; the component tests assert on individual flags, which is what
   makes a failure readable, so the row stays open rather than gaining unused
   API.
7. **The device models one image and two slots.** A second image pair is
   recorded as follow-up work; nothing below P12 needs it, and `single_image`
   still exercises the client's decoding of an explicit `image` key.

**Two things worth knowing.**

* **Reusing `image::Sha256` in the simulator is safe; reusing `cbor::Reader`
  would not have been.** The distinction is what the reused code is anchored
  to. SHA-256 is pinned by the published FIPS 180-4 vectors in
  `test_sha256.cpp`, so a bug there fails that test first and cannot cancel out
  here. The CBOR façade is anchored only by hand-built vectors in the unit
  tests, so a symmetric encoder/decoder bug would sail straight through a round
  trip that used it on both ends — which is exactly what this phase's
  acceptance criterion is supposed to catch. Hence `test_cbor.*`.
* **A `std::vector<std::pair<std::string, Value>>` inside `Value` compiles
  under GCC and is rejected by Clang** — a `std::pair` of an incomplete type is
  undefined, while `std::vector<T>` inside `T` is specifically allowed. The map
  is now stored flattened as key, value, key, value, exactly as CBOR encodes
  one. Another entry in the "build every preset" ledger.

---

<a id="p12"></a>
## P12 — `FirmwareUpdater` orchestration

**Status: Complete** (2026-09-06) · **Depends on:** P11

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
repeated in SMP v2 (`SmpClientConfig::smp_version`) and with params
unsupported.

**Docs.** [`design.md`](design.md) §8 diagram and table reconciled;
[`api.md`](api.md) DFU section; [`architecture.md`](architecture.md) if
component boundaries moved.

**Gates.** All P11 gates. **Branch coverage of `src/dfu/` ≥ 90 %.**

**Acceptance.** All component scenarios pass with no wall-clock dependence.

**Exit.** The portable product is functionally complete; everything after this
is hardening, platform work and packaging.

### Outcome

**Completed.** `include/smply/dfu/firmware_updater.hpp`,
`src/dfu/update_state_machine.{hpp,cpp}` (the pure machine) and
`src/dfu/firmware_updater.cpp` (the effects), with
`tests/unit/test_update_state_machine.cpp` and
`tests/component/test_firmware_update.cpp`. 72 new tests (559 total). All gates
green across the six Linux presets that link here, and in CI.

**The phase's own gate is met**: `src/dfu/` is at **93 % branch** against the
≥ 90 % threshold, and whole-core coverage rose to **96.3 % line, 84.3 %
branch** (from 95.6 / 82.3). Getting there took reading the uncovered-branch
list rather than the number: the first measurement was **81 %**, and almost all
of the gap was the "event not legal in this state" fall-through, which a single
spot-check test had left unexercised in thirteen of the fourteen states.

**The decision this phase had to make first — [ADR-0014](decisions/ADR-0014-confirmation-is-the-applications-call.md).**
P11 established that a device refuses `confirm` on any slot that is not the
running one, so `UpdateMode::ConfirmImmediately` could not do what `api.md`
promised. Worse, the replacement P11 sketched ("the same as the default, minus
the pause") was wrong, because the default flow had no pause: `Confirming`
followed `VerifyingBooted` automatically, which made the two modes
indistinguishable and left `TestThenConfirm` promising a test that nothing
performed. **The default now stops at `AwaitingConfirmation` and hands the
decision to the application**; `ConfirmImmediately` runs the identical sequence
without asking. Both modes mean something, and the rollback net still protects
the un-validated case either way.

**Remaining in this phase.** None.

**Deviations, each recorded in the document it contradicts.**

1. **`FirmwareUpdater::confirm()`, `UpdateState::AwaitingConfirmation` and
   `UpdateEvent::Kind::ConfirmationRequired` are new public API**, per ADR-0014.
   `api.md` and `design.md` §8 are updated; the §8 diagram gains the state.
2. **`Planning` is a real state with a real transition**, reached by an
   `Effect::Continue` the driver feeds straight back in. A decision needing no
   I/O would otherwise be invisible — and untestable as a state.
3. **The planner has four cases, not one.** Two of them exist because an
   application may be restarted *mid-update*: a device already running the
   target unconfirmed lands directly in the confirmation window, and one already
   holding it marked lands directly on the reset. That also makes the
   `ImageAlreadyPending` recovery fall out for free rather than needing its own
   path.
4. **A reset that times out is treated like one whose link dropped** (A3
   widened). The device may reset before its answer goes out; failing here would
   abandon a device that is already swapping, and the verify after the reboot is
   the real check.
5. **`UpdateReport` gained `revert_pending` and `upload_skipped`, and lost
   `chunk_retries`/`upload_restarts`.** The retry counters live inside the
   upload and were never plumbed out; inventing fields the library does not fill
   is worse than not having them. `revert_pending` is the one a caller cannot do
   without: "cancelled" and "cancelled, and this device will undo the swap when
   it next restarts" are different outcomes.
6. **`UpdateReport::target_hash` is an `ImageHash`, not a `Hash`.** `api.md`
   said the latter, which is the *upload* hash; what the report carries is what
   the device reports for a slot. P8 made them different types precisely so this
   could not be got wrong silently.
7. **The updater learns about a dropped link by asking the client**, not by
   listening to the transport. It never touches a connection (ADR-0004), and
   `SmpClient::connected()` already knows.

**Two things worth knowing.**

* **`ServerSimulator::fail_next()` gained an operation filter, and it caught two
  tests passing for the wrong reason.** The image group's read and write share
  command 0, so "fail the next set-state" was silently failing the get-state
  before it — both the refused-confirm and refused-mark tests were green without
  ever reaching the path they are named after. This is the same trap P9 hit with
  a fixture builder, and the same lesson: **a fault injector that cannot name
  its target will hit the wrong one.**
* **A `FakeTransport` is terminally disconnected once dropped**, exactly as a
  real link is, so an update that goes round the reconnect loop twice — an
  interrupted upload does — needs a fresh transport per cycle. The component
  harness's `Application` helper takes a list of spares for that reason.

**Start here.** Three things P11 established change the design before it is
written, and all three are in [`protocol-notes.md`](protocol-notes.md) §7:

* **`ConfirmImmediately` cannot be implemented by confirming the uploaded image
  before the swap.** An ordinary build denies a confirm on any slot that is not
  the running one. Decide what that mode *does* mean — probably test → reset →
  confirm without waiting for the caller's approval — before writing the state
  machine around a command that will fail on real hardware.
* **The signature of a trial boot reads backwards.** After the swap, the running
  image is `active` with **no** `confirmed`, and the slot holding the image it
  will fall back to is the one marked `confirmed`. That, not a `pending` flag,
  is how `RolledBack` is detected.
* **Set-state refuses more than S4 suggests**, and one refusal is a success:
  re-requesting exactly the swap that is already scheduled returns OK and does
  nothing, so an idempotent retry after a lost response is safe. Any *other*
  change while a swap is scheduled is `ALREADY_PENDING`.

Two more, from §6: a full 32-byte `sha` is what makes "does the device already
hold this image?" answerable in one round trip (rule 9a) — the shortest path
through the whole update — and it is also what makes `match` meaningful at all
(rule 9c). Component tests go in `tests/component/`; `harness.hpp` already has
the fixture and the bounded `run_until()` loop.

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
| O2 | Should smply probe SMP v2 and fall back to v1? | after P17 | Deferred in [ADR-0010](decisions/ADR-0010-request-correlation.md); decide on HIL evidence. **P8 adds a concrete cost to staying on v1**: a server built with `CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL` translates image-group codes onto `mcumgr_err_t` and drops the group, so `HASH_NOT_FOUND` is indistinguishable from a generic failure (protocol-notes §9, A16). |
| O3 | Raise `max_in_flight` above 1 using `buf_count`? | after P17 | Needs HIL throughput measurements; would change retransmission reasoning. |
| ~~O4~~ | ~~Is `MemoryImageSource` enough, or is a `FileImageSource` wanted in the library?~~ | ~~P9~~ | **Resolved in P9: `MemoryImageSource` only.** `ImageSource` is two virtual functions, so a file-backed source is a dozen lines in the application, and adding one to the core would drag in file I/O, paths and error mapping across three platforms for no protocol benefit (architecture.md §2). The P14 example provides one. |
| O5 | Multi-image (image ≥ 1) support in `UpdatePlan` — exercise it, or document as untested? | P12 | Representable already; the question is test/HIL coverage. |
| O6 | Expose a `std::error_code` interop layer? | after P12 | Only if a consumer asks. |

## Discovered follow-up work

Sessions append items here as they find them, with the phase that should absorb
them.

| Found in | Item | Absorb into |
| -------- | ---- | ----------- |
| P0 | `install(EXPORT)` for a target that links a `FetchContent`-provided static library needs care: `target_link_libraries(smply PRIVATE qcbor)` still records `$<LINK_ONLY:qcbor::qcbor>` in smply's interface, which `install(EXPORT)` rejects unless QCBOR is exported too. Options: require `find_package(qcbor)` for installed builds, or re-export. Install/export was already scoped to P18; this is the specific problem it must solve. | P18 |
| ~~P0~~ | ~~`check_docs.py` R4 is a line-based heuristic untested against templates and nested namespaces.~~ **Done in P1**: three bugs found and fixed (nested `detail` namespaces, brace-depth tracking, attribute-prefixed declarations). Still a heuristic; expect further hardening as headers grow. | — |
| ~~P0~~ | ~~`cppcheck` runs only in the `gates` CI job (it is absent from the development container), so a local `tools/lint.sh` is weaker than CI.~~ **Corrected in P7**: cppcheck *is* installable in the container (`apt-get install -y cppcheck`), and the premise cost a red CI cycle before that was tried. Install it and run `tools/lint.sh` in full before pushing. | — |
| P0 | The `windows-msvc` and `core-without-winrt` presets set `CMAKE_C_COMPILER=cl`, which requires a configured MSVC developer environment. Documented in the CI workflow; a developer configuring by hand outside a Developer Command Prompt will get a confusing failure. Consider a clearer diagnostic. | P18 |
| P1 | `Result` has no monadic operations (`and_then`, `transform`). std::expected has them, smply's subset does not, so using one would break the C++20 build. **P6 did not need them** — the client's chains are short and each step wants a different error message — so the ban stays explicit. Reconsider if P7–P12 start hand-rolling the same three-line unwrap. | P12 |
| P5 | The `Writer` has no way to write a nested map or an array under a key. Nothing in MCUmgr's *request* shapes needs one — every request smply sends is a flat map — so it was not built. If a future group needs it, add it rather than hand-rolling the bytes. | when needed |
| P5 | `for_each_map_in_array` visits map elements only. An array of scalars would need a separate visitor. No MCUmgr response in scope uses one. | when needed |
| P4 | `Transport` has no `connected()` query. The core learns about a lost link only through `on_disconnected()`. That is sufficient today — `SmpClient` tracks the state itself — but a transport that reconnects underneath the client would have no way to say so. Revisit if a self-healing transport is ever wanted. | P15 |
| ~~P3~~ | ~~The assembler's re-entrancy guard makes a re-entrant sink an error … the constraint should be stated in `SmpClient`'s documentation.~~ **Done in P6**: stated in `smp_client.hpp` under "Re-entrancy". | — |
| ~~P2~~ | ~~`Header` has no `is_response()` / `response_to()` helper.~~ **Done in P6**: both added to `smp/header.hpp`; the client correlates on `response_to(request.op)`. | — |
| P6 | Two obligations are documented on `SmpClient` but belong in the normative transport contract in `transport.hpp` and `design.md` §9, where a transport author will actually read them: (a) `Transport::send()` **must not** deliver inbound bytes before returning; (b) a transport must outlive every client bound to it, because the client detaches on destruction and on rebind. Consider instead making the contract symmetric — a transport that notifies its listener as it is destroyed would remove (b) entirely. | P15 |
| ~~P6~~ | ~~`quality-gates.md` §6 states branch-coverage thresholds without defining the metric.~~ **Resolved in P7's documentation pass**: §6 now pins the metric to what `tools/coverage.sh` reports (gcovr with `--exclude-throw-branches`), the script passes that flag, and §6 records the measured position. | — |
| P6 | `src/cbor/` is at 80.9 % branch coverage against the ≥ 90 % elevated gate in `quality-gates.md` §6. The one area below its gate. Unenforced today; P13 must either raise it or move the directory out of the elevated list — not quietly drop the row. | P13 |
| P7 | Deliberate invariant guards are unreachable by construction and drag branch coverage down — `src/groups/os/` is at 71.6 % with *only* guards uncovered. **This is not an open question**: `quality-gates.md` §6 already rules that forcing coverage of unreachable defensive code is unwanted and that such lines should carry an exclusion marker with a comment. The task is to apply the markers to the eight guards in `os_management.cpp` (and any later equivalents), not to decide policy. | P13 |
| ~~P7~~ | ~~A failed build leaves the previous test binary in place, so `ctest` reports the old suite passing.~~ **Recorded in `handoff.md` § Standing caveats**; no code change needed, it is a habit. | — |
| P7 | **`tools/coverage.sh` produced no report at all, from P0 until P7.** `--txt "$BUILD_DIR"` made gcovr treat the build directory as that option's output file; the script exits 0 by design, so the CI coverage job stayed green while reporting nothing. Fixed in P7's documentation pass. The lesson generalises: a gate that cannot fail is not a gate, and `verify_gates.sh` covers the *checkers* but not this reporter — consider extending it. | P13 |
| P7 | **cppcheck cannot parse some Catch2 test translation units.** It runs without the project's include paths, so `TEST_CASE`/`SECTION`/`REQUIRE` are unknown macros, and it explores preprocessor configurations no build uses (it reports `toomanyconfigs` on the same run). On `test_os_group.cpp` that yields a `syntaxError` at the first `TEST_CASE`; passing `-I` and a smaller check set makes it vanish, which is what identifies it as a parser artefact rather than a defect. Suppressed narrowly (`syntaxError:tests/*`) with the reason written in `tools/cppcheck-suppressions.txt`. The better fix is to give `lint.sh` the include paths and exclude `_deps/`, but that made cppcheck report findings inside Catch2's own headers, so it needs its own pass — do it before P13 leans on cppcheck for anything. | P13 |
| ~~P8~~ | ~~`cbor::Reader::for_each_map_in_array` rejected an array of exactly `max_elements`: the cap was tested before entering an element, so the end of the array was never looked for and the last legal element failed.~~ **Fixed in P8**, in `src/cbor/reader.cpp`, with the boundary case added to `test_cbor.cpp` — the original test used 8 elements against a cap of 3, which passes either way. Found because an image-state response with exactly `limits::kMaxImages` entries did not decode. | — (fixed) |
| P8 | `cbor::Reader::enter_map(key)` is the one failure path that does **not** call `record()`: a missing or non-map nested key returns a failed `Result` but leaves `status()` clean. Every other error is sticky. Nothing uses it yet — P8's nested decoding goes through `for_each_map_in_array` — but a caller that follows the house rule of "check `status()` at the end" would miss this one. Make it sticky, or document why it is not. | P13 |
| P8 | `upload_image_id` in a slot-info response means two different things: the global slot index plus one under `CONFIG_MCUMGR_GRP_IMG_DIRECT_UPLOAD`, and the image number without it (protocol-notes §6). smply reports the number verbatim and does not try to tell the two apart. P10 must decide whether the upload path may use it at all, or whether it stays advisory. | P10 |
| P8 | `ImageState` has no `operator==`, so a test comparing two whole states has to compare `slots` and `split_status` separately. Trivial to add if P11's component tests want it. | when needed |
| P9 | **Compressed images are unhandled.** MCUboot has `IMAGE_F_COMPRESSED_LZMA1/LZMA2/ARM_THUMB_FLT` and a separate `IMAGE_TLV_DECOMP_SHA`, whose relationship to the slot hash is the same question encrypted images raise (A13). smply carries the flags through and does not interpret them, so a compressed image's hash correlation is unverified rather than known-wrong. Decide whether to flag it like `encrypted`, or to document it as untested. | P12 |
| P9 | `fuzz_mcuboot_header` and `fuzz_tlv_scan` are specified in `testing.md` §5 but not built. The TLV scanner is the strongest candidate in the library for fuzzing — it indexes with file-supplied offsets — and it is written to be a pure function over an `ImageSource`, so a target is a few lines. | P13 |
| P9 | `image::narrow<To>()` exists because GCC's `-Wuseless-cast` rejects a `static_cast` between `std::uint64_t` and `std::size_t` on a 64-bit host. It is in `src/image/` because that is where it was needed; if a second area needs the same thing, promote it rather than copying it. | when needed |
| P9 | The TLV entry cap counts loop iterations, so the one iteration spent stepping over the unprotected area's header consumes a unit of the budget. Immaterial at 256, but if the cap is ever tightened it should count entries. | when needed |
| ~~P10~~ | ~~**`ImageManagement` is now stateful, and nothing enforces the lifetime rule it introduces.**~~ **Done in P11**: `test_round_trip.cpp`'s "destroying the group mid-upload completes the callback once" destroys the whole fixture with an upload in flight, under both sanitizer presets. | — |
| P12 | The upload's retry and restart counters are not plumbed out to `UpdateReport`, so a caller cannot see that an update succeeded only after three retransmissions. `UploadResult` would have to carry them first. | when a caller asks |
| P12 | `UpdateReport::upload_skipped` reflects only the updater's **pre-flight** decision. When the *server* completes an upload on the first packet (rule 9a) the client cannot tell — `UploadResult` does not say whether any data was sent. Adding that would also let a caller distinguish "already present" from "transferred". | P13 |
| P12 | The `life.expired()` guards in `FirmwareUpdater`'s callbacks are exercised for one callback type out of six; the other five are unreachable without destroying the updater with that specific request in flight. They are invariant guards of the kind §6 says should carry a coverage-exclusion marker. | P13 |
| P11 | The simulator models **one image and two slots**, and refuses an upload naming any other image with `NoFreeSlot` rather than writing slot 1 regardless. So `UploadOptions::image` has no end-to-end coverage: a device with two image pairs reports an `image` key that varies per entry and carries a second swap state. | P12, if the DFU flow grows multi-image support |
| P11 | `ServerSimulator::pump()` answers everything queued in one turn, so `buf_count` and any notion of the device being busy with a previous packet are not modelled. A device that refused a second request while one was outstanding would exercise the client's `max_in_flight` path from the far side. | when needed |
| P11 | The component suite left `src/` coverage unchanged at 95.6 % line / 82.3 % branch — it covers *sequences*, which line coverage cannot see. Worth remembering before anyone reads a flat coverage number as a measure of what the component tests are worth. | — |
| P10 | The upload driver keeps the chunk in a second buffer before encoding it, because `cbor::Writer` needs the bytes up front. A `put_bytes_from()` that let a caller fill the byte string in place would remove a 512-byte copy per chunk. Immaterial at these sizes; revisit only if a profile says so. | when needed |
| P10 | `fuzz_cbor_upload_response` is specified in `testing.md` §5 and not built. The response decode is small, but it is device-supplied and feeds a state machine with budgets — a good target. | P13 |
| P10 | A retransmission necessarily carries a new sequence number, so a device that deduplicates on `seq` would see two distinct requests at the same offset. Harmless against Zephyr, which keys on offset, but worth checking on real hardware. | P17 |
| P1 | `to_string(const Error&)` allocates. The zero-allocation path is `to_string(ErrorCode)`, which callers must choose deliberately. If logging becomes hot, consider a caller-buffer overload. | P13 |
| P1 | Clang's `compiler-rt` is absent in the dev container, so `linux-clang-asan-ubsan` can only be verified in CI. **P6 correction: `linux-gcc-asan-ubsan` does not "cover sanitizers locally".** GCC's ASan does not report stack-use-after-scope for a dangling callback capture — verified by running the failing case under GCC with `-fsanitize-address-use-after-scope` and `detect_stack_use_after_scope=1`, which passed while Clang aborted. The two jobs are not interchangeable, and this class of bug is CI-only. | — (accepted) |
| P1 | **`verify_gates.sh` fixtures can rot silently.** Its R2 case injected its violation by rewriting `P1 ... Status: Planned`; completing P1 turned that into a no-op, so the gate went untested while the check still reported PASS. Fixed by appending a synthetic `P99` phase instead. When adding a case, make the violation independent of any real content that later work will change. | — (fixed) |
