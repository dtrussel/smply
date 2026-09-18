<!-- SPDX-License-Identifier: Apache-2.0 -->

# `winrt_ble_dfu` — updating a Zephyr device over Bluetooth LE

A console tool that installs firmware on a Zephyr device running the MCUmgr SMP
server, over BLE, on Windows. It is the reference for how an application drives
smply **across a device reboot** — which is the part of a DFU that is easy to
get wrong and impossible to see in a single-shot example.

## Read this first: what has and has not been verified

This program was written on Linux, where it cannot be compiled, and the
`windows-winrt` CI job compiles and links it on a runner with no Bluetooth radio.
**It first ran in P17a** (2026-09-08), against a NUCLEO-WB55RG running Zephyr
4.4.99's `smp_svr` (`tests/hil/README.md`):

| Claim | Status |
| ----- | ------ |
| It compiles and links | **verified** by CI |
| The reconnect backoff and give-up schedule | **verified** — `smply::dfu_app::ReconnectPolicy` is unit-tested, and `cli_dfu --flaky-reconnect` drives it on every push |
| Reading firmware off disk | **verified** — the same `FileImageSource` `cli_dfu` uses |
| What Zephyr advertises, and the active-scan requirement | **verified against Zephyr's source** (protocol-notes §8, S22); the device advertised the SMP UUID and was found by it |
| Connecting by address and a whole update over the air | **verified on hardware** — `Completed` in both directions, about 26 s for a 134 KiB image including the device's 6 s reboot |
| `--name` (an active scan for the scan-response name) | verified on hardware in P17a's log |

Each line of the progress output carries the milliseconds since the update
began, and the report ends with the client's counters (`sent`, `received`,
`timed out`, `late`, …). Both were added on the bench: without them the very
first defect — a final chunk timing out and completing through a retransmission
(protocol-notes §9, A19) — was invisible in an update that reported success.

## Usage

```
winrt_ble_dfu --image firmware.bin                    # first SMP device seen
winrt_ble_dfu --image firmware.bin --name my-thing    # match an advertised name
winrt_ble_dfu --image firmware.bin --address AA:BB:CC:DD:EE:FF
```

| Option | Meaning |
| ------ | ------- |
| `--image PATH` | The firmware to install. **Required** — see below. |
| `--name NAME` | Connect to the first device whose advertised name contains `NAME`. |
| `--address ADDR` | `AA:BB:CC:DD:EE:FF` or bare hex. Skips scanning. |
| `--mode MODE` | `test-then-confirm` (default), `confirm-immediately`, `upload-only`. |
| `--scan-timeout MS` | How long to look for a device. Default 10000. |
| `--quiet` | Print only the outcome. |

Exit codes: `0` success · `1` update failed · `2` usage · `3` no device ·
`4` reconnection gave up.

**`--image` is required, unlike `cli_dfu`.** That example invents a demo image
when given none, because it installs it into a stub in its own process. Writing
a synthetic image to real hardware would install firmware that does not run.

## Two obligations on the caller

1. **A multi-threaded apartment.** `main()` calls
   `winrt::init_apartment(winrt::apartment_type::multi_threaded)` before
   anything else. Both the scanner and `WinRtBleTransport::connect()` block on
   WinRT asynchronous operations, which deadlocks on a single-threaded
   apartment — silently, with no diagnostic.
2. **Bluetooth must be on and the device in range.** The tool does not pair or
   bond; a device requiring pairing must already be paired in Windows settings.

## Why the scan is active

Zephyr's `smp_svr` sample puts the **SMP service UUID in the advertisement** and
the **device name in the scan response** (protocol-notes §8, source S22). A
passive scan never asks for the scan response, so `--name` under a passive
watcher would match nothing — indefinitely, and indistinguishably from the
device being switched off. `scanner.cpp` therefore always scans actively, and
uses the service UUID as the default filter because that one *is* in the
advertisement.

A product need not advertise the SMP UUID at all — it costs 16 of an
advertisement's 31 bytes. That is why `--address` exists and why the UUID filter
is a default rather than a requirement.

## What to read, in what order

1. **`examples/cli_dfu/main.cpp`** — the same pump loop, against a stub, and the
   one that actually runs in CI.
2. **`main.cpp` here** — that loop pointed at a radio. The pump is written out
   in full rather than shared, because an example exists to be read.
3. **`scanner.cpp`** — turning "the device on my desk" into an address.
   Scanning is the application's job, never the transport's (ADR-0005).

The reconnect backoff is *not* in this directory: it is
`support/dfu_app/reconnect_policy.hpp`, shared with `cli_dfu`. Nothing here is
run by CI, so anything that can live somewhere testable does.

## Static analysis

clang-tidy and cppcheck do **not** see this directory, for the same reason they
do not see `transports/winrt_ble/`: both run from a Linux build where these
translation units do not exist. clang-format still covers it, and MSVC's
`/W4 /WX` stands in. See `docs/quality-gates.md` §3.
