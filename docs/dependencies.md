# Dependency inventory

Philosophy: **as few as possible, permissively licensed, none in the public
API.** Every dependency needs a row here and an ADR if it is non-obvious
([ADR-0011](decisions/ADR-0011-build-and-dependencies.md)). CI fails if a
declared dependency is missing from this table
([`quality-gates.md`](quality-gates.md) §9).

## Runtime (linked into `smply::smply`)

| Name | Purpose | Licence | Maintenance | In public API? | Replaceable? |
| ---- | ------- | ------- | ----------- | -------------- | ------------ |
| **QCBOR** | CBOR encode/decode | BSD-3-Clause | Actively maintained (Laurence Lundblade); used in IETF/IoT stacks | **No** — hidden behind `smply::cbor::Reader/Writer` | Yes — `src/cbor/backend_qcbor.*` is the only file that names it. TinyCBOR or zcbor could replace it behind the same façade. See [ADR-0007](decisions/ADR-0007-cbor-library.md). |
| **SHA-256** (vendored, `src/image/sha256.cpp`) | SHA-256 for the MCUmgr upload `sha` field | Public domain / CC0 | ~150 lines, vendored deliberately rather than depending on a crypto library | No | Trivially — swap for a platform API if one is ever preferred. See [ADR-0009](decisions/ADR-0009-mcuboot-boundary.md). |

That is the complete runtime footprint: **one third-party library and one
vendored primitive.** No Boost, no fmt, no JSON library, no OpenSSL, no async
framework.

## Build and test only (never shipped)

| Name | Purpose | Licence | In public API? |
| ---- | ------- | ------- | -------------- |
| **Catch2 v3** | unit/component test framework | BSL-1.0 | No |
| **CMake ≥ 3.24** | build system (`FetchContent` + `OVERRIDE_FIND_PACKAGE`) | BSD-3-Clause | n/a |
| **clang-format / clang-tidy** | formatting, static analysis | Apache-2.0 WITH LLVM-exception | n/a |
| **cppcheck** | complementary static analysis | GPL-3.0 (*tool only, never linked or copied from*) | n/a |
| **libFuzzer** | fuzzing (part of Clang) | Apache-2.0 WITH LLVM-exception | n/a |
| **lcov / gcov** | coverage | GPL-2.0 (*tool only*) | n/a |
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

Never linked by `smply::smply`; enforced by the `core-without-winrt` CI job.

## Acquisition and pinning

Dependencies are fetched with CMake `FetchContent` pinned to an exact tag **and**
commit hash, with `OVERRIDE_FIND_PACKAGE` so a distribution- or vcpkg-provided
copy is used when present. `SMPLY_USE_SYSTEM_QCBOR=ON` forces `find_package`.
Nothing is vendored under `third_party/` unless an upstream becomes unavailable;
if that happens it requires an ADR.

## Licence of smply itself

To be decided in P0. Intent: a permissive licence (Apache-2.0 or MIT) compatible
with linking into proprietary Windows applications and with all of the above.
