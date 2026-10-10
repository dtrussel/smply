# Roadmap and backlog

This file is the project's open work: what is in progress, what is undecided,
and what is known but not yet worth doing. It is **not** a history. An item is
deleted when it is done, and the commit that closes it says what changed
([ADR-0018](decisions/ADR-0018-maintenance-process.md)).

The library was built in numbered development phases. The last commit that
still has their outcome write-ups and the session log is `97f1647`. Read them with
`git show 97f1647:docs/roadmap.md` and `git show 97f1647:docs/handoff.md`. ADR
bodies still cite those phase IDs.

## Current state

The library is feature-complete for what it exists to do:
* SMP framing, reassembly and request correlation;
* the OS and image management groups;
* the statistics and settings management groups (groups 2 and 3), traced to
  Zephyr's source and run against the BL54L15 bench;
* MCUboot image handling;
* the upload state machine and `FirmwareUpdater`;
* a reference WinRT BLE adapter and a Windows DFU tool;
* serial console framing, and a reference serial port adapter with an example;
* several images of one device in one update, including images the device
  commits itself after the client images are confirmed, and a reader for
  nRF Connect SDK's multi-image package (ADR-0021, ADR-0022);
* updates that follow the device's MCUboot mode (ADR-0025): the mode read
  through OS command 8, a refusal before anything is sent for an update the
  mode cannot carry out or a downgrade the device would refuse, and direct-XIP
  devices updated from one file, one build per slot, or a direct-XIP package.

Its released version is 0.3.0. The Windows side has updated real devices from
two hardware benches, a NUCLEO-WB55RG and a BL54L15 DVK (nRF54L15, nRF Connect
SDK), including from a fresh clone consumed out of tree. The reference serial
port adapter (`transports/serial_port/`) has updated the stub device over a
pseudo-terminal in CI and the BL54L15 over its console UART. The multi-image
update and the MCUboot-mode handling have not met a device that exercises them:
they run against the simulator and the stub device only.

## In progress

Nothing. Pick the next item from the backlog below, by its "When".

## Acceptance gaps that need the hardware bench

None of these can be closed from a container.

* **Run a multi-image package against a coordinator MCU** (ADR-0021). This
  needs product firmware that implements the device contract in
  `multi-image.md`: a coordinator that applies a target's image, such as an
  application MCU updating its Bluetooth controller module. That firmware is
  outside smply. Until then, the contract is tested against the
  simulator and the stub device only. `serial_dfu --port … --package …` is
  the tool for the run.
* **Update a real direct-XIP device** (ADR-0025). Both variants, without and
  with revert, and from a direct-XIP package, are tested against
  `ServerSimulator`'s model of Zephyr's direct-XIP rules only (protocol-notes
  §7). A build of the BL54L15 with `CONFIG_MCUBOOT_BOOTLOADER_MODE_DIRECT_XIP`
  (and `_WITH_REVERT`) and `CONFIG_MCUMGR_GRP_OS_BOOTLOADER_INFO` would close
  it: the mode reported, the upload landing in the slot not running, and the
  device booting it. Its bench's swap build would also show the mode reported
  for the first time.
* **Commission the self-hosted `smply-bench` runner.** `hil.yml` is committed
  but no runner is registered, so the hardware suite has only ever run by hand.
  Its header carries the steps and the `schedule:` block to restore. The
  cross-check's HCI capture needs BTVS running elevated. A runner registered as
  a service can do that and an interactive session cannot.

## Open questions

Resolving a question means an ADR or a documentation change, not a code
comment. The IDs are stable, because code and documents cite them, and
`check_docs.py` R2 checks that every cited ID is defined here.

