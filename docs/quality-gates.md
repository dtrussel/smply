# Quality gates

Every gate below runs in CI and blocks merge unless marked *advisory*.

**Status (as of P10):** the gates marked *(P13)* / *(P15)* below are specified
but not yet wired, because the targets they would exercise do not exist.
Everything else is live and enforced. Each live gate has been observed rejecting
a deliberate violation — `tools/verify_gates.sh` reproduces that proof, and the
`gate-self-check` CI job runs it on every push.

## 1. Build matrix (required)

| Job | OS | Compiler | Standard | Notes |
| --- | -- | -------- | -------- | ----- |
| `linux-gcc` | ubuntu-latest | GCC 13 | C++20 | core + tests |
| `linux-clang` | ubuntu-latest | Clang 18 | C++20 | core + tests |
| `linux-gcc-fallback-expected` | ubuntu-latest | GCC 13 | C++20 | forces smply's own `expected<>` even where `std::expected` exists (ADR-0002) |
| `linux-gcc-cxx23-std-expected` | ubuntu-latest | GCC 13 | **C++23** | builds the same tests against `std::expected`. Under the C++20 baseline the standard type does not exist, so without this job only smply's own backing is ever exercised and ADR-0002's interchangeability claim is untested. C++20 remains the baseline (ADR-0001); this job only proves the C++23 path works. |
| `windows-msvc` | windows-latest | MSVC v143 | C++20 | core + tests |
| `windows-winrt` *(P15)* | windows-latest | MSVC v143 | C++20 | `-DSMPLY_BUILD_WINRT=ON`: adapter + example |
| `core-without-winrt` | windows-latest | MSVC v143 | C++20 | **API-discipline gate**: `-DSMPLY_BUILD_WINRT=OFF` must build cleanly |
| `linux-clang-asan-ubsan` | ubuntu-latest | Clang 18 | C++20 | tests under ASan+UBSan |
| `linux-gcc-asan-ubsan` | ubuntu-latest | GCC 13 | C++20 | the same, under GCC — the two implementations do not diagnose identically, and GCC's runtime is available where Clang's `compiler-rt` package is not |
| `linux-clang-fuzz-smoke` *(P13)* | ubuntu-latest | Clang 18 | C++20 | each fuzz target, `-runs=20000` over the seed corpus |
| `linux-gcc-coverage` | ubuntu-latest | GCC 13 | C++20 | gcovr/lcov, uploads the report |
| `gates` | ubuntu-latest | Clang 18 | C++20 | format, clang-tidy, cppcheck, and the three `check_*.py` scripts |
| `gate-self-check` | ubuntu-latest | Clang 18 | C++20 | `tools/verify_gates.sh` — proves each gate rejects a violation |
| `nightly-fuzz-soak` *(P13)* | ubuntu-latest | Clang 18 | C++20 | 30 min per target (*advisory*, opens an issue on a find) |

Minimum supported toolchains are GCC 11, Clang 14 and MSVC 19.30 (ADR-0001); CI
pins the versions above. Clang's sanitizer jobs need `libclang-rt-<v>-dev`
installed — without it the link fails with a missing `libclang_rt.asan`.

**Dependency headers are included as `SYSTEM`** (`FetchContent_Declare(... SYSTEM)`,
CMake ≥ 3.25). This is not cosmetic: without it, clang-tidy attributes findings
inside Catch2's headers to *our* test files through macro expansion — 140 errors
from two trivial `TEST_CASE`s — and the gate becomes unusable.

MSVC additionally builds with `/permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8`.

## 2. Compiler warnings (required)

Warnings are **errors for smply's own targets only**, applied through the
`smply_internal_options` INTERFACE target linked `PRIVATE` — third-party code and
downstream consumers are unaffected.

```cmake
# cmake/warnings.cmake
if(MSVC)
  target_compile_options(smply_internal_options INTERFACE
    /W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8
    /w14242 /w14254 /w14263 /w14265 /w14287 /we4289 /w14296
    /w14311 /w14545 /w14546 /w14547 /w14549 /w14555 /w14619 /w14640
    /w14826 /w14905 /w14906 /w14928)
else()
  target_compile_options(smply_internal_options INTERFACE
    -Wall -Wextra -Wpedantic -Werror
    -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
    -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
    -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
    -Wnull-dereference -Wextra-semi)
  # GCC only
  -Wduplicated-cond -Wduplicated-branches -Wlogical-op -Wuseless-cast
endif()
```

