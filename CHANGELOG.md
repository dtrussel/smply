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

This file starts at `0.1.0`. The development history before that release is in
git; commit `97f1647` is the last to carry the per-phase record in
`docs/roadmap.md` and `docs/handoff.md`.

## [Unreleased]

### Added

- **A reference serial port adapter**, `smply::serial_port`
  (`transports/serial_port/`). It is a `Transport` over a UART, a USB CDC ACM
  port or any tty, carrying MCUmgr's console framing. It has POSIX (`termios`)
  and Win32 (overlapped I/O) implementations behind one header, and one I/O
  thread of its own that marshals through the application's `Dispatcher`.
  `counters()` surfaces what the link discarded, including
  `LineSplitter::dropped_lines()`. It is **not installed**: a reference adapter
  like `winrt_ble`, outside the stable surface
  ([ADR-0020](docs/decisions/ADR-0020-serial-port-reference-adapter.md)).
  Tested over a pseudo-terminal. The Win32 half is compile-only, and nothing
  serial has run against a device yet.
- **`examples/serial_dfu/`**: the DFU loop over a serial port. With `--port`
  it drives a device. Without it, and on POSIX only, it runs a whole update,
  reset included, against the stub device behind a pseudo-terminal, as two
  ctests covering a UART that stays open and a USB port that vanishes and
  returns renamed (roadmap O7).
- Serial HIL cases for the bench's console UART, which have never run.
- **Several images of one device in one update**
  ([ADR-0021](docs/decisions/ADR-0021-multi-image-update.md),
  [`docs/multi-image.md`](docs/multi-image.md)).
  `FirmwareUpdater::start(std::span<const ImageTarget>, plan, callback)`
  stages every image, resets once, and confirms only after every image the
  device commits itself has been reported applied. New types: `ImageTarget`,
  `CommitBy` (`Client` or `Device`) and `ImageReport`. `UpdateReport::images`
  reports each image. `UpdatePlan::apply_timeout` and `apply_poll_interval`
  bound the wait, in the new state `UpdateState::AwaitingDeviceApply`. The
  device contract for a device-committed image is `docs/multi-image.md`.
  Tested against the simulator and the stub; nothing multi-image has run on
  hardware.
- **`smply::dfu_package`** (`support/dfu_package/`, not installed): reads the
  multi-image `dfu_application.zip` nRF Connect SDK's sysbuild writes, stored
  zips only, and checks the manifest against the images' own headers. It
  reports each image's MCUboot dependency TLVs. Hand-written, bounded, and
  fuzzed (`fuzz_dfu_package`). `smply::dfu_app::PackageUpdate` turns a package
  file into `start()`'s image list.
- `--package`, `--demo-package`, `--commit` and `--apply-fails` in `cli_dfu`
  and `serial_dfu`, and three ctests that run a two-image package end to end.

### Changed

- **`UpdateState` gains `AwaitingDeviceApply`.** An exhaustive `switch` over
  it no longer compiles until it handles the new value. `0.x` allows this
  (ADR-0016).
- **Every confirm names its image by hash.** A hashless confirm reaches only
  the device's running image, so image ≥ 1 could never have been confirmed.
  For image 0 the hash names the same slot, so single-image behaviour is
  unchanged (protocol-notes §6).
- **Every update decision is scoped to the image being updated.** Before, a
  pending swap of another image could turn a revert into "did not boot", and
  a copy of the target in another image's slot could skip an upload (roadmap
  O5, now resolved).
- `UpdateReport`'s summary fields now cover every image of the update. The
  bytes are summed, `upload_skipped` means every image was skipped, and
  `rolled_back` means any image was. For a single-image update they are
  exactly what they were.
- The examples' stub device can hold a second image it commits itself.
- The examples' stub device moved to `examples/stub_device/` and talks to a
  `DeviceLink`, so `cli_dfu` and `serial_dfu` share it. `cli_dfu` behaves as
  before.

### Documented

- Over serial, a Zephyr device accepts at most `buf_size − 4` bytes per SMP
  message, because its netbuf also holds the serial length prefix and CRC
  (protocol-notes A25). The serial adapter's default cap of 256 keeps a
  default device safe. The general fix is on the roadmap.

## [0.2.0] - 2026-09-25