| ID | Question | Status and notes |
| -- | -------- | ---------------- |
| O1 | Which licence for smply itself? | **Resolved: Apache-2.0**, with `LICENSE`, `NOTICE` and an SPDX identifier in every source file. |
| O2 | Should smply probe SMP v2 and fall back to v1? | **Resolved: no.** v1 stays the default and v2 is an explicit opt-in (`SmpClientConfig::smp_version`). Resolved from hardware; see ADR-0010's status note. |
| O3 | Raise `max_in_flight` above 1 using `buf_count`? | **Open.** The peer reports `buf_count` 4 beside `buf_size` 2475 (protocol-notes §8), so server buffering is not what limits smply to one request. The open question is whether the throughput gain is worth giving up the retransmission reasoning of ADR-0010. Answering it needs a new ADR, because pipelining changes ADR-0010's premise about which offset is authoritative and ADR-0006's about non-interleaved fragments. It also needs a `ServerSimulator` that models `buf_count`. |
| O4 | Is `MemoryImageSource` enough, or does the library want a `FileImageSource`? | **Resolved: `MemoryImageSource` only.** A file-backed source is a dozen lines in the application. The examples share one in `support/dfu_app/`. |
| O5 | Multi-image (image ≥ 1) in `UpdatePlan`: exercise it, or document it as untested? | **Resolved: exercised, and extended** ([ADR-0021](decisions/ADR-0021-multi-image-update.md)). Every decision is scoped to one image, and every confirm names its image by hash. Several images of one device go in one update with one reset, each committed by smply (`Client`) or by the device (`Device`, per the contract in `multi-image.md`). `support/dfu_package/` reads nRF Connect SDK's multi-image package. Covered by `ServerSimulator`'s N image pairs and device-committed mode, the stub device, and the examples' package ctests. Not yet run against a multi-image device. |
| O6 | Expose a `std::error_code` interop layer? | **Open.** Only if a consumer asks. |
| O7 | Does a device reset drop a serial link, and what should `FirmwareUpdater` assume? | **Open for USB CDC only.** ADR-0020's answer needs no core change. A USB CDC port that vanishes is reported by the adapter as `on_disconnected`, so `AwaitingDisconnect` ends at once. A hardware UART that stays open reports nothing, so the updater moves on when `UpdatePlan::disconnect_grace` expires; a serial application sets that to a few seconds. On `ReconnectRequired` the application reopens by path in both cases, and a stable path such as `/dev/serial/by-id/…` covers a port that returns renamed. `examples/serial_dfu/`'s two ctests show both shapes against a pty stub, the renamed CDC case included. **Measured for a UART that stays open** (BL54L15 bench, 2026-10-05): over the J-Link VCOM, four `serial-update` runs saw the reset as `grace`, reopened by path and completed with no SMP timeout, and the boot output was discarded as console noise. **Assumed, not measured:** that a real CDC port reports the hang-up at all, and how long it is gone. The nRF54L15 has no USB; closing this needs one `serial-update` run against a device on a USB CDC port. |

## Backlog

Known work that nobody has needed yet. Each item says when it becomes worth
doing.

### Core library and API

