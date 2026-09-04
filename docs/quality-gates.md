# Quality gates

Every gate below runs in CI and blocks merge unless marked *advisory*.
Configuration files are created in roadmap phase **P0**; their intended content
is given here so P0 is mechanical.

## 1. Build matrix (required)

| Job | OS | Compiler | Standard | Notes |
| --- | -- | -------- | -------- | ----- |
| `linux-gcc` | ubuntu-latest | GCC 13 | C++20 | core + tests |
| `linux-clang` | ubuntu-latest | Clang 17 | C++20 | core + tests |
| `windows-msvc` | windows-latest | MSVC v143 | C++20 | core + tests |
| `windows-winrt` | windows-latest | MSVC v143 | C++20 | `-DSMPLY_BUILD_WINRT=ON`: adapter + example |
| `core-without-winrt` | windows-latest | MSVC v143 | C++20 | **API-discipline gate**: `-DSMPLY_BUILD_WINRT=OFF` must build cleanly |
| `linux-clang-asan-ubsan` | ubuntu-latest | Clang 17 | C++20 | tests under ASan+UBSan |
| `linux-clang-fuzz-smoke` | ubuntu-latest | Clang 17 | C++20 | each fuzz target, `-runs=20000` over the seed corpus |
| `linux-gcc-coverage` | ubuntu-latest | GCC 13 | C++20 | gcov/lcov, uploads the report |
| `nightly-fuzz-soak` | ubuntu-latest | Clang 17 | C++20 | 30 min per target (*advisory*, opens an issue on a find) |

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
inherent here.

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

| Gate | Threshold |
| ---- | --------- |
| Line coverage, whole core | **≥ 85 %** |
| Branch coverage, whole core | **≥ 75 %** |
| Branch coverage, `src/smp/`, `src/cbor/`, `src/groups/image/upload_session.*`, `src/dfu/update_state_machine.*` | **≥ 90 %** |
| Regression | no drop > 1 pp vs. the base branch |

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
  is implemented in P9.

## 8. Fuzzing (required, smoke)

Every fuzz target in [`testing.md`](testing.md) §5 builds and runs 20 000
iterations over its committed seed corpus on each PR. Nightly soak is advisory.
Any crash reproducer is committed alongside its fix.

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
* is not self-contained (each public header is compiled standalone in a
  generated TU as part of the build).

Plus the `core-without-winrt` matrix job (§1).

## 11. Documentation gate (required)

`tools/check_docs.py` fails a PR when:

* files under `include/smply/`, `src/smp/`, `src/dfu/` or `src/groups/` changed
  and no file under `docs/` changed, unless the PR body carries
  `Docs-Impact: none` with a one-line justification;
* a roadmap phase is marked `Complete` while its "Remaining work" section is
  non-empty;
* an ADR is referenced from a doc but does not exist, or an ADR's `Status:` is
  neither `Proposed`, `Accepted`, `Superseded by ADR-NNNN` nor `Deprecated`;
* a public symbol in `include/smply/` has no `///` documentation comment.

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