`-Wconversion`/`-Wsign-conversion` are deliberately included: this is a protocol
library where a silent narrowing of a length or offset is a bug class, not
noise. Deliberately **excluded**: `-Weverything`, `/Wall`, `-Wpadded`,
`-Wswitch-default` (exhaustive switches without `default` are a design rule —
see [`design.md`](design.md) §11).

## 3. Static analysis (required)

**clang-tidy** on all core sources, warnings-as-errors. `.clang-tidy`:

```yaml
Checks: >
  -*,
  bugprone-*,
  -bugprone-easily-swappable-parameters,
  cert-*, -cert-err58-cpp,
  clang-analyzer-*,
  concurrency-*,
  cppcoreguidelines-pro-type-reinterpret-cast,
  cppcoreguidelines-pro-type-const-cast,
  cppcoreguidelines-pro-type-cstyle-cast,
  cppcoreguidelines-owning-memory,
  cppcoreguidelines-slicing,
  cppcoreguidelines-init-variables,
  cppcoreguidelines-narrowing-conversions,
  misc-*, -misc-non-private-member-variables-in-classes,
  modernize-*, -modernize-use-trailing-return-type,
  performance-*,
  readability-*, -readability-magic-numbers,
    -readability-identifier-length, -readability-function-cognitive-complexity
WarningsAsErrors: '*'
HeaderFilterRegex: '(include/smply|src)/.*'
```

Rationale for the notable choices: `cppcoreguidelines-pro-type-*-cast` and
`owning-memory` encode the robustness rules in [`design.md`](design.md) §11
mechanically; `readability-magic-numbers` is off because protocol constants are
named in one place already and the check fires constantly on test vectors;
`easily-swappable-parameters` is off because `(offset, length)` pairs are
inherent here; **`performance-enum-size` is off** because underlying types here
are chosen to match the wire format and to leave room for protocol growth, not
to minimise `sizeof` -- `Group` must be `uint16_t` because the SMP header
carries 16 bits, so the check would fight every protocol enumeration for no
benefit.

Note that `.clang-tidy`'s `Checks:` value is a YAML *folded scalar*: a `#`
inside it is not a comment, it becomes part of the check list. Rationale
comments go above the key.

`clang-analyzer-optin.core.EnumCastOutOfRange` is also off, for a reason
specific to this domain: it assumes an enumeration's valid values are exactly
its enumerators, which is false for a wire-format enumeration. `Group` is open
by design — every 16-bit value is legal and must round-trip — so decoding one is
a `static_cast` from an arbitrary value by construction, and the check fires on
correct code throughout the codec, the groups layer and their tests.

One check is suppressed at specific call sites rather than globally, with a
written reason at each: `bugprone-unchecked-optional-access`, where a `REQUIRE`
already guarantees engagement but the checker cannot see through Catch2's
macros.

**cppcheck** runs as a complement (`--enable=warning,performance,portability
--inline-suppr --error-exitcode=1`, C++20, suppressions in
`tools/cppcheck-suppressions.txt`). It is kept because its whole-program value
tracking finds different defects from clang-tidy's AST checks; overlap is
suppressed rather than duplicated.

## 4. Formatting (required)

`.clang-format` (LLVM base, 4-space indent, 100 columns, pointer-left,
`AllowShortFunctionsOnASingleLine: Empty`). CI runs
`clang-format --dry-run --Werror` over `include/ src/ tests/ transports/
examples/` and fails on any difference. `tools/format.sh` applies it locally;
a pre-commit hook is offered but not required.

## 5. Tests (required)

`ctest --output-on-failure` must be green in every matrix job. New protocol
logic without a test is a review blocker, not a CI blocker
(see Definition of Done).

## 6. Coverage (required, with judgement)

