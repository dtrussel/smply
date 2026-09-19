<!-- SPDX-License-Identifier: Apache-2.0 -->

# Changelog

All notable changes to smply are recorded here, in the format of
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

smply follows [Semantic Versioning](https://semver.org/). **The stable surface
is `include/smply/` plus the installed transport headers**; everything under
`support/`, `tests/`, `examples/` and `transports/winrt_ble/` is outside it and
may change in any release. While the version is `0.x`, a minor bump may break
even the stable surface — that is what `0.x` means.
[ADR-0016](docs/decisions/ADR-0016-installed-package-and-versioning.md) is the
full policy.

This file starts at `0.1.0`. Entries before that release are grouped by the
development phase that produced them, because that is how the work is recorded
in [`docs/roadmap.md`](docs/roadmap.md), which remains the detailed history.

## [Unreleased]

### Added

- **The installed package gained `smply::transport_common`** — the portable BLE
  framing, link state, send-admission queue and SMP UUIDs. A third-party
  transport adapter can now be written against the package alone; before this
  it had to vendor or re-derive the fragmentation arithmetic and the
  send-admission queue. Installed headers keep their in-tree spelling
  (`#include "common/ble_framing.hpp"`) from
  `<prefix>/include/smply/transports`.
- `SECURITY.md`, this changelog, and an SPDX 2.3 SBOM generator
  (`tools/sbom.py`), generated from the same pins the build uses and published
  as a CI artefact. `quality-gates.md` §9 had promised an SBOM since P0.
- An advisory weekly **OSV-Scanner** workflow over the pinned dependency set,
  which §9 had also promised. It fails if the scan could not run, which is a
  different thing from finding nothing; its scheduled firing is unproven.
- `tests/consumption/` proves all three out-of-tree consumption modes —
  `find_package`, `add_subdirectory` and `FetchContent` — against one shared
  smoke program. Only `find_package` was covered before.

### Fixed

- The package no longer installs `include/smply/version.hpp.in`, the
  un-configured template, beside the generated `version.hpp`.
- The installed include root follows `CMAKE_INSTALL_INCLUDEDIR` instead of a
  hard-coded `include`.

### Changed

- `.github/workflows/hil.yml` no longer carries a nightly `schedule:`. No
  self-hosted runner is registered, so it was queueing a 90-minute timeout
  every night against nothing. `workflow_dispatch` remains, and the file
  documents how to commission a runner and restore the schedule.

## [0.1.0]

The portable library, complete, installable, and run against a device.

### Added

- **SMP protocol core** — the 8-byte header codec, streaming message
  reassembly within hard bounds, and a bounded non-allocating CBOR façade over
  QCBOR that never lets a device-supplied size reach an allocation.
- **`SmpClient`** — sequence allocation, request correlation on the full
  `(seq, group, command, op)` tuple, deadlines, cancellation, disconnect and
  transport rebinding. Callbacks are always deferred, never delivered inside
  the call that started the operation.
- **Management groups** — OS (reset, MCUmgr parameters, echo) and the whole of
  image (state read and write, erase, slot info, and upload).
- **MCUboot image handling** — header parsing, SHA-256 over a file, and the
  `IMAGE_TLV_SHA` trailer scan, kept distinct from the upload `sha` because
  conflating the two hashes is the classic client bug.
- **`FirmwareUpdater`** — the end-to-end update including the reset, the
  reconnect, the trial boot and confirmation.
- **`smply::util`** (`Dispatcher`), the thread-marshalling helper an adapter
  needs, shipped as a separate target that the core never links.
- **`smply::transport_common`**, the portable BLE helpers, and
  **`smply::winrt_ble`**, the reference Windows C++/WinRT BLE adapter.
- **Examples** — `cli_dfu`, which performs a whole update against a stub device
  on another thread and runs in CI, and `winrt_ble_dfu`, which does it over a
  real radio.
- **Install/export** with `find_package(smply)` support.
- Seven libFuzzer targets, enforced coverage thresholds, and a 16-job CI gate.

### Verified on hardware

Updates in both directions against a NUCLEO-WB55RG running Zephyr's `smp_svr`,
confirmed over a UART path the client does not touch, plus a cross-check
against a third-party client agreeing at three checkpoints including the trial
boot. Seven protocol findings came out of it, every one contradicting something
the simulated suite had accepted — among them that a real device's CBOR is
indefinite-length, and that the final upload chunk is answered only after the
device has hashed the whole image, which is why `UploadOptions` carries a
separate `final_chunk_timeout`.

[Unreleased]: https://github.com/dtrussel/smply/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/dtrussel/smply/releases/tag/v0.1.0
