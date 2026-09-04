# ADR-0011 — Target-based CMake, FetchContent, exact pinning

**Status:** Accepted (2026-09-04)

## Context

smply is consumed by a Windows application that has its own build, and is also
built standalone on Linux for tests, sanitizers, fuzzing and coverage. A library
that leaks compiler flags, include paths or dependency policy into its consumers
is a nuisance; one that cannot be built reproducibly in CI is worse.

## Decision

**Modern target-based CMake, ≥ 3.24.**

* No `include_directories()`, no `link_libraries()`, no global
  `add_compile_options()`, no `CMAKE_CXX_FLAGS` mutation.
* Usage requirements are on targets:
  `target_include_directories(smply PUBLIC $<BUILD_INTERFACE:...>
  $<INSTALL_INTERFACE:include>)`.
* Warnings and sanitizers live on `smply_internal_options`, an `INTERFACE`
  target linked **`PRIVATE`** by smply's own targets — consumers never inherit
  `-Werror` ([`../quality-gates.md`](../quality-gates.md) §2).
* Options: `SMPLY_BUILD_TESTS`, `SMPLY_BUILD_FUZZERS`, `SMPLY_BUILD_EXAMPLES`,
  `SMPLY_BUILD_WINRT`, `SMPLY_BUILD_HIL`, `SMPLY_USE_SYSTEM_QCBOR` — all
  defaulting `OFF` when smply is a subproject (detected via
  `PROJECT_IS_TOP_LEVEL`), so being `add_subdirectory()`'d is quiet and cheap.
* Namespaced aliases from the start: `smply::smply`, `smply::winrt_ble`.
* Install/export produces `smplyConfig.cmake` + `smplyConfigVersion.cmake` so
  `find_package(smply)` works, with `smply::smply` importable.
* `CMakePresets.json` defines exactly the configurations CI runs, so a
  contributor reproduces a CI failure with one command.
* Tests reach internals through `smply::smply_internal`, an INTERFACE target
  adding `src/` to the include path. Production consumers cannot.

**Dependencies via `FetchContent`**, pinned to an exact tag **and** commit hash,
declared with `OVERRIDE_FIND_PACKAGE` so a system/vcpkg copy is used when
present. `SMPLY_USE_SYSTEM_QCBOR=ON` forces `find_package`. `third_party/` stays
empty unless an upstream disappears — vendoring requires a new ADR.

## Alternatives considered

**Git submodules.** Reliable pinning, but they burden every consumer with
`--recursive`, break shallow clones and archive downloads, and interact badly
with vcpkg-style consumption. Rejected.

**Vendoring sources into `third_party/`.** Maximum reproducibility and offline
builds. Costs manual update work, hides provenance from vulnerability scanners,
and inflates the repository. Rejected as the default; kept as the documented
escape hatch.

**A package manager (vcpkg / Conan) as the required path.** Excellent for a
Windows application team, but forcing one on every consumer and on CI is
heavier than the two dependencies justify. `OVERRIDE_FIND_PACKAGE` means a vcpkg
user gets their own QCBOR and Catch2 for free without smply mandating anything.
Rejected as a requirement, supported as an option.

**Header-only library.** Would remove build integration questions entirely.
Rejected: it forces every implementation detail into public headers,
contradicting the public/internal split in
[`../architecture.md`](../architecture.md) §3, slows consumer builds, and makes
the fuzzers and coverage instrumentation awkward.

## Consequences

* A consumer writes `find_package(smply)` + `target_link_libraries(app PRIVATE
  smply::smply)` and inherits include paths and nothing else.
* CI reproducibility comes from the pins; supply-chain visibility from the
  hashes plus the OSV scan and the SBOM
  ([`../quality-gates.md`](../quality-gates.md) §9).
* Adding a dependency means: an ADR, a row in
  [`../dependencies.md`](../dependencies.md), a pin, and a licence check —
  deliberate friction.
* The WinRT adapter is a separate target guarded by `SMPLY_BUILD_WINRT`, and a
  CI job builds with it **off** on Windows to prove the core is independent of
  it.
