# Quality gates

Every gate below runs in CI and blocks merge unless marked *advisory*.

**Every gate below is wired and enforced**, with one exception by design: the
`hil` row is *advisory*, and no self-hosted runner is registered, so the
hardware suite has only ever run from the bench by hand. Each live gate has been
observed rejecting a deliberate violation. `tools/verify_gates.sh` reproduces
that proof, and the `gate-self-check` CI job runs it on every push. Locally a
case whose tool is missing prints SKIP; under `CI=true`, which GitHub Actions
sets, any SKIP fails the script, so CI cannot report a gate it never exercised.

**One job still proves less than its name suggests.** `windows-winrt` *compiles*
the WinRT adapter and the example that drives it, and runs the adapter's
link-and-call smoke test; a GitHub runner has no Bluetooth radio, so no CI job
has ever put a byte of that code on the air. Read it as "it builds". The
behavioural coverage is the hardware suite, run from a bench, which found
seven things this job was never going to (protocol-notes §9,
A18–A24).

## 1. Build matrix (required)

| Job | OS | Compiler | Standard | Notes |
| --- | -- | -------- | -------- | ----- |
| `linux-gcc` | ubuntu-latest | GCC 13 | C++20 | core + tests |
| `linux-clang` | ubuntu-latest | Clang 18 | C++20 | core + tests |
| `linux-gcc-release` | ubuntu-latest | GCC 13 | C++20 | **Release**, because that is what a consumer builds. Every other preset is Debug, and some warnings appear only when optimising. |
| `linux-gcc-fallback-expected` | ubuntu-latest | GCC 13 | C++20 | forces smply's own `expected<>` even where `std::expected` exists (ADR-0002) |
| `linux-gcc-cxx23-std-expected` | ubuntu-latest | GCC 13 | **C++23** | builds the same tests against `std::expected`. Under the C++20 baseline the standard type does not exist, so without this job only smply's own backing is ever exercised and ADR-0002's interchangeability claim is untested. C++20 remains the baseline (ADR-0001); this job only proves the C++23 path works. |
| `windows-msvc` | windows-latest | MSVC v143 | C++20 | core + tests, with `SMPLY_BUILD_WINRT=OFF` set explicitly in the preset. Also compiles the serial port adapter's Win32 half and runs `smply_serial_port_tests`, whose Windows cases open no port: like `windows-winrt`, green means "it builds". This is also the **API-discipline gate**: the core must build cleanly with no WinRT anywhere in the build |
| `windows-winrt` | windows-latest | MSVC v143 | C++20 | `-DSMPLY_BUILD_WINRT=ON`: **compiles** `smply::winrt_ble` and `examples/winrt_ble_dfu`, and runs the adapter's link-and-call smoke test. A runner has no Bluetooth radio, so it never exercises GATT and never runs the example at all — green means "it builds". Behaviour is established on the bench, by `tests/hil/` and the evidence bundles it writes |
| `linux-clang-asan-ubsan` | ubuntu-latest | Clang 18 | C++20 | tests under ASan+UBSan |
| `linux-gcc-asan-ubsan` | ubuntu-latest | GCC 13 | C++20 | the same, under GCC — the two implementations do not diagnose identically, and GCC's runtime is available where Clang's `compiler-rt` package is not |
| `linux-clang-tsan` | ubuntu-latest | Clang 18 | C++20 | the suite under ThreadSanitizer; it exists for `Dispatcher` (§7) |
| `linux-clang-fuzz-smoke` | ubuntu-latest | Clang 18 | C++20 | each fuzz target, `-runs=20000` over the committed corpus |
| `linux-gcc-coverage` | ubuntu-latest | GCC 13 | C++20 | gcovr/lcov, uploads the report |
| `gates` | ubuntu-latest | Clang 18 | C++20 | format, clang-tidy, cppcheck, and the three `check_*.py` scripts |
| `install-check` | ubuntu-latest | GCC 13 | C++20 | installs to a prefix, then consumes it from three separate projects — `find_package`, `add_subdirectory` and `FetchContent` — building and **running** the same smoke program each time (§13). Also emits the SBOM and uploads it. Named `install-check` in the workflow, `out-of-tree consumption` in the UI |
| `gate-self-check` | ubuntu-latest | Clang 18 | C++20 | `tools/verify_gates.sh` — proves each gate rejects a violation |
| `nightly-fuzz-soak` | ubuntu-latest | Clang 18 | C++20 | 30 min per target, in its own scheduled workflow (*advisory*, opens an issue on a find) |
| `osv-scan` (`osv.yml`) | ubuntu-latest | — | — | OSV-Scanner over the SBOM: weekly, on demand, and on any change to what is pinned (*advisory* about findings; a scan that could not run **does** fail). Its scheduled firing is still unproven |
| `hil` (`hil.yml`) | **self-hosted** Windows runner labelled `smply-bench` | MSVC v143 | C++20 | `windows-hil` preset, then `tests/hil/run_hil.py` over the NUCLEO-WB55RG bench (`tests/hil/README.md`). `tests/hil/crosscheck.py`, the third-party comparison, is deliberately **not** in this workflow: its HCI half needs BTVS running elevated, which is a runner-configuration question that belongs with commissioning rather than with the case suite. *Advisory*, `continue-on-error`, on demand only; **no runner is registered yet**, so it has never run in CI — the suite has run from the bench by hand. Verdicts are pass / fail / **unavailable**; a missing bench is never a pass (ADR-0015) |

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

