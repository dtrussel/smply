<!-- SPDX-License-Identifier: Apache-2.0 -->

# P17 hardware interoperability (`tests/hil/`)

**Status: bring-up in progress.** Nothing here is part of the PR gate, and
`SMPLY_BUILD_HIL` stays `OFF` in every normal build. The sections below are kept
true as the phase advances; the roadmap's P17 entries say what each phase
established. Thirteen unattended cases pass back-to-back (P17b) and the
cross-check below compares smply against a third-party client on the same
device (P17c) -- but no self-hosted runner is registered, so all of it runs from
this bench, by hand.

## The bench

| Item | Value |
| ---- | ----- |
| Board | **NUCLEO-WB55RG** (P-NUCLEO-WB55 Nucleo68), STM32WB55RGV6, device ID 0x495 rev Y, 1 MB flash |
| Probe | on-board ST-LINK/V2-1 (firmware V2J45M30); appears as `ST-Link Debug` plus `STLink Virtual COM Port` |
| Console | USART1 → the ST-LINK VCP, 115200 8N1. Resolve the COM port by device, not by number |
| Programmer | STM32CubeProgrammer **2.22.0** CLI (`STM32_Programmer_CLI.exe`) |
| Host radio | Intel Wireless Bluetooth, Windows 11 (26200). Shared with the user's own peripherals, so scans are noisy: filter on the SMP service UUID or the name below, never "first device seen" |
| Firmware | Zephyr `e71ff182603865f59e2e25f05655d6affda4f288` (4.4.99, `v4.4.0-1908`), MCUboot `ee39e2d694bd827ffd1bebbce2f571a9154e6ec2`, hal_stm32 `d1d3c0c9ddf697f6bcda911f158777136aa21c5c`, Zephyr SDK 1.0.1 |
| Peer name | `smply-p17-wb55` (`firmware/peer.conf`) |

### The wireless coprocessor (CPU2) — do this once

Zephyr on STM32WB drives the Bluetooth controller on the Cortex-M0+ over IPCC,
and it needs the **HCI-only** coprocessor binary: since STM32CubeWB 1.13.2 the
"full stack" binaries do not work with Zephyr at all (`boards/st/nucleo_wb55rg/doc`).
The pinned hal_stm32 is based on **STM32CubeWB 1.24.0** (`lib/stm32wb/README.rst`),
so the matching binaries are `Projects/STM32WB_Copro_Wireless_Binaries/STM32WB5x/`
at tag `v1.24.0` of `STMicroelectronics/STM32CubeWB`:

| Binary | SHA-256 | Install address (1 MB part) |
| ------ | ------- | --------------------------- |
| `stm32wb5x_FUS_fw_1_2_0.bin` | `2712d4d2…addbfd` | `0x080EC000` |
| `stm32wb5x_FUS_fw.bin` (FUS 2.2.0) | `bbab9b42…edf30d` | `0x080EE000` |
| `stm32wb5x_BLE_HCILayer_fw.bin` | `7c7971bd…872d66e` | `0x080E1000` |

The bench board arrived with FUS 1.0.2 and a full-feature stack (SFSA `0xCB`). The
release notes require an incremental FUS upgrade, and this is what worked:

```text
STM32_Programmer_CLI -c port=swd mode=UR -startfus
STM32_Programmer_CLI -c port=swd mode=HOTPLUG -r32 0x20030030 1     # FUS version word
STM32_Programmer_CLI -c port=swd mode=UR -fwupgrade stm32wb5x_FUS_fw_1_2_0.bin 0x080EC000 firstinstall=0
STM32_Programmer_CLI -c port=swd mode=UR -fwupgrade stm32wb5x_FUS_fw.bin       0x080EE000 firstinstall=0
STM32_Programmer_CLI -c port=swd mode=UR -fwupgrade stm32wb5x_BLE_HCILayer_fw.bin 0x080E1000 firstinstall=1
STM32_Programmer_CLI -c port=swd mode=UR -ob displ                   # expect SFSA = 0xE1
```

Two things to know: reading the version table under reset (`mode=UR`) returns
zeros, because SRAM2a is cleared by the reset — use `mode=HOTPLUG`. And the first
attempt to install the HCI layer over the old full stack reported
`FUS_ERROR_INVALID_OB … Old Firmware delete failed` while nevertheless removing
the old stack (SFSA moved to `0xF4`); the same command with `firstinstall=1`
then succeeded. `-fusgetstate`/`-startfus` load a small FUS operator into CPU1's
first two flash pages, which the baseline flash overwrites. The full transcript is
kept in the local evidence bundle (`build/hil-evidence/cpu2/`).

## Reproduce the firmware

