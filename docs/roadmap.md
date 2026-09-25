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
* MCUboot image handling;
* the upload state machine and `FirmwareUpdater`;
* a reference WinRT BLE adapter and a Windows DFU tool;
* serial console framing, and a reference serial port adapter with an example.

Its released version is 0.2.0. The Windows side has updated a real device from
a hardware bench. A reference serial port adapter (`transports/serial_port/`)
has updated the stub device over a pseudo-terminal in CI. It has not yet been
used against a device.

## In progress

Nothing. The structured quality review that produced 0.2.0 is finished; its
plan is in git history (`git log --diff-filter=D -- docs/review-plan.md`). Pick
the next item from the backlog below, by its "When".

## Acceptance gaps that need the hardware bench

None of these can be closed from a container.

* **Run the serial HIL cases** (`--uart COM4 --cases serial,serial-update`).
  The serial port adapter's Win32 half has only been compiled, and nothing
  serial has met a device. The first run answers O7 and A26, and says whether
  the adapter tolerates the bench console's shell and log traffic, which
  defeated `mcumgr-client`. A failure there is a finding for protocol-notes.md.

* **Re-run an update from a fresh clone consumed out of tree.** The
  consumption half runs on every push (`tools/check_install.sh`, three modes).
  The device half needs one run of `winrt_ble_dfu` built from a
  `find_package` consumer instead of in-tree. The install rules have changed
  since the last bench run, and the protocol path has not. So a pass is
  expected, but it has not been shown.
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
| O5 | Multi-image (image ≥ 1) in `UpdatePlan`: exercise it, or document it as untested? | **Resolved: exercised** ([ADR-0021](decisions/ADR-0021-multi-image-update.md)). `ServerSimulator` models N image pairs, and the updater scopes every decision to `plan.upload.image` and confirms by hash. Component tests cover the image-1 update, resume, revert, an absent image and the default refusal to confirm image 1. Not yet run against a multi-image device. |
| O6 | Expose a `std::error_code` interop layer? | **Open.** Only if a consumer asks. |
| O7 | Does a device reset drop a serial link, and what should `FirmwareUpdater` assume? | **Open: designed for, tested on a pseudo-terminal, not measured on hardware.** ADR-0020's answer needs no core change. A USB CDC port that vanishes is reported by the adapter as `on_disconnected`, so `AwaitingDisconnect` ends at once. A hardware UART that stays open reports nothing, so the updater moves on when `UpdatePlan::disconnect_grace` expires; a serial application sets that to a few seconds. On `ReconnectRequired` the application reopens by path in both cases, and a stable path such as `/dev/serial/by-id/…` covers a port that returns renamed. `examples/serial_dfu/`'s two ctests show both shapes against a pty stub, the renamed CDC case included. **Assumed, not measured:** that a real CDC port reports the hang-up at all, how long it is gone, and whether a real UART's boot output is only noise. The `serial-update` HIL case (`tests/hil/README.md`) is the measurement, and closing this needs one run of it. |

## Backlog

Known work that nobody has needed yet. Each item says when it becomes worth
doing.

### Core library and API

| Item | When |
| ---- | ---- |
| **Retry, restart and bytes-sent counters in `UpdateReport`.** A caller cannot see that an update succeeded only after retransmissions, and a resume that finds the transfer already complete cannot say how much this run moved. `UploadResult` would have to carry the counters first. `already_present` means only "no progress in this session". | when a caller asks |
| **`Error` cannot carry an OS diagnostic.** `where()` is a static literal and `reason()` is the device's `rsn`, so an adapter drops the `HRESULT` behind every WinRT failure, and the serial adapter drops the `errno` or `GetLastError()` behind every port failure. Widen `reason()`'s contract or add a detail field. | when an adapter is next touched |
| **`Transport` has no `connected()` query**, so a transport that reconnects underneath the client cannot say so. `SmpClient` tracks link state itself, which is enough today. | if a self-healing transport is wanted |
| **Two transport obligations are documented on `SmpClient`, not in the normative contract** (`transport.hpp`, `design.md` §9): `send()` must not deliver inbound bytes before it returns, and a transport must outlive every client bound to it. A transport that notified its listener on destruction would remove the second. | when the transport contract is next revised |
| **`Result` has no monadic operations** (`and_then`, `transform`). `std::expected` has them and smply's C++20 subset does not, so using them would break the C++20 build. | if the same unwrap is hand-rolled repeatedly |
| **`cbor::Writer` cannot write a nested map or array under a key**, and `for_each_map_in_array` visits maps only. No MCUmgr request or response in scope needs either. | when a new group needs it |
| **`to_string(const Error&)` allocates.** The zero-allocation path is `to_string(ErrorCode)`. | when logging is profiled as hot |
| **The upload driver copies each chunk once before encoding**, because `cbor::Writer` needs the bytes up front. A `put_bytes_from()` that fills in place would remove a 512-byte copy per chunk. | if a profile says so |
| **The client-context assertion covers `SmpClient` only.** Using `ImageManagement` from a second thread without reaching the client (for example, by reading `transferred()`) does not trip it. Closing that needs the groups to use pimpl. | when the groups are next reworked |
| **`Dispatcher::pending()` is racy by construction** and exists for diagnostics. An adapter branching on it is a bug; a blocking `wait_and_drain()` may be the better offer. | when an adapter asks |
| `ImageState` has no `operator==`. | when a test wants it |

