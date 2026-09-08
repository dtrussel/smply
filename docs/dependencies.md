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

## Hardware bench only (P17; never linked or shipped)

[ADR-0015](decisions/ADR-0015-hardware-evidence.md) records this boundary: none
of these is a dependency of `libsmply` or of anything it installs, and none of
the third-party clients is a protocol reference. Exact versions and hashes of
every run's inputs are kept in the bench's evidence bundle; the pins that make a
run reproducible are in `tests/hil/firmware/` and `tests/hil/README.md`.

| Tool | Pin | Purpose / licence |
| --- | --- | --- |
| Zephyr | `e71ff182603865f59e2e25f05655d6affda4f288` (4.4.99) | The reference peer (`smp_svr`) and the protocol's primary source; Apache-2.0 plus module licences |
| MCUboot | `ee39e2d694bd827ffd1bebbce2f571a9154e6ec2` | Bootloader and signing script; Apache-2.0 |
| hal_stm32 | `d1d3c0c9ddf697f6bcda911f158777136aa21c5c` | STM32WB55 support, including the IPCC HCI driver; BSD-3-Clause |
| STM32CubeWB coprocessor binaries | v1.24.0 (`stm32wb5x_BLE_HCILayer_fw.bin`, FUS 2.2.0) | The Bluetooth controller firmware Zephyr needs on the WB55's Cortex-M0+; ST SLA0044, bench board only |
| Zephyr SDK | 1.0.1 (GCC 14.3.0) | Firmware compiler; toolchain component licences, including GPL |
| west | 1.5.0 | The pinned firmware workspace; Apache-2.0 |
| STM32CubeProgrammer | 2.22.0 | ST-LINK flashing, CPU2 (FUS) provisioning, hard reset -- the bench's recovery primitive; ST SLA0048 |
| pyserial / bleak | 3.5 / 3.0.2 | UART capture and the reset-window stopwatch; BSD / MIT |
| imgtool | 2.4.0 | Present in the venv for the west build; the images are signed by Zephyr's own build step; Apache-2.0 |
| mcumgr-client | 0.0.9 | UART (shell transport) image-state oracle and behavioural comparison only; Apache-2.0 |
| smpmgr / smpclient / smp | 0.19.1 / 7.3.0 / 4.1.0 | BLE behavioural comparison only, and the transport of the reset stopwatch; Apache-2.0 |
| Microsoft BTP/BTVS | 1.14.0 | HCI capture (requires an elevated shell); Microsoft tool licence |
| Wireshark/tshark | 4.6.6 | HCI decoding; GPL-2.0-or-later, external tool only |

### A defect in QCBOR, worked around rather than pinned past

P17a found that QCBOR (the pinned 1.6.1 **and** `master` at `65fd7cb4`,
2026-09-04) mishandles two consecutive indefinite-length breaks when a map is
left with `QCBORDecode_ExitMap()`: the enclosing array's break is consumed
along with the map's, after which the walk either fails with `BAD_BREAK` or
silently reads the parent map's next entries as array elements. Zephyr's zcbor
emits exactly that encoding (protocol-notes §9, A18). Since no upstream version
fixes it, `cbor::Reader::for_each_map_in_array` no longer enters and exits
elements in place; it peeks, skips with `QCBORDecode_VGetNextConsume()` and
decodes each element from its own byte range in a child reader
(`src/cbor/reader.cpp`). The pin is unchanged and ADR-0007 stands. Reporting it
upstream is filed as follow-up work.

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