Measured on `linux-gcc-coverage`, over `src/` and `include/smply/` only
(tests, examples and `transports/` excluded).

**The metric is exactly what `tools/coverage.sh` reports**: gcovr with
`--exclude-throw-branches`. Pinning this matters more than it sounds — on the
same object files the branch figure moves by roughly 12 points depending on that
one flag, because every potentially-throwing call contributes two branches a
suite that raises no exceptions can never take. A threshold that does not name
its tool and flags means whichever number CI happens to produce.

**Not enforced until P13.** `tools/coverage.sh` reports and CI publishes the
artefact without failing. Two things must be settled before the thresholds can
switch on, and both are P13's:

* `src/cbor/` is below its elevated gate and has been since P6. P13 either
  raises it or moves the directory out of the elevated list — it does not
  quietly drop the row.
* Deliberate invariant guards — unreachable by construction, kept because a
  decoder that assumes its input was validated elsewhere is one refactor away
  from trusting a device — count against the branch denominator exactly as
  throw branches do. §6 already rules that such lines carry an exclusion
  marker; applying them is the outstanding task.

| Gate | Threshold | Measured 2026-09-05 (P10) |
| ---- | --------- | ------------------------- |
| Line coverage, whole core | **≥ 85 %** | 95.6 % ✓ |
| Branch coverage, whole core | **≥ 75 %** | 82.3 % ✓ |
| Branch coverage, `src/smp/`, `src/cbor/`, `src/groups/image/upload_session.*`, `src/dfu/update_state_machine.*` | **≥ 90 %** | `src/smp/` 96 % ✓ · **`upload_session.*` 94 % ✓** · `src/cbor/` 82 % ✗ |
| Regression | no drop > 1 pp vs. the base branch | — |

P10 is the first phase whose own acceptance criterion was one of the elevated
gates, and `upload_session.*` clears it at 94 % branch and 99 % line.

For reference, outside the elevated list: `src/image/` is at 98 % line and 93 %
branch, `src/groups/image/` at 95 % line and 86 % branch, `src/groups/os/` at
85 % line.

**`gcovr` is not installed in the development container**, and `coverage.sh`
falls back to plain `gcov` without failing — whose branch metric is a different
measurement and not comparable with the numbers above. `pip install gcovr`
before quoting one.

**`--txt` takes the next argument as its output file.** Running gcovr by hand as
`gcovr … --txt <build-dir>` fails with "Is a directory" — the same mistake that
silently disabled `coverage.sh` from P0 to P7. Put the search path first, or
omit `--txt` entirely: text is the default.

`src/cbor/` is the one area below its gate. It is unenforced until P13, but P13
cannot switch enforcement on without either raising that coverage or moving the
directory out of the elevated list — decide which, do not quietly drop the row.

The elevated per-directory gate is the point of the exercise: those four areas
are pure decision logic over untrusted input, where a missed branch is a real
untested protocol path. Coverage elsewhere (glue, formatting, accessors) is
informational — **the percentage is not a goal, the branch table in
[`testing.md`](testing.md) is.** Raising a threshold to force coverage of
unreachable defensive code is explicitly not wanted; mark such code
`LCOV_EXCL_LINE` with a comment instead.

## 7. Sanitizers (required)

* **ASan** + **UBSan** (`-fno-sanitize-recover=all`) on all unit and component
  tests, Linux/Clang.
* **UBSan sub-checks** kept on: `integer-divide-by-zero`, `shift`,
  `signed-integer-overflow`, `bounds`, `alignment`, `object-size`, `vptr`.
* `-fsanitize=implicit-conversion` is **not** enabled globally (it fires on
  legitimate narrowing at protocol boundaries); the same class is covered by
  `-Wconversion` plus explicit checked casts.
* **MSan** is *not* adopted: it requires an instrumented libc++ and the library
  has no uninitialised-read surface that ASan and the fuzzers miss. Revisit only
  if a real defect escapes.
* **TSan** is *not* adopted: the core is single-threaded by contract
  ([ADR-0004](decisions/ADR-0004-threading-model.md)) and has no shared state.
  `Dispatcher` (the one concurrent component) gets a dedicated TSan job when it
  is implemented in P14.