### The four directories neither analyser sees

`tools/lint.sh` excludes four directories from clang-tidy **and** cppcheck:
* `transports/winrt_ble/`, the adapter;
* `examples/winrt_ble_dfu/`, the tool that drives it;
* `tests/hil/`, the hardware suite built on both;
* `transports/serial_port/win32/`, the Win32 half of the serial port adapter
  (ADR-0020). The rest of `transports/serial_port/`, the POSIX half included,
  is analysed like any other code.

Both gates run from a Linux build, where those translation units are absent from
the compile database. clang-tidy would fall back to default arguments and fail
on the first `<winrt/…>` or `<windows.h>` include, and cppcheck can parse
neither the projection headers nor the coroutines. This is the only code in the repository outside
clang-tidy's reach. It is said plainly so that a green `gates` job does not
imply otherwise.

What still covers it: **clang-format**, which sees every file
`tools/sources.sh` lists, and **MSVC `/W4 /WX`**: in the `windows-winrt` job
for the WinRT code, and in `windows-msvc` for the serial adapter's Win32 half,
which builds without `SMPLY_BUILD_WINRT`.

Each exclusion is an exact directory prefix, and `tools/verify_gates.sh` proves
it. A portable decoy file whose *name* contains the excluded directory's name
is planted beside each excluded directory and must still be analysed. A filter
written as `grep -v winrt` would swallow the decoys and go on passing, silently
skipping files.

### The fuzz targets are analysed under an inferred command

`SMPLY_BUILD_FUZZERS` is on only in `linux-clang-fuzz`, and the `gates` job
configures `linux-clang`. So `tests/fuzz/`'s translation units are **absent
from the compile database clang-tidy is given**, like the two consumer
projects. Unlike those, they are not excluded, so clang-tidy analyses them
anyway and infers each command from a neighbouring directory.

That inferred command need not carry the include roots a target needs.
`tools/lint.sh` therefore passes the project's own include roots to clang-tidy
with `--extra-arg`. For a translation unit that *has* a real command, the
duplicate `-I` is a no-op. The script deliberately does not supply a *system*
include, so a genuinely missing one still fails. Pointing the gate at a second
build directory would have made it depend on a preset CI does not configure.

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

Unlike clang-tidy it cannot read `compile_commands.json`, so `tools/lint.sh`
passes it the include paths explicitly. That alone was not enough to make it
parse the Catch2 suites: with no macros pinned, cppcheck explores Catch2's own
option macros, and in the `CATCH_CONFIG_DISABLE;CATCH_CONFIG_PREFIX_ALL`
combination `TEST_CASE` is undefined, so the file genuinely has no valid parse.
No build uses that combination, and `-UCATCH_CONFIG_DISABLE
-UCATCH_CONFIG_PREFIX_ALL` removes exactly those configurations, so no blanket
suppression for `tests/` is needed. A full pass takes about three minutes, which is why
`SMPLY_LINT_SKIP_CPPCHECK=1` exists: `tools/verify_gates.sh` sets it for its
clang-tidy case, whose violation only the required half of the script has to
reject. Nothing in CI sets it, and under `CI=true` a missing cppcheck is an
error rather than a note. Dependency headers under `_deps/` are not
reported on, for the same reason they are declared `SYSTEM` for
clang-tidy (§1).

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