`firmware/west.yml` pins Zephyr and imports only the module subset this board
needs. Create a separate west workspace — never put `.west` in this repository:

```powershell
New-Item -ItemType Directory <workspace>/manifest
Copy-Item tests/hil/firmware/west.yml <workspace>/manifest/west.yml
west init -l <workspace>/manifest
Set-Location <workspace>
west update --narrow -o=--depth=1
```

Use a Python virtual environment with `west` 1.5.0, `pyserial` 3.5, `imgtool`
2.4.0 and the pinned Zephyr `scripts/requirements-base.txt`. Then, from the smply
checkout:

```powershell
python tests/hil/firmware/build_peer.py --workspace <workspace> `
  --build-dir <out> --sdk <zephyr-sdk-1.0.1>
```

The recipe runs `west build --sysbuild` **twice** on the unmodified `smp_svr`
sample with its `bt.conf` plus our `firmware/peer.conf`: image **A** signed as
version 1.0.0 and image **B** as 2.0.0. Both are signed by Zephyr's own signing
step, so header size, slot size and the STM32WB's 8-byte write alignment come
from the board's device tree and MCUboot's Kconfig rather than from a number
copied by hand. Same source, different signed version, therefore different
`IMAGE_TLV_SHA256` hashes — and neither image confirms itself, which the
rollback case depends on. MCUboot is built for **swap-using-offset** (the
board's slot 1 is one 4 KiB page larger than slot 0 for exactly that) with the
sample's RSA-2048 development key, which is public and appropriate only for a
dedicated bench.

`<out>/evidence/` holds `a.signed.{bin,hex}`, `b.signed.{bin,hex}`,
`mcuboot.hex`, the effective application/MCUboot/sysbuild Kconfig, both device
trees, the frozen manifest, the Python freeze and `sha256.json`. The recipe
refuses to finish if the two builds produced different bootloaders.

## Restore the baseline

```powershell
python tests/hil/firmware/flash_baseline.py --evidence <out>/evidence
```

Full CPU1 erase, MCUboot + image A programmed and read back, hardware reset. This
is the one recovery primitive every HIL case relies on, and the reason the case
list can run unattended: whatever a case leaves behind — a pending swap, a
corrupted slot, a half-written upload — a reflash over ST-LINK undoes it. It
exits 2 when there is no probe, which a supervisor must report as *bench
unavailable*, never as a pass or a fail. CPU2 is not touched.

## Tools in `tools/`

* `uart_log.py` — timestamps the console UART into a file for the duration of a
  case (MCUboot's swap and validation messages come out here).
* `measure_reset.py` — the stopwatch behind `ReconnectSettings`' defaults: N
  resets, and for each the time from acceptance to disconnect, first
  advertisement, GATT connect and a usable SMP characteristic. A bench
  instrument only; it uses the third-party `smp`/`bleak` packages to put a reset
  on the air, and infers nothing about the protocol from them (ADR-0015). It
  **scans once per iteration and connects to the `BLEDevice` object**, never to
  the address string: a `BleakClient(address)` resolves the address with a scan
  of its own, and two scanners over one radio cost 13 of 20 iterations to
  "device not found" the first time this was run. Four iterations in twenty
  still fail with `WinError -2147023673` on a connect that races the peer coming
  back; that is the instrument, not smply.

## Manual acceptance came first (P17a, 2026-09-08)

P16's acceptance criterion — `winrt_ble_dfu` completing an update against a real
device — was discharged before any case was automated:

```powershell
python tests/hil/tools/uart_log.py --port COM4 --out dfu.uart.log --seconds 100   # alongside
winrt_ble_dfu.exe --image <out>/evidence/b.signed.bin --address 80:E1:26:00:65:E2
mcumgr-client.exe -d COM4 list                                                   # the oracle
```

Five runs completed in both directions (exit 0, `Completed`, about 26 s each);
the UART oracle showed the expected version and hash `confirmed` and `active`
after each. The defects the first runs exposed, the measurements, and what they
changed are in the roadmap's P17a outcome and `protocol-notes.md` §9 (A18, A19).
The peer's identity address is `80:E1:26:00:65:E2`; it advertises the SMP
service UUID and puts the name in the scan response, as protocol-notes §8 says.

Two facts about the bench that a case author needs: **only one process may hold
COM4** — stop `uart_log.py` before reading state with `mcumgr-client`; and the
device is unreachable for about 6.3 s after a swap reset, roughly 5 s of it
MCUboot copying the image, so any deadline around a reboot must allow for that
and for larger images taking longer.

## Running the cases (P17b)

```powershell
cmake --preset windows-hil          # from an MSVC developer shell
cmake --build --preset windows-hil
python tests/hil/run_hil.py --exe build/windows-hil/tests/hil/smply_hil.exe `
    --evidence <out>/evidence --address 80:E1:26:00:65:E2 --uart COM4 --out build/hil-runs
```

