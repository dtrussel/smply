<!-- SPDX-License-Identifier: Apache-2.0 -->

# P17 hardware interoperability (`tests/hil/`)

**Status: bring-up in progress.** Nothing here is part of the PR gate, and
`SMPLY_BUILD_HIL` stays `OFF` in every normal build. The sections below are kept
true as the phase advances; the roadmap's P17 entry says which cases have passed.

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
  on the air, and infers nothing about the protocol from them (ADR-0015).

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

## HCI capture

Microsoft's Bluetooth Test Platform (BTP 1.14.0) is installed at
`C:\BTP\v1.14.0\x86\btvs.exe`; with `-Mode Wireshark -Remote on` it exposes the
host controller's HCI traffic for `tshark -i TCP@127.0.0.1:24352`. **`btvs.exe`
requires an elevated shell.** A capture with zero packets is not evidence of
anything; check the packet count before trusting one.