| Item | When |
| ---- | ---- |
| **One update run for applications, and a sealed update state machine** ([`update-run-plan.md`](update-run-plan.md)). `report()` returns an empty report while an update runs; every application re-implements the update loop; the state machine's context is written by `FirmwareUpdater` and by the unit tests. | next: ready for implementation |
| **One `poll` and one `next_deadline` for the whole core.** `FirmwareUpdater` keeps timers separate from `SmpClient`'s, so every pump merges two deadlines. Running the updater's timers on `SmpClient::poll` would remove that, but it changes the public interface and touches ADR-0003 and ADR-0004. The update run in `update-run-plan.md` hides the two clocks from applications first. | if an application outside `smply::dfu_app` trips over the two clocks |
| **Several *devices* in one update.** `FirmwareUpdater` updates one device, whose own firmware keeps several images consistent (ADR-0021). A product without a coordinating MCU would need a host-side coordinator over several updaters, and ADR-0021 records why that cannot make the pair atomic. | when a product without a coordinating MCU needs it |
| **Retry, restart and bytes-sent counters in `UpdateReport`.** A caller cannot see that an update succeeded only after retransmissions, and a resume that finds the transfer already complete cannot say how much this run moved. `UploadResult` would have to carry the counters first. `already_present` means only "no progress in this session". | when a caller asks |
| **`Error` cannot carry an OS diagnostic.** `where()` is a static literal and `reason()` is the device's `rsn`, so an adapter drops the `HRESULT` behind every WinRT failure, and the serial adapter drops the `errno` or `GetLastError()` behind every port failure. Widen `reason()`'s contract or add a detail field. | when an adapter is next touched |
| **`Transport` has no `connected()` query**, so a transport that reconnects underneath the client cannot say so. `SmpClient` tracks link state itself, which is enough today. | if a self-healing transport is wanted |
| **Two transport obligations are documented on `SmpClient`, not in the normative contract** (`transport.hpp`, `design.md` §9): `send()` must not deliver inbound bytes before it returns, and a transport must outlive every client bound to it. A transport that notified its listener on destruction would remove the second. | when the transport contract is next revised |
| **`Result` has no monadic operations** (`and_then`, `transform`). `std::expected` has them and smply's C++20 subset does not, so using them would break the C++20 build. | if the same unwrap is hand-rolled repeatedly |
| **`cbor::Writer` cannot write a nested map or array under a key.** No MCUmgr request in scope needs one; the reader side now visits maps, text and unsigned-valued maps. | when a new group's request needs it |
| **Settings `commit()` and `load()` use the client's default deadline.** `save()` takes `SaveOptions::timeout`; the other two run every settings handler but write nothing, so they were left without one. Not observed. | if one is seen to outlast the default |
| **`to_string(const Error&)` allocates.** The zero-allocation path is `to_string(ErrorCode)`. | when logging is profiled as hot |
| **The upload driver copies each chunk once before encoding**, because `cbor::Writer` needs the bytes up front. A `put_bytes_from()` that fills in place would remove a 512-byte copy per chunk. | if a profile says so |
| **The client-context assertion covers `SmpClient` only.** Using `ImageManagement` from a second thread without reaching the client (for example, by reading `transferred()`) does not trip it. Closing that needs the groups to use pimpl. | when the groups are next reworked |
| **`Dispatcher::pending()` is racy by construction** and exists for diagnostics. An adapter branching on it is a bug; a blocking `wait_and_drain()` may be the better offer. | when an adapter asks |
| `ImageState` has no `operator==`. | when a test wants it |
| **A direct-XIP image that is not newer than the running one is sent anyway.** A direct-XIP bootloader boots only a newer image, a tie going to the lower slot (protocol-notes §7), and MCUboot's downgrade prevention cannot be enabled in that mode. So an equal or older image is uploaded in full and then fails after the reset, reported as a rollback. The updater could refuse it before the upload, as `Refusal::Downgrade` does when the device reports `no-downgrade`; that is new behaviour ADR-0025 did not decide. | if a direct-XIP user is caught by it |
| **Nothing checks that a package is meant for the device it is sent to.** The package names its `board` (`DfuPackage::board`), but smply takes no expectation from the caller and does not compare. A mismatch is refused only by the device's signature check, after the upload. A caller-supplied expectation compared in `PackageUpdate` would catch it before the first byte. | when a product ships several boards from one tool |

### Protocol and images

| Item | When |
| ---- | ---- |
| **The FS, Shell, Enum and Zephyr-basic management groups are not implemented** (groups 8, 9, 10 and 63). Every full client in Zephyr's tools table has FS and Shell, and most have Enum; smply's scope so far is the update. Enum would also let smply ask which groups a device has instead of handling `ENOTSUP` per command. Each is a group of its own, traced to Zephyr's `smp_group_<n>.rst` and source. | when an application asks for one |
| **Single slot, the firmware loader and the RAM-load modes have no update path.** `FirmwareUpdater` refuses them by name (`Refusal::UnsupportedMode`, ADR-0025). Each needs a flow of its own: single slot and the firmware loader update through a separate loader, and RAM load has no set-state (S10). A Zephyr RAM-load device reports `-1` (A38), so it is not even refused by name: it is treated as unknown and fails at set-state, as before. | when a product uses one |
| **MCUboot serial recovery is not a supported target.** Its SMP server is not the application's image group: an upload's `image` names a slot (S43), set-state only schedules the secondary slot (S44), and the default upload overwrites the primary slot with no trial. `serial_dfu` pointed at a device in recovery would not behave. Supporting it would be a separate update mode, not a flag. | if a product updates through serial recovery from the PC |
| **Ask the device which board it is.** The OS group's `info` command (`os_mgmt_info`) can report the board, which would let the board check above use the device's own answer instead of the caller's. It needs the command implemented in `OsManagement`, and reading Zephyr's source for the format. | with the board check, if a caller cannot supply the expectation |
| **The package manifest is not authenticated.** nRF Connect SDK's `manifest.json` carries no signature, so a tampered manifest can relabel an image's index. Each image is signed, and its signed dependency TLVs are what stop a mismatched set from booting; smply's self-consistency check (ADR-0022) reads those, not the manifest. A signed manifest would be a product format of its own. | if a product defines a signed container |
| **Compressed images are unhandled.** MCUboot's `IMAGE_F_COMPRESSED_*` flags and `IMAGE_TLV_DECOMP_SHA` raise the same slot-hash question as encrypted images (A13). smply carries the flags through without interpreting them. Decide whether to flag them like `encrypted` or document them as untested. | before claiming support |
| **`upload_image_id` means two things** in a slot-info response (protocol-notes §6): the global slot index plus one under `CONFIG_MCUMGR_GRP_IMG_DIRECT_UPLOAD`, and the image number otherwise. smply reports it verbatim and treats it as advisory. | if the upload path ever wants it |
| **The TLV entry cap counts loop iterations**, so stepping over the unprotected area's header consumes one unit. This makes no difference at 256. | if the cap is tightened |
| **A write that fails mid-message leaves the device's reassembler holding a partial message.** The adapter discards its waiting message, so the request times out. Whether Zephyr's `smp_bt` reassembler discards a partial message on a timeout of its own is unverified, and the answer decides whether the discard is always sufficient or only usually. | when the reassembler is next read |
| **A `TransportBusy` seen again would need a clock-driven backoff, and that needs an ADR.** Nothing below `FirmwareUpdater` owns a clock (`design.md` §6), and giving one a clock touches ADR-0003 and ADR-0004. Do not patch it quietly. | if it is seen again |
| **Zephyr's settings documentation names the wrong Kconfig for the read response's `max_size`** (protocol-notes §9, A31): it says `CONFIG_MCUMGR_GRP_SETTINGS_NAME_LEN`, the source uses `_VALUE_LEN`. Reporting it upstream is an outward-facing act and needs the maintainer's go-ahead. | when the maintainer agrees |
| **QCBOR mishandles consecutive indefinite-length breaks** (`dependencies.md`, protocol-notes §9 A18), in pinned 1.6.1 and `master` alike. It is worked around in `cbor::Reader`. Reporting it upstream is an outward-facing act and needs the maintainer's go-ahead. The minimal reproduction is `{"images": [_ {"slot": 0}], "x": 5}`, walked with `EnterArrayFromMapSZ` / `EnterMap` / `ExitMap` / `PeekNext`. | when the maintainer agrees |