### Protocol and images

| Item | When |
| ---- | ---- |
| **Over serial, `buf_size` overstates the device's whole-message limit by four bytes** (protocol-notes §9, A25): the netbuf holds the serial length prefix and CRC too. Upload sizing takes `min(buf_size, transport max, cap)`, so a device whose `buf_size` is at or below the serial adapter's cap gets messages it silently drops. Fixing it means letting a transport report a per-message overhead that the core subtracts from `buf_size`, which is a change to the `Transport` contract. The adapter's 256-byte default keeps a default-configured device safe. | before claiming serial support for devices with a small `buf_size` |
| **Compressed images are unhandled.** MCUboot's `IMAGE_F_COMPRESSED_*` flags and `IMAGE_TLV_DECOMP_SHA` raise the same slot-hash question as encrypted images (A13). smply carries the flags through without interpreting them. Decide whether to flag them like `encrypted` or document them as untested. | before claiming support |
| **`upload_image_id` means two things** in a slot-info response (protocol-notes §6): the global slot index plus one under `CONFIG_MCUMGR_GRP_IMG_DIRECT_UPLOAD`, and the image number otherwise. smply reports it verbatim and treats it as advisory. | if the upload path ever wants it |
| **The TLV entry cap counts loop iterations**, so stepping over the unprotected area's header consumes one unit. This makes no difference at 256. | if the cap is tightened |
| **A write that fails mid-message leaves the device's reassembler holding a partial message.** The adapter discards its waiting message, so the request times out. Whether Zephyr's `smp_bt` reassembler discards a partial message on a timeout of its own is unverified, and the answer decides whether the discard is always sufficient or only usually. | when the reassembler is next read |
| **A `TransportBusy` seen again would need a clock-driven backoff, and that needs an ADR.** Nothing below `FirmwareUpdater` owns a clock (`design.md` §6), and giving one a clock touches ADR-0003 and ADR-0004. Do not patch it quietly. | if it is seen again |
| **QCBOR mishandles consecutive indefinite-length breaks** (`dependencies.md`, protocol-notes §9 A18), in pinned 1.6.1 and `master` alike. It is worked around in `cbor::Reader`. Reporting it upstream is an outward-facing act and needs the maintainer's go-ahead. The minimal reproduction is `{"images": [_ {"slot": 0}], "x": 5}`, walked with `EnterArrayFromMapSZ` / `EnterMap` / `ExitMap` / `PeekNext`. | when the maintainer agrees |

### Transports and examples

| Item | When |
| ---- | ---- |
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

### Hardware bench

| Item | When |
| ---- | ---- |
| **HCI capture does not work on this bench.** BTVS never opens its listener, and the manifest provider `Microsoft-Windows-BTH-BTHPORT` captured nothing. The untried lead is the WPP provider in Microsoft's own recording profile, converted with `BTETLParse.exe -pcap`. `tests/hil/README.md` has the commands. Until then the cross-check's Tier B is decode-verified, not live. | when a capture is wanted, or with the runner |
| **The UART client arm cannot complete an upload**, so the cross-check is two clients and an oracle (protocol-notes §9). A third client needs the peer's logs moved off USART1, or the raw UART MCUmgr transport. | if a UART comparison is wanted |
| **`smp_decode.py` is a second CBOR reader and SMP reassembler.** It exists so the decode is independent of the clients being compared (ADR-0015), but two decoders can drift. | if the cross-check grows |