Coverage is measured on `linux-gcc-coverage`. It includes:
* `src/`;
* `include/smply/`;
* `transports/common/`;
* `transports/serial/`.

It excludes tests, examples, `support/`, and the *platform* adapters under
`transports/`.

The portable transport directories are included because they are library code
with real tests. The platform adapters are excluded because a WinRT translation
unit cannot be compiled, let alone instrumented, on the coverage runner.

**The filter is a list of directories, so a new directory is invisible until it
is added** to `tools/coverage.sh`. Its tests would still run and its lines
would still not count, so the whole-core percentage could even go *up*. A gate
that silently narrows still reports a pass.

**The metric is exactly what `tools/coverage.sh` reports**: gcovr with
`--exclude-throw-branches`. On the same object files the branch figure moves by
about 12 points depending on that one flag, because every potentially-throwing
call contributes two branches that a suite raising no exceptions can never
take. A threshold that does not name its tool and flags means whichever number
CI happens to produce.

**CI enforces it** with `tools/coverage.sh <build-dir> --enforce`, which fails
the job below either whole-core threshold. `--enforce` **refuses to run
without gcovr** rather than falling back to lcov or gcov, which measure branches
differently and so would enforce a different threshold. `tools/verify_gates.sh`
checks that the reporter rejects, accepts and refuses as it should (the
`gate-self-check` job in §1).

**The report is kept.** gcovr also writes `coverage.xml` (Cobertura) and
`coverage-html/` into the build directory, and the `coverage` job uploads both
as the `coverage-report` artifact. The upload uses `if-no-files-found: error`,
so a report that was never written fails the job.

**Deliberate invariant guards carry exclusion markers.** `LCOV_EXCL_LINE`
excludes only the line it sits on, so marking only the `if` would leave the
guard's body counted. Guards are wrapped in `LCOV_EXCL_START` /
`LCOV_EXCL_STOP` instead, except where the guard's `else` arm is the ordinary
path and must stay counted. A marker on a comment line above the code is
silently ignored.

| Gate | Threshold | Measured 2026-09-25 |
| ---- | --------- | ------------------- |
| Line coverage, whole core | **≥ 85 %** | 98.2 % ✓ |
| Branch coverage, whole core | **≥ 75 %** | 86.7 % ✓ (86.9 % before the serial port adapter; see below) |
| Branch coverage, `src/smp/`, `src/cbor/`, `src/groups/image/upload_session.*`, `src/dfu/` | **≥ 90 %** | `src/cbor/` 94.6 % ✓ · `src/smp/` 96.3 % ✓ · `upload_session.*` 91.0 % ✓ · `src/dfu/` 92.5 % ✓ |
| `transports/serial/` (no elevated gate; recorded) | — | line 100 % · branch 98.0 % |
| `transports/serial_port/` (a platform adapter: **not** in the whole-core figure; recorded) | — | line 91.7 % (289/315) · branch 78.5 % (168/214). The POSIX half and the portable files only; the Win32 half is not built here. The misses are system-call failure arms (`pipe`, `fcntl`, `tcsetattr`, `poll`) that a pseudo-terminal cannot be made to take |
| `support/dfu_package/` (support code: **not** in the whole-core figure; recorded 2026-09-25) | — | line 97.2 % (551/567) · branch 91.5 % (483/528). Among the misses: `kMaxPackageSize`, whose test would need a 64 MiB archive; a manifest over the JSON size bound inside a valid zip; and a few of the JSON reader's end-of-input arms. `fuzz_dfu_package` reaches what the table does not |
| Regression | no drop > 1 pp vs. the base branch | — |

**The elevated per-directory targets are measured, not enforced.** Only the two
whole-core thresholds are enforced. gcovr has no per-directory threshold, and
four separate invocations would turn one number into five that can disagree. A
number recorded by hand decays: treat these as "true when last measured", and
re-measure rather than quote them. A directory could fall below 90 % with CI
green. The roadmap's backlog tracks this.