A quality release. A structured review, from the architecture down to the
tests and CI, found and fixed what had drifted: a header cycle, internals in
the public namespace, an update event whose fields depended on its kind, docs
that no longer described the code, and CI gates that could pass without having
checked anything. It adds the coroutine and future adapters `architecture.md`
had long promised, and the serial console framing. **It breaks the API in the
places listed under "Changed"**, which is what `0.x` allows (ADR-0016).

### Added

- **`smply::asyncutil`: coroutines and futures over the callbacks**
  ([ADR-0019](docs/decisions/ADR-0019-async-adapters.md)). This is a
  header-only, installed target that the core does not link.
  - `smply/async/task.hpp`: `async::await_result<T>()` makes any `Callback<T>`
    operation awaitable, and `async::Task<T>` is a minimal eager coroutine type,
    so a sequence of commands reads as straight-line code on the pump thread.
  - `smply/async/future.hpp`: `async::post_for_future<T>()` starts an
    operation on the pump thread through a `Dispatcher` and returns a
    `std::future` for a caller on another thread.
  - `architecture.md` had promised this target, under this name, for a long
    time. It now exists.
- **MCUmgr's serial (console) framing, in `transports/serial/`.** A device with
  no radio now has a protocol implementation waiting for a port:
  `SerialFramer` turns one SMP message into console frames, `LineSplitter`
  turns arbitrary reads into lines and `SerialDeframer` turns lines back into
  whole SMP packets, CRC-verified. Header-only and portable; it ships under the
  existing `smply::transport_common`, spelled
  `#include "serial/serial_framing.hpp"` in this tree and out of an install
  prefix alike. **Not a transport** — opening the port, the reader thread and
  its `smply::Dispatcher` remain the application's, and nothing here has put a
  byte on a wire. [ADR-0017](docs/decisions/ADR-0017-serial-framing-placement.md)
  and [`design.md`](docs/design.md) §12.
- An eighth libFuzzer target, `fuzz_serial_deframe`, over the decoder — the
  first over a transport, and the first whose input is expected to be mostly
  noise.
- **The installed package gained `smply::transport_common`** — the portable BLE
  framing, link state, send-admission queue and SMP UUIDs. A third-party
  transport adapter can now be written against the package alone; before this
  it had to vendor or re-derive the fragmentation arithmetic and the
  send-admission queue. Installed headers keep their in-tree spelling
  (`#include "common/ble_framing.hpp"`) from
  `<prefix>/include/smply/transports`.
- `SECURITY.md`, this changelog, and an SPDX 2.3 SBOM generator
  (`tools/sbom.py`), generated from the same pins the build uses and published
  as a CI artefact.
- An advisory weekly **OSV-Scanner** workflow over the pinned dependency set,
  which §9 had also promised. It fails if the scan could not run, which is a
  different thing from finding nothing; its scheduled firing is unproven.
- `tests/consumption/` proves all three out-of-tree consumption modes —
  `find_package`, `add_subdirectory` and `FetchContent` — against one shared
  smoke program. Only `find_package` was covered before.

### Fixed

- **The firmware updater now recovers a lost mark-for-test over SMP v1.** It
  branched on `ImageError::ImageAlreadyPending`, which a server built with
  `CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL` translates away for a v1
  client — so a shipped recovery path could not fire at all on the commonest
  configuration, including the bench peer's
  ([`protocol-notes.md`](docs/protocol-notes.md) §9, A16 and A24). It now also
  accepts a group-less `SmpError::BadState`, which is what that translation
  produces for `NoFreeSlot`, `CurrentVersionIsNewer` and `ImageAlreadyPending`
  alike; all three want the same re-read-and-replan, and the existing one-shot
  budget still bounds it. Deliberately *not* extended to `SmpError::Unknown`,
  the same table's catch-all for eighteen codes including every flash failure.
  No public signature changed.
- The package no longer installs `include/smply/version.hpp.in`, the
  un-configured template, beside the generated `version.hpp`.
- The installed include root follows `CMAKE_INSTALL_INCLUDEDIR` instead of a
  hard-coded `include`.

### Changed

