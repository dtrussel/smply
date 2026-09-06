# Dependency inventory

Philosophy: **as few as possible, permissively licensed, none in the public
API.** Every dependency needs a row here and an ADR if it is non-obvious
([ADR-0011](decisions/ADR-0011-build-and-dependencies.md)). CI fails if a
declared dependency is missing from this table
([`quality-gates.md`](quality-gates.md) §9).

## Runtime (linked into `smply::smply`)

| Name | Purpose | Licence | Maintenance | In public API? | Replaceable? |
| ---- | ------- | ------- | ----------- | -------------- | ------------ |
| **QCBOR** `v1.6.1` (`930708bb86481e88879eb1d87fd4d664f1d69503`) | CBOR encode/decode | BSD-3-Clause | Actively maintained (Laurence Lundblade); used in IETF/IoT stacks | **No** — hidden behind `smply::cbor::Reader/Writer` | Yes — `src/cbor/backend_qcbor.*` is the only file that names it. TinyCBOR or zcbor could replace it behind the same façade. See [ADR-0007](decisions/ADR-0007-cbor-library.md). |
| **SHA-256** (`src/image/sha256.{hpp,cpp}`) | SHA-256 for the MCUmgr upload `sha` field | Apache-2.0 — **smply's own code**, not a third-party component | ~150 lines of FIPS 180-4, written for this project rather than depending on a crypto library; correctness pinned by the NIST vectors in `tests/unit/test_sha256.cpp` | No | Trivially — swap for a platform API if one is ever preferred. See [ADR-0009](decisions/ADR-0009-mcuboot-boundary.md). |

That is the complete runtime footprint: **one third-party library**, plus one
primitive smply implements itself. No Boost, no fmt, no JSON library, no
OpenSSL, no async framework.

The SHA-256 row is listed here even though it is not a dependency, because the
question "where does smply get its crypto?" has to have an answer in this file.
Being first-party rather than vendored is deliberate: it keeps every source file
under the project's own SPDX identifier and keeps `NOTICE` free of an
attribution entry for 150 lines of a published standard. P9 recorded the change;
[ADR-0009](decisions/ADR-0009-mcuboot-boundary.md)'s decision — no crypto
library dependency — is unaffected.

## Build and test only (never shipped)

| Name | Purpose | Licence | In public API? |
| ---- | ------- | ------- | -------------- |
| **Catch2** `v3.9.1` (`dfc2dff8d70d083c60c1c6986030e5389a867a93`) | unit/component test framework | BSL-1.0 | No |
| **CMake ≥ 3.25** | build system (`FetchContent` + `OVERRIDE_FIND_PACKAGE` needs 3.24; `SYSTEM` needs 3.25) | BSD-3-Clause | n/a |
| **clang-format / clang-tidy** | formatting, static analysis | Apache-2.0 WITH LLVM-exception | n/a |
| **cppcheck** | complementary static analysis | GPL-3.0 (*tool only, never linked or copied from*) | n/a |
| **libFuzzer** | fuzzing (part of Clang) | Apache-2.0 WITH LLVM-exception | n/a |
| **gcovr / lcov / gcov** | coverage (`tools/coverage.sh` uses whichever is present) | Apache-2.0 / GPL-2.0 (*tools only*) | n/a |
| **OSV-Scanner** | vulnerability monitoring | Apache-2.0 | n/a |

Note on GPL tools: cppcheck and lcov are *executed*, never linked, and no code
is copied from them. This keeps the shipped artefact free of copyleft
obligations, which matters because smply is intended for use in a proprietary
Windows application. The same rule applies to reference implementations: no code
is copied from GPL or otherwise incompatible MCUmgr clients — they are consulted
for behavioural comparison only ([`protocol-notes.md`](protocol-notes.md) §1).

## Platform (adapter targets only)

| Name | Purpose | Licence | Target |
| ---- | ------- | ------- | ------ |
| **C++/WinRT** (Windows SDK) | BLE GATT | Microsoft Windows SDK licence | `smply::winrt_ble`, `examples/winrt_ble_dfu` only |
| **Threads** (`Threads::Threads`, i.e. pthreads on Linux) | `std::mutex` in `Dispatcher` | part of the platform's C library | `smply::util` only |

Never linked by `smply::smply`; enforced for WinRT by the `core-without-winrt`
CI job, and for Threads by the fact that `smply::util` is a separate target the
core does not link (`architecture.md` §5).

Neither is a `FetchContent` dependency, so neither is covered by
`tools/check_deps.py` — that gate reads `FetchContent_Declare` names. They are
listed here because this file is the inventory of *everything smply links*, not
only of what it downloads.

## Acquisition and pinning

Dependencies are fetched with CMake `FetchContent` pinned to an exact tag **and**
commit hash, with `OVERRIDE_FIND_PACKAGE` so a distribution- or vcpkg-provided
copy is used when present. `tools/check_deps.py` fails the build if a declared
dependency is missing from this file **or** is pinned to anything other than a
full 40-character commit hash — a tag can be moved, a hash cannot.

QCBOR's pin was exercised for the first time in P5 and its spiffy-decode API
behaved as [ADR-0007](decisions/ADR-0007-cbor-library.md) assumed: map-key
getters, a sticky error, and a distinguishable "label not found" that makes
absent-versus-malformed separable. No fallback to TinyCBOR was needed.

Each declaration also passes `SYSTEM`, so dependency headers are system includes.
Without it, findings from inside Catch2's and QCBOR's headers are reported
against smply's own files (macro expansion attributes them to the expansion
site), which made clang-tidy unusable. `SMPLY_USE_SYSTEM_QCBOR=ON` forces `find_package`.
Nothing is vendored under `third_party/` unless an upstream becomes unavailable;
if that happens it requires an ADR.

## Licence of smply itself

**Apache-2.0** (decided in P0, resolving roadmap open question O1). Permissive,
compatible with linking into proprietary Windows applications and with every
licence above, and it carries an explicit patent grant — worth having for a
protocol implementation. See `LICENSE` and `NOTICE`.

Every source file carries `// SPDX-License-Identifier: Apache-2.0`.