**A template is counted once per instantiation.** `src/groups/common.hpp`'s
`send()`, `complete()` and `reject()` are instantiated for every result type a
group returns. A branch that no caller can take, such as an empty callback, is
then missing once per type. The same holds for `include/smply/async/task.hpp`.
Read those gaps from the list, not the percentage: each line is exercised,
just not in every instantiation.

**A new *consumer* can move the whole-core number without any regression.**
The serial port adapter is the measured example. Whole-core branch coverage
went from 86.9 % (1803/2075) to 86.7 % (1807/2083). A per-file diff against
the baseline commit shows the entire change in `detail/expected.hpp`: 182 → 190
branches, 90 → 94 taken, from the adapter's new `Result<>` instantiations.
Every other file is identical. A
caller that instantiates `Result<T>` for new types grows the counted lines of
`include/smply/detail/expected.hpp`, because an uninstantiated template counts
on neither side of the ratio. Before treating a small move as lost coverage,
read it against what was added.

The rest of `src/`, outside the elevated list (2026-09-25):

| Area | Line | Branch | Notes |
| ---- | ---- | ------ | ----- |
| `src/util/` | 100 % | 100 % | |
| `src/groups/os/` | 100 % | 94.4 % | |
| `src/core.cpp` | 99.1 % | 98.5 % | |
| `src/image/` | 98.8 % | 95.4 % | |
| `src/groups/image/` | 98.7 % | 91.0 % | Includes `upload_session.*`, which has its own elevated row above. |
| `src/groups/common.hpp` | 91.6 % | 70.4 % | Templates, counted once per result type (see above). The misses are, per instantiation: the empty-callback arms of `reject()` and `complete()`, the decode-failure arm for decoders that cannot fail (`decode_nothing`), and compiler-generated branches in the lambdas `reject()` and `send()` capture into. |
| `src/detail/` | 92.9 % | 50.0 % | Two branches in all. `client_thread.hpp`'s miss is the debug assertion firing, which no test can take without aborting. `narrow.hpp`'s is `checked_narrow`'s refusal arm as gcovr counts it; `test_narrow.cpp` checks that arm directly. |

### Reading the list, not the percentage

This is why the section's title says "with judgement". The uncovered-branch
*list* has repeatedly said something the percentage did not:
* **A gap was reachable code.** Most of the missing branches in `src/cbor/`
  were reachable straight through the API. Many were the same over-long-key
  guard repeated at every accessor. Tests for them raised the directory by
  about 15 points.
* **A gap was one branch repeated.** In `src/dfu/`, the "event not legal in
  this state" fall-through had been exercised in only one state. Looping that
  test over every state closed the gap on its own.
* **A suite can be valuable without moving the numbers.** The component suite
  exercises the same lines as the unit suite. What it adds is evidence about
  *sequences*: that the commands smply issues, in the order it issues them, are
  ones a server accepts. Line and branch coverage cannot see that.

### Traps

**Without `--enforce`, `coverage.sh` falls back to plain `gcov` when gcovr is
missing.** That is a different measurement, not comparable with the numbers
above. Run `pip install gcovr` before quoting a figure.

**`--txt` takes the next argument as its output file.** Running gcovr by hand
as `gcovr … --txt <build-dir>` fails with "Is a directory". Put the search path
first, or omit `--txt`: text is the default.

**A moved or renamed source leaves its `.gcno` behind**, and gcovr refuses to
read the orphan rather than skipping it. `coverage.sh` distinguishes gcovr's
threshold exit codes (2, 4, 6) from any other failure and says which happened.
`rm -rf` the build directory after moving a source.

**Stale `.gcda` files survive a rebuild.** Building over an existing coverage
build prints `libgcov profiling error: … overwriting an existing profile data
with a different checksum`, and then mixes counts from two versions of the
code. Delete them with `find <build-dir> -name '*.gcda' -delete` and re-run the
tests before measuring.