### Transports and examples

| Item | When |
| ---- | ---- |
| **`winrt_ble_dfu` has no `--package`.** The multi-image update runs from `cli_dfu` and `serial_dfu` only. Over BLE on a dual-MCU product the link drops while the controller is updated (ADR-0022), so the Windows tool's reconnect policy (six attempts, 23.5 s of backoff between them) must be sized for the controller's transfer and reboot before a package update can rely on it. | when the dual-MCU product is on the bench |
| **A QSPI split-image direct-XIP package is refused.** nRF Connect SDK's QSPI XIP build lists four files: an internal and an external part, each linked for both slots (S58). The reader refuses more than one image given as a slot pair, by name, and the updater refuses several images under direct-XIP anyway, because Zephyr's image group updates one (S10). | if a product ships one |
| **`support/dfu_package` reads stored zips only.** A deflated package is refused by name. Deflate would mean a new dependency or a hand-written inflater, and the package format written today is stored (ADR-0021, S38). | when someone ships a deflated package |
| **The stub device does not model the confirm-denial rules for image 1** (A27), so `--commit 1=client` succeeds against it where a default Zephyr build would refuse. `ServerSimulator` models them, and its tests cover the refusal. | if the examples are used to demonstrate A27 |
| **The serial port adapter's Win32 half is compile-only** (ADR-0020). It is outside clang-tidy and cppcheck, and CI never opens a port with it; a MinGW cross-build is the only local check (handoff.md). Running clang-tidy on the Windows runner would recover the analysis for this half and for `winrt_ble` alike. | with the WinRT row below |
| **No CI job builds on macOS**, so the POSIX serial half's macOS paths (`#ifdef B460800` and friends, `CRTSCTS` as a plain `int`) are unbuilt. | if a macOS user appears |
| **The serial adapter writes a message's frames back to back.** Zephyr's UART driver holds only two undecoded lines (protocol-notes A26), so a slow device could drop one and the request would time out. A per-frame pause in the adapter would fix it. Not observed. | if the bench shows it |
| **Raw UART (`CONFIG_MCUMGR_TRANSPORT_RAW_UART`) is not implemented.** It needs no framing, only the port, so it is `SerialPortTransport` without `frame_message()`/`SerialInbound`, plus a way to delimit messages without the frame markers. | when a device uses it |
| **The WinRT adapter is outside clang-tidy and cppcheck**, because both run from a Linux build. Running clang-tidy on the Windows runner would recover the analysis. That is the prerequisite for installing `smply::winrt_ble` (ADR-0016). | when the adapter is wanted in the package |
| **`cli_dfu` shows only the happy path.** A `--fail-confirm` mode would show MCUboot's revert, the safety property the design turns on. It needs the stub device to model a boot failure. | when revert is to be demonstrated |
| **`--flaky-reconnect` refuses attempts in the application**, not in the stub device, so it cannot model a device that accepts a connection and then drops it mid-handshake. | when the stub is next extended |
| **The examples build in-tree only**, because they link `smply::minicbor` and `smply::dfu_app`, which are deliberately not installed (ADR-0016). Say so where a consumer will look, or give `cli_dfu` a mode without the stub device. | when a consumer tries it |
| **An installed sanitizer build does not carry its link options to consumers**, because they ride on `smply_internal_options`, which is `$<BUILD_INTERFACE:>`. | if anyone ships one |