- **Breaking: `UpdateEvent` is a `std::variant`** of `UpdateStateChanged`,
  `UploadProgress`, `DisconnectExpected`, `ReconnectRequired`,
  `ConfirmationRequired` and `UpdateFinished`, replacing a struct with a `Kind`
  enum. In the struct, most fields were meaningful for one kind only, and
  `Finished` handed out a raw pointer valid only during the callback.
  `UpdateFinished` now holds its `Result<UpdateReport>` by value, and
  `ReconnectRequired::hint` replaces `reconnect_hint`. A `std::visit` over the
  event fails to compile when a kind is not handled. `smply::overloaded`
  combines lambdas into one visitor, and both examples show the idiom.
- **The upload's values move to `smply/groups/image_upload.hpp`**:
  `UploadOptions`, `UploadProgress`, `UploadResult`, `UploadHandle`, and a new
  `ProgressCallback` alias for the progress callback `upload()` takes.
  `groups/image.hpp` includes the new header, so no source changes.
- **`ImageManagement::resume()` returns an `UploadHandle`**, like `upload()`:
  the same handle when the upload resumes, and an invalid one when it is
  refused. Its callback is documented as what it always was, the callback for
  the resumed attempt, firing exactly once. The old comment said the callback
  given to `upload()` "fires again", which it never did.
- **Breaking: `UpdatePlan::image` is removed.** The plan carried the image
  number twice, in `UpdatePlan::image` and in `UpdatePlan::upload.image`, and
  the updater silently overwrote the second with the first. `upload.image` is
  now the one image number for the whole update: the image transferred,
  inspected after the reboot, marked and confirmed. Set `plan.upload.image`
  where you set `plan.image`.
- **`ImageHash` and `ImageVersion` are declared in `smply/mcuboot_image.hpp`**,
  no longer in `smply/groups/image.hpp`. The two headers depended on each other:
  the image group computes the upload `sha` with `sha256()`, and the MCUboot
  header used the image group's value types. The include now runs one way only,
  image group to image file. `groups/image.hpp` includes `mcuboot_image.hpp`, so
  code that includes the group header sees both types unchanged. **Breaking**
  only for code that reached the image group through `mcuboot_image.hpp`: it
  must now include `smply/groups/image.hpp` itself.
- **`protocol-notes.md` §8's UART subsection is rewritten from the server's own
  code** rather than from the transport specification, correcting two things
  that would each have produced a client no device accepts: "124" is a count of
  base64 *characters* and the payload figure is **93 bytes**, and the CRC
  covers the SMP packet but **not** the two-byte length prefix that precedes it
  in the encoded body. It also records the constraint that decides any
  frame-splitting scheme — every frame carries whole base64 quartets except the
  last — which was written down nowhere.
- `tools/coverage.sh` measures `transports/serial/` as well as
  `transports/common/`. A filter is a list of directories, so a new one is
  invisible until it is added.
- `tools/lint.sh` names the project's include roots to clang-tidy explicitly.
  The fuzz targets are absent from the compile database the `gates` job builds
  (`SMPLY_BUILD_FUZZERS` is on only in the fuzz preset), so clang-tidy had been
  *inferring* their command from a neighbouring directory — which happened to
  work until a fuzz target included a transport header.
- **The process is simpler**
  ([ADR-0018](docs/decisions/ADR-0018-maintenance-process.md)). The roadmap
  is a backlog, not a phase record, and there is no session log; the
  reasoning behind a change is in its commit message. `CONTRIBUTING.md`, a
  pull-request template and Dependabot (GitHub Actions only) are new.
- **CI no longer passes quietly when a tool is missing.** Under `CI=true`,
  `tools/lint.sh` fails without cppcheck and `tools/verify_gates.sh` fails on
  any skipped case. The coverage job uploads a real report (`coverage.xml`,
  `coverage-html/`); before this, the upload matched nothing. The nightly fuzz
  soak runs all eight targets, and the smoke job fails if one is left out.
- **Build presets:** `core-without-winrt` is removed; it configured exactly what
  `windows-msvc` does, and `windows-msvc` now sets `SMPLY_BUILD_WINRT=OFF`
  explicitly. The flag-leak check moved from `tests/consumer/` to
  `tests/interface_flags/`.
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

[Unreleased]: https://github.com/dtrussel/smply/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/dtrussel/smply/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/dtrussel/smply/releases/tag/v0.1.0