**The elevated targets are the point.** Those four areas are pure decision logic
over untrusted input, where a missed branch is a real untested protocol path.
Coverage elsewhere (glue, formatting, accessors) is informational. **The
percentage is not a goal. The branch table in [`testing.md`](testing.md) is.**
Raising a threshold to force coverage of unreachable defensive code is not
wanted. Mark such code with an exclusion marker and a comment instead.

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
* **TSan** is not part of the *reasoning* about the core: it is single-threaded
  by contract ([ADR-0004](decisions/ADR-0004-threading-model.md)) and has no
  shared state. It exists for `Dispatcher`, the one concurrent component smply
  ships, and the `linux-clang-tsan` job runs the whole suite under it
  anyway — it costs seconds, and a future component that quietly acquires a
  thread should fail there rather than in somebody's adapter.

  TSan and ASan cannot be combined, so this is a separate preset rather than a
  variant of `linux-clang-asan-ubsan`. It was verified to fire: removing the
  lock from `Dispatcher::post()` fails the job immediately. Note *how* it fails
  — the concurrent `push_back` corrupts the allocator, so the report is an
  `allocation-size-too-big` rather than a clean `data race`. A TSan finding that
  does not say "data race" is still a TSan finding.

Note that sanitizer *link* options do propagate to consumers of an instrumented
static library, and must — a consumer of an ASan-instrumented `libsmply.a` has
to link the ASan runtime. Only *compile* options are held back; that is what the
flag-leak guard in `tests/interface_flags/` checks.

## 8. Fuzzing (required, smoke)

Every fuzz target in [`testing.md`](testing.md) §5 builds and runs 20 000
iterations over its committed corpus on each push and pull request
(`linux-clang-fuzz-smoke`). That job is blocking, and it is a **regression**
gate, not a search: a crash there means a reproducer that was fixed once has
come back, or a change has broken a property one of the targets asserts.

The search is the scheduled `nightly-fuzz-soak` workflow, 30 minutes per target,
which is advisory — it uploads the grown corpus and any reproducer, and opens
one issue per target on a find. Its matrix names the targets by hand, so the
smoke job checks that every target it built appears there; a new target that
was left out of the soak fails the smoke job. Any crash reproducer is committed to
`tests/fuzz/corpus/<target>/` **alongside its fix**, which is what moves it from
the advisory job into the blocking one.

Neither job writes to the committed corpus: both copy it out of the tree first,
because libFuzzer writes what it discovers into the directory it is given and
what the repository carries is a decision for a person.

## 9. Dependency and licence hygiene (required)

* `docs/dependencies.md` lists every dependency with purpose, version, licence,
  maintenance status, public-API exposure and replaceability. CI fails if a
  `FetchContent_Declare` name is absent from that file (`tools/check_deps.py`).
* Every dependency is pinned to an exact tag **and** commit hash.
* **A declaration of smply itself is skipped**, by exact name. Proving smply can
  be consumed by FetchContent means writing a `FetchContent_Declare(smply …)`
  for it (§13), and nothing is a third-party dependency of itself — the gate's
  scope is third-party. The fixture's declaration is deliberately a
  `SOURCE_DIR` with no `GIT_TAG`, so without the exemption it would fail. The
  exemption is an identity and not a prefix: a `FetchContent_Declare(smply_x …)`
  is an ordinary dependency, and `verify_gates.sh` plants one to prove it is
  still rejected — the same shape as §3's "directories, not the substring
  `winrt`" case.
* **An SPDX 2.3 SBOM** is generated by `tools/sbom.py` from the pins in
  `cmake/dependencies.cmake` — the same ones the build uses, so it cannot
  describe a different dependency set than the one that shipped — and is
  uploaded as an artefact by the `out-of-tree consumption` job, which is the
  job that already produces a Release install. `tools/sbom.py --check` runs in
  the `gates` job and **fails when a `FetchContent_Declare` has no licence
  entry or no PURL**. Licences are declared in `sbom.py`, never inferred.
  The PURL is required because a scanner matches advisories on a package
  *identifier*, not a name. An SBOM without PURLs parses perfectly and reports
  "found 0 packages" in any scanner that reads it.
  Each dependency carries `pkg:github/<owner>/<repo>@<commit>` — the commit,
  because that is what the build actually pins.