### Tests, tooling and CI

| Item | When |
| ---- | ---- |
| **`fuzz_smp_client_rx` drives one pending request**, so it cannot reach the retired-sequence table (`kMaxRetiredSeqs`, `security.md` T5). The unit suite covers that table. | when the target is next touched |
| **The per-directory coverage targets are measured, not enforced.** `coverage.sh --enforce` applies only the two whole-core thresholds (`quality-gates.md` §6). | if a directory regresses unnoticed |
| **The nightly fuzz soak dedupes on one open issue per target**, so a second, different crash in the same target is silent until the first issue is closed. | if finds become common |
| **R5 cannot read a glob** (`server_simulator.*`), so a layout entry written with one is checked by nobody. | when a glob entry drifts |
| **`protocol-notes.md` has two kinds of verification date**: read from source, and observed on a radio. Nothing marks which is which. A per-fact `[source]` / `[bench]` marker would make it checkable. | when a third kind of evidence appears |
| **`osv.yml` is unproven on two counts.** It has never fired on its cron, and nothing shows that OSV has advisory coverage for QCBOR and Catch2 as `pkg:github` PURLs. An empty report looks the same either way. Put a package with a known advisory through `tools/sbom.py` once. | before a clean report is relied on |
| **Nothing enforces that unit and component tests never read the real clock** (`testing.md` §2). A `lint.sh` grep for `steady_clock::now()` under `tests/unit/` and `tests/component/`, with a `verify_gates.sh` case proving it fires, would. | when a test is next found reading it |
| **`-Wnull-dereference` is off for GCC** because of false positives inside libstdc++ under `-O2` (`cmake/warnings.cmake`). | when GCC stops false-positiving |
| **`check_docs.py` R6 does not read a root `GLOSSARY.md`.** Its living-document list names the top-level files one by one, and the glossary `/domain-modeling` creates lazily (`docs/agents/domain.md`) is not among them. | when `GLOSSARY.md` is first created |
| **`.claude/validate.py` is not run by CI**, so a skill update or a change to `tools/sources.sh` that breaks the Claude Code setup is caught only by whoever runs it. A step in the `docs` job would do; it needs only Python and clang-format. | if the setup drifts unnoticed |

### Hardware bench

| Item | When |
| ---- | ---- |
| **HCI capture does not work on this bench.** BTVS never opens its listener, and the manifest provider `Microsoft-Windows-BTH-BTHPORT` captured nothing. The untried lead is the WPP provider in Microsoft's own recording profile, converted with `BTETLParse.exe -pcap`. `tests/hil/README.md` has the commands. Until then the cross-check's Tier B is decode-verified, not live. | when a capture is wanted, or with the runner |
| **The UART client arm cannot complete an upload**, so the cross-check is two clients and an oracle (protocol-notes §9). A third client needs the peer's logs moved off USART1, or the raw UART MCUmgr transport. | if a UART comparison is wanted |
| **The WinRT adapter does not ask for a fast connection interval**, so a BLE upload runs at whatever interval wins between the peer's one-shot request and Windows' own choice: 7.5 ms or 45 ms on the BL54L15, about 2x in upload time (protocol-notes §9, A36). Windows 11 documents `BluetoothLEDevice.RequestPreferredConnectionParameters` (`ThroughputOptimized`) for this; not yet tried. An adapter option, not a core change. | if upload time matters, or with O3 |
| **`smp_decode.py` is a second CBOR reader and SMP reassembler.** It exists so the decode is independent of the clients being compared (ADR-0015), but two decoders can drift. | if the cross-check grows |