Note that sanitizer *link* options do propagate to consumers of an instrumented
static library, and must — a consumer of an ASan-instrumented `libsmply.a` has
to link the ASan runtime. Only *compile* options are held back; that is what the
flag-leak guard in `tests/consumer/` checks.

## 8. Fuzzing (required, smoke)

*(P13.)* Every fuzz target in [`testing.md`](testing.md) §5 builds and runs
20 000 iterations over its committed seed corpus on each PR. Nightly soak is
advisory. Any crash reproducer is committed alongside its fix.

## 9. Dependency and licence hygiene (required)

* `docs/dependencies.md` lists every dependency with purpose, version, licence,
  maintenance status, public-API exposure and replaceability. CI fails if a
  `FetchContent_Declare` name is absent from that file (`tools/check_deps.py`).
* Every dependency is pinned to an exact tag **and** commit hash.
* An SPDX SBOM is generated per release build and attached to the release.
* **OSV-Scanner** runs weekly and on every dependency change against the pinned
  set; a known vulnerability opens an issue and blocks a release.
* New dependencies require an ADR (see [ADR-0011](decisions/ADR-0011-build-and-dependencies.md)).

## 10. API discipline (required)

`tools/check_public_headers.py` fails if any header under `include/smply/`:

* includes a third-party header (QCBOR, WinRT, Catch2, anything outside the
  standard library);
* mentions `winrt`, `Windows.h`, `_WIN32`-conditional API surface, `qcbor`,
  `UsefulBuf`, or `#include <windows.h>`;
* is not self-contained — the script compiles each public header alone in a
  generated translation unit, so a missing include is caught here rather than
  by the first consumer who includes it first.

Plus the `core-without-winrt` matrix job (§1), and a configure-time assertion in
`tests/consumer/` that smply's interface does not propagate its strict warning
set — checking both `INTERFACE_COMPILE_OPTIONS` **and**
`INTERFACE_LINK_LIBRARIES`, since linking `smply_internal_options` `PUBLIC`
instead of `PRIVATE` leaks the flags transitively while leaving the former
empty.

## 11. Documentation gate (required)

`tools/check_docs.py` fails a PR when:

* files under `include/smply/`, `src/smp/`, `src/dfu/` or `src/groups/` changed
  and no file under `docs/` changed, unless the PR body carries
  `Docs-Impact: none` with a one-line justification;
* a roadmap phase is marked `Complete` while its "Remaining work" section is
  non-empty;
* an ADR is referenced from a doc but does not exist, or an ADR's `Status:` is
  neither `Proposed`, `Accepted`, `Superseded by ADR-NNNN` nor `Deprecated`;
* a public symbol in `include/smply/` has no `///` documentation comment
  (namespace scope only; `detail` namespaces are exempt).

The first rule needs a diff base and a pull-request body. On a plain branch push
or a local run neither exists, so that rule is skipped with a message and the
other three still run — a branch push must never fail for a reason that cannot
apply to it.

## 12. Definition of Done

A change — feature, phase, or fix — is done only when **all** hold:

1. Implementation is complete for the stated scope; anything deferred is written
   down in the roadmap, not left implicit.
2. Tests exist and pass, **including error paths**, at the level
   [`testing.md`](testing.md) prescribes for that component.
3. All required gates above are green.
4. Every public symbol added or changed is documented in its header and in
   [`api.md`](api.md).
5. [`architecture.md`](architecture.md) and [`design.md`](design.md) still
   describe reality; anything they no longer describe correctly is updated in
   the same change.
6. Any protocol behaviour discovered, disputed or worked around is recorded in
   [`protocol-notes.md`](protocol-notes.md).
7. Significant design decisions have an ADR; a decision that contradicts an
   existing ADR **supersedes** it explicitly (never silently).
8. [`roadmap.md`](roadmap.md) status, completed/remaining work and discovered
   follow-ups are current.
9. No known contradiction between docs and code is left undocumented.
10. A handoff note is appended to [`handoff.md`](handoff.md), and the repository
    alone is sufficient for the next session — no reliance on chat history.
