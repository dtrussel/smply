# ADR-0015 — Hardware evidence and isolated bench tooling

**Status:** Accepted (2026-09-08)

## Context

P15b (`transports/winrt_ble/`) and P16 (`examples/winrt_ble_dfu/`) were compiled
by CI at `/W4 /WX` and never executed: a GitHub runner has no radio. P17 is where
that code first meets a Zephyr device, and where the roadmap's case list is run
against real hardware. Three things have to be decided before that work can be
trusted:

* what counts as **evidence**. A green compile, an accepted reset command, or a
  process that a supervisor killed on a timeout is not hardware acceptance;
* what the bench **depends on**, and how none of it leaks into the library;
* what a **third-party client** may be used for. The roadmap's original
  acceptance named `mcumgr-client`, which Zephyr's own tool inventory lists as a
  serial-only third-party application, so the wording had to be corrected.

The bench is a NUCLEO-WB55RG on its ST-LINK, driven from a Windows 11 host with
an Intel Bluetooth radio (`tests/hil/README.md`). It replaced a damaged nRF52840
DK before any hardware case had run.

## Decision

1. **The HIL executable and its supervisor are opt-in and never a PR gate.**
   `smply_hil` builds only with `SMPLY_BUILD_HIL=ON`, is absent from every CI
   preset, and its advisory nightly workflow runs on a self-hosted runner that
   is commissioned separately. Its results are reported as **pass, fail or
   unavailable**, and unavailable -- no probe, no radio, no coprocessor stack,
   no capture tool -- is never a pass.

2. **Bench tooling is a development dependency of the bench, not of smply.**
   Zephyr, MCUboot, hal_stm32, the Zephyr SDK, west, STM32CubeProgrammer, the
   STM32CubeWB coprocessor binaries, pyserial, bleak, imgtool, `mcumgr-client`,
   `smpmgr`/`smpclient`, BTVS and Wireshark are recorded in `dependencies.md`
   with their versions and licences and are frozen per run in the evidence
   bundle. Nothing in `include/smply/`, `src/` or the installed package refers
   to any of them, and `tools/check_deps.py`'s inventory does not change.

3. **Third-party clients are for behavioural comparison and as an independent
   oracle, never as a protocol reference.** `smpmgr` over BLE runs the same
   sequence as smply so image state and HCI captures can be compared;
   `mcumgr-client` over the UART shell transport reads device state through a
   path smply does not touch, which is what makes it an oracle. Neither is a
   source of protocol truth or of copied code; every divergence is traced to
   Zephyr or MCUboot source and recorded in `protocol-notes.md` §9 before any
   behaviour changes (CLAUDE.md rule 5).

4. **The firmware is the unmodified pinned `smp_svr` sample plus explicit
   configuration**, built twice by Zephyr's own signing step so the two images
   differ only in signed version. Faults are injected from the client side or by
   altering the *file* (a corrupted image), never by changing the server's SMP
   handlers to make a case pass. MCUboot's public development key is used
   because this is a dedicated bench; the bootloader still verifies signatures.

5. **Recovery is a full reflash over ST-LINK, verified by read-back**, and it
   runs before every case. Recovery is what makes the case list unattended; it
   is never itself counted as success, and the supervisor preserves a failing
   case's evidence before it recovers.

6. **Assumption updates are the phase's primary deliverable.** A blind-written
   adapter meeting a device will invalidate guesses; each one becomes a
   protocol-notes entry (and an ADR when behaviour changes), a fix, and where a
   portable seam exists, a unit test -- the P17a findings A18 and A19 set the
   pattern.

## Consequences

* The first deliverable was a manual update and the defects it found, not a
  harness (P17a). The case suite (P17b) and the cross-check (P17c) build on a
  transport that has demonstrably worked.
* Everything in the roadmap's case list runs without a person present, because
  the ST-LINK provides a hard reset and a reflash. Physical power loss and RF
  interference remain outside the list.
* Firmware and tool binaries stay in build or evidence directories; only
  reproducible inputs (`tests/hil/firmware/`) and concise results live in the
  repository.
* HCI capture on this host needs an elevated shell for BTVS, which an unattended
  runner must be configured to provide; a run without a capture reports the
  capture as unavailable rather than silently omitting it.