`smply_hil` is a Catch2 executable (`test_hil_cases.cpp`) driving the public API
through `support/rig.*` — the example's pump loop made callable one operation at
a time. Each case works out its own target image (whichever of A and B is not
running), so the supervisor reflashes the baseline between **groups** rather
than between cases and a person can run one case by hand against a device in
either state. Cases read the bench from `SMPLY_HIL_ADDRESS`, `SMPLY_HIL_IMAGE_A`
and `SMPLY_HIL_IMAGE_B`; without them they **SKIP**, which the supervisor reports
as `unavailable`.

`run_hil.py` wraps each group in `flash_baseline.py` and a `uart_log.py`
capture, runs each case with a hard deadline and a JUnit report, keeps its
stdout (timeline and `HIL-METRIC` lines), the device log and the report under
`build/hil-runs/<run>/<group>/<case>/`, and writes `summary.json` with a
**pass / fail / unavailable** verdict per case. Two groups need to know about
each other: the *restart* pair runs without a reflash in between, and the
*give-up* case prints `HIL-MARK: device-gone-now` when it starts reconnecting,
on which **a person** powers the board off so that nothing ever advertises
again. It is out of `--cases all` for that reason, and the supervisor prints a
notice when it is selected. An earlier design had the supervisor erase the
device over ST-LINK on that line; it never worked, because CubeProgrammer
toggles reset to attach and the device re-advertises before the erase halts it.
Exit status: 0 all pass, 1 any fail, 2 any unavailable.

A run that fails part way can leave the board **not advertising**. The next
run's per-group baseline flash recovers it; to recover by hand, run
`firmware/flash_baseline.py --evidence <out>/evidence`.

## Cross-checking against another client (P17c)

```powershell
python tests/hil/crosscheck.py --evidence <out>/evidence --address 80:E1:26:00:65:E2 `
    --uart COM4 --dfu build/windows-hil/examples/winrt_ble_dfu/winrt_ble_dfu.exe `
    --smpmgr build/hil-tools/Scripts/smpmgr.exe `
    --mcumgr-client build/mcumgr-client/mcumgr-client-windows-x86/mcumgr-client.exe