* **OSV-Scanner** (`.github/workflows/osv.yml`) runs weekly, on
  `workflow_dispatch`, and on any change to what is pinned, scanning the SBOM.
  It is **advisory**: the result is a SARIF artefact, and acting on a finding
  is a decision — ADR-0011 says a dependency change needs one. It does not
  block a pull request, and it does not open an issue.
  `osv.yml` is in its own `paths` filter, so a change to it tests itself. Two
  ways it can fail while looking like success:
  * **It scans nothing.** The action runs in a container that mounts only the
    workspace, so the SBOM must be written inside the workspace.
    `continue-on-error` would otherwise swallow the exit 127.
  * **It finds 0 packages.** That is an SBOM without PURLs, as above.

  The job **fails when no SARIF was produced**. This is the same discipline
  `hci_capture.py` applies to a packet capture: a listener that attaches and
  records nothing is not a capture.

  Two things are still unproven and should not be read as passing. The
  **scheduled** firing, which has not yet been observed. And whether
  OSV has advisory **coverage** for two C libraries consumed from git as
  `pkg:github` PURLs: a clean report from a database with nothing to say about
  an ecosystem looks exactly like a clean report from one that checked. The
  report is evidence that the scan ran, not yet that it would find something.
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
* includes a public header of the same or a higher **layer**. The layers are
  the dependency diagram of [`architecture.md`](architecture.md) §3: core types,
  then the header codec, transport contract and image source, then the MCUboot
  image header, then `smp_client.hpp`, then the groups, then the updater. The
  core headers may include one another. A header the table does not list is
  rejected, so a new header gets a layer deliberately.

It also fails if a directory under `src/` includes an internal directory it may
not use, or a public header above its layer ceiling. For example, `src/image/`
may include neither `cbor/` nor `smp_client.hpp`, so parsing a firmware file
can never come to depend on the SMP client. Both tables are at the top of the
script, and `verify_gates.sh` proves each rule fires.

Plus the `windows-msvc` matrix job, which builds the core with WinRT off (§1),
and a configure-time assertion in `tests/interface_flags/` that smply's
interface does not propagate its strict warning set — checking both
`INTERFACE_COMPILE_OPTIONS` **and** `INTERFACE_LINK_LIBRARIES`, since linking
`smply_internal_options` `PUBLIC` instead of `PRIVATE` leaks the flags
transitively while leaving the former empty.

## 11. Documentation gate (required)

`tools/check_docs.py` fails when any of the following rules is broken.

* **R1.** Files under `include/smply/`, `src/smp/`, `src/dfu/` or
  `src/groups/` changed, but no file under `docs/` did. The PR body can waive
  this with `Docs-Impact: none` and a one-line justification.
* **R2.** The roadmap is not a backlog
  ([ADR-0018](decisions/ADR-0018-maintenance-process.md)), in any of these ways:
  * one of its required sections is missing;
  * a row is struck through, when a done item should be deleted;
  * a document cites an open question (`O<n>`) that the roadmap's table does
    not define.
* **R3.** An ADR is referenced but does not exist, an ADR's `Status:` is not
  `Proposed`, `Accepted`, `Superseded by ADR-NNNN` or `Deprecated`, or an ADR
  is missing from the index in `decisions/README.md`.
* **R4.** A public symbol in `include/smply/` has no `///` documentation
  comment. This covers namespace scope only; `detail` namespaces are exempt.
* **R5.** A path named in the repository-layout tree of
  [`architecture.md`](architecture.md) §10 does not exist.
* **R6.** A living document, or a source, build, CI or tool file, names a
  development phase (`P<n>`). The library was built in numbered phases, and
  that history is in git, not in the documents or the comments (ADR-0018). ADR
  bodies are exempt, because they are immutable records of their day. So are
  fuzz corpora, which are data.

R1 needs a diff base and a pull-request body. A plain branch push or a local run
has neither, so R1 is skipped with a message and the other rules still run. A
branch push must never fail for a reason that cannot apply to it.

**R5 is deliberately conservative and reports how much it skipped.** The tree
is prose. An entry the rule cannot read as a path is skipped rather than
guessed at: a brace expansion, a glob, or a phrase that is not a file name. The
printed count is what would reveal a rule that had quietly narrowed to checking
nothing. R5 reads every path-shaped token on an entry line, and on the
continuation lines under it.
* **An unresolvable token in first position is an error**, because that is what
  the line is about.
* **A later token is an error only if it has a file extension the tree uses.**
  Otherwise the rule would trip over "run_hil.py supervises the case suite",
  which is a sentence, not a listing.

`verify_gates.sh` plants a decoy for each case: an entry that does not exist,
and a missing file named in second position.

**These rules catch the shapes of drift, not wrong claims.** No gate notices a
document describing a fixed defect as if it were the design. That takes a
person reading the document against the code.