```

Each *arm* is reflashed to the same baseline and installs image B by
test-then-confirm, and device state is read through **`mcumgr-client` over
UART** — a path neither BLE client touches — at three checkpoints: after the
flash, **during the trial boot**, and after the confirm. The middle checkpoint
is the one that can fail: reading only before and after compares fields every
arm was already required to reach.

Before any arm runs, the script proves the oracle can tell two states apart —
flash A, flash B, flash A again, and require that the diff names exactly what
changed and nothing more. If that fails, the whole run is `unavailable` and no
arm runs, because a comparison whose instrument cannot detect disagreement
means nothing.

Three things to know before reading a result:

* **`mcumgr-client` is the oracle, not a third client**, and is not in the
  default `--clients`. It reads state reliably and could not complete an upload
  over the shell transport on this peer in any configuration tried
  (`docs/protocol-notes.md` §9). Ask for it explicitly to retry that on a
  future bench revision.
* **Without a capture the correct exit status is 2**, with Tier B reported
  `unavailable` for both BLE arms. That is not a pass, and it is not a failure
  either.
* **One capture can hold both arms.** Each arm is a fresh GATT connection and so
  a distinct connection handle, and `tools/smp_decode.py --all-streams` reports
  every stream carrying plausible SMP in the order they first appear. That
  matters if the capture is taken outside the script -- an externally started
  trace covers the whole run, not one arm.
* **The negative control is `--skip-confirm <arm>`**, which leaves one arm in
  its trial boot and must produce divergences in the slot count, the active
  slot's `confirmed` and the fallback slot's flags. Run it after any change to
  the comparison; if it exits 0, the comparison has stopped comparing. There is
  deliberately no "wrong image" control — with two images on the bench the only
  other image is the running one, and marking that for test is refused, so such
  an arm would fail rather than diverge.

## HCI capture

Microsoft's Bluetooth Test Platform (BTP 1.14.0) is installed at
`C:\BTP\v1.14.0\x86\btvs.exe`; with `-Mode Wireshark -Remote on` it exposes the
host controller's HCI traffic for `tshark -i TCP@127.0.0.1:24352`. **`btvs.exe`
requires an elevated shell.** A capture with zero packets is not evidence of
anything; check the packet count before trusting one.

`tools/hci_capture.py` does that checking, and the reason is on disk:
`build/capture-probe.pcapng` is a **valid pcapng with the right interface name,
the right encapsulation and zero packets**. BTVS accepts the TCP connection and
negotiates the link type without elevation — it just delivers nothing. So "the
socket connected" and "tshark started" are both true in the failing case, and
only the packet count discriminates.

### What P17c established about getting a capture at all

**No capture was obtained.** What follows is the route that remains untried at
its last step, and the three things that were ruled out, so the next attempt
starts where this one stopped rather than at the beginning.

**Ruled out — BTVS never opened its listener.** Its window reported
`Wireshark Viewer: Disabled` and `Error Connection failed`, and neither of two
running instances held any TCP socket. Its usage string is
`[-Mode Frontline|Ellisys|Wireshark] [-Address 127.0.0.1] [-Port 24352]
[-Service 1|2|3] [-Remote off|on]`, and it locates Frontline through registry
keys but Wireshark only by the bare name `wireshark` — which is not on `PATH`
here. It also tries to set `MaxEtwBytes`, `EtwDropLargeEvents` and
`EtwLogSensitiveData`, and **none of those keys exists on this machine**, so it
never got that far either.

**Ruled out — enabling the manifest provider by hand captures nothing.** A
`logman` session on `Microsoft-Windows-BTH-BTHPORT`
`{8A1F9517-3A8C-4A9E-A018-4F17A200F277}` at level 255 with every keyword,
verified attached by `logman query`, produced **two events across three minutes
of BLE traffic — both ETW housekeeping and zero Bluetooth**. Adding
`Microsoft-Windows-BTH-BTHUSB` and toggling the radio off and on changed
nothing. The keyword names looked right (`Microsoft-Windows-BTH-BTHPORT/HCI`,
`Microsoft-Windows-BTH-HCI/HCIRAW`), which is what made this worth ruling out
properly rather than assuming.

**The route that is left.** Microsoft's own profile, from
<https://aka.ms/BluetoothTracing>, enables ~156 providers including
`Microsoft.Windows.Bluetooth.WPP.BthPort` under
`{d88ace07-cac0-11d8-a4c6-000d560bcba5}` — a **WPP** provider, a channel none of
the attempts above touched. `BTETLParse.exe`, in the same BTP directory, turns an
ETL into a capture file and **runs unelevated**; its own message about not
supporting "PCAPNG from legacy tracing format" is the hint that it expects WPP
and that `-pcap` is the output to ask for. A copy of the profile is kept at
`build/hil-evidence/BluetoothStack.wprp`.

```powershell
# elevated, and note the selector is the profile's Name, not its Id --
# "!BluetoothStack.Verbose.File" fails with 0xc5600611
wpr -start "<repo>\build\hil-evidence\BluetoothStack.wprp!BluetoothStack" -filemode
# ... run crosscheck.py --capture off ...
wpr -stop C:\temp\BthTracing.etl
```

```powershell
# unelevated
& "C:\BTP\v1.14.0\x86\BTETLParse.exe" -pcap out.pcap C:\temp\BthTracing.etl
python tests/hil/tools/smp_decode.py --capture out.pcap --all-streams --out ops.json
```

**One side effect to plan for:** with that profile running, `winrt_ble_dfu`
failed in 0.1 s with "no device at that address" three seconds after a baseline
reflash — Windows had not yet cached the peer. The verbose tracing slows the
stack enough to expose a settle that was too short; `POST_FLASH_SETTLE` in
`crosscheck.py` is now 10 s for that reason.

The workable division of labour from an unelevated session: start BTVS by hand,
elevated, and leave it running. **Note the `&`** — in PowerShell a quoted
executable path without the call operator is parsed as a string, so the flags
never reach the program; it starts with its defaults instead, and `-Remote`
defaults to **off**. The symptom is a BTVS window that looks fine and no
listener on the port, which is indistinguishable from "the capture is broken"
unless you know to check:

```powershell
& "C:\BTP\v1.14.0\x86\btvs.exe" -Mode Wireshark -Remote on
```

Its full option set, from the binary's own usage string, is
`[-Mode Frontline|Ellisys|Wireshark] [-Address 127.0.0.1] [-Port 24352]
[-Service 1|2|3] [-Remote off|on]`. **Only one instance should be running.**
Check with `Get-Process btvs` before starting another: a stale one holds the
radio, and the new one's `listen` then fails while its window still opens.

— then run `crosscheck.py --capture attach`, or
`python tests/hil/tools/hci_capture.py --seconds 10` first to check in ten
seconds that packets are arriving at all, rather than finding out after a
five-minute arm. The script also spawns BTVS itself when it *is* elevated, for a
commissioned runner; that branch has never executed and says so.