## 12. Definition of Done

A change, whether a feature or a fix, is done only when **all** of these hold:

1. Implementation is complete for the stated scope. Anything deferred is
   written down in the roadmap's backlog, not left implicit.
2. Tests exist and pass, **including error paths**, at the level
   [`testing.md`](testing.md) prescribes for that component.
3. All required gates above are green.
4. Every public symbol added or changed is documented in its header and in
   [`api.md`](api.md).
5. [`architecture.md`](architecture.md) and [`design.md`](design.md) still
   describe reality. Anything they no longer describe correctly is updated in
   the same change.
6. Any protocol behaviour discovered, disputed or worked around is recorded in
   [`protocol-notes.md`](protocol-notes.md).
7. Significant design decisions have an ADR. A decision that contradicts an
   existing ADR **supersedes** it explicitly, never silently.
8. [`roadmap.md`](roadmap.md) is current. Items this change finished are
   deleted, and anything it found is added.
9. No known contradiction between docs and code is left undocumented.
10. The repository alone is enough for the next contributor, with no reliance
    on chat history. A lesson that will bite again goes into
    [`handoff.md`](handoff.md)'s standing caveats.

## 13. Out-of-tree consumption (required)

`tools/check_install.sh`
builds smply in **Release**, installs it into a throwaway prefix, and then
builds and **runs** a consumer **three ways**:

| Mode | Consumes | What only this mode can catch |
| ---- | -------- | ----------------------------- |
| `find_package` | the installed prefix | the **export** — a wrong exported target name, a missing archive, a header left out of the package |
| `add_subdirectory` | the source tree | smply misbehaving as a **subproject**: tests or examples defaulting ON, the strict warning set reaching the parent |
| `FetchContent` | the source tree, declared | that smply can be a FetchContent dependency at all, populated into a build directory it does not name |

All three compile the **same** program, `tests/consumption/smoke.cpp`. One
program on purpose: three would drift, and the one that drifted would be the
mode nobody noticed had stopped checking anything.

Every part of the arrangement is load-bearing:

* **Separate projects.** Anything built inside our own tree consumes the
  build-tree targets, where the in-tree `ALIAS` names resolve. It proves nothing
  about the export. The first run of this check caught exactly that:
  `install(EXPORT)` names an exported target `<namespace><target-name>`, so
  `smply_util` shipped as `smply::smply_util` while every consumer in the
  repository says `smply::util`. Fixed with `EXPORT_NAME`; invisible without
  this gate, and the reason `smply::transport_common` got an `EXPORT_NAME` the
  day it was added.
* **Running it.** A package that exports the headers but forgets the archive can
  still configure and build a program that never calls into it. The smoke
  program calls a symbol from each installed target, and compares
  `smply::version()` (from the archive) against `SMPLY_VERSION_STRING` (from
  the header), so a prefix mixing two installs fails.
* **Release.** It is what a consumer builds, and see §1.
* **`FetchContent` with `SOURCE_DIR`, not `GIT_REPOSITORY`.** A git fetch would
  populate the last *commit*, so the check would pass or fail on a tree nobody
  has — the stale-binary trap of §1 wearing different clothes. Whether CMake can
  clone is CMake's problem; whether *this* tree can be consumed is ours.
* **The `.in` check.** `include/smply/version.hpp.in` sits beside the public
  headers, and an unfiltered `install(DIRECTORY)` shipped it next to the
  configured `version.hpp`. The script fails if it reappears.

What the package contains, and why each excluded target is excluded, is
[ADR-0016](decisions/ADR-0016-installed-package-and-versioning.md):
`smply::smply`, `smply::util`, `smply::transport_common` and
`smply::asyncutil` ship;
`smply::minicbor`, `smply::dfu_app`, `smply::dfu_package` (ADR-0021),
`smply::winrt_ble` and the test doubles do not. The installed prefix does also contain QCBOR's headers and config package —
QCBOR's own install rules run alongside ours — which is a packaging fact, not an
API leak: ADR-0007 keeps QCBOR out of `include/smply/` and §10 still enforces it.

`verify_gates.sh` stages the negative arm: with `smply::transport_common`
removed from the export set, the `find_package` consumer must fail to
configure. The positive arm is this gate itself, which runs on every push.
