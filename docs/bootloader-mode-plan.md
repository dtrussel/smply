<!-- SPDX-License-Identifier: Apache-2.0 -->

# MCUboot-mode-aware updates

Status: ready for implementation

Written 2026-10-09. The roadmap's "In progress" section points here. Work the
stages in order. Each stage is checked off in this file in the commit that
lands it, and the file is deleted with the last one
([`agents/issue-tracker.md`](agents/issue-tracker.md)). The decisions below
were settled in a `/grill-with-docs` session. The first stage writes them
down as a new ADR, before any code changes.

## Problem Statement

An application that updates a device with smply cannot tell which MCUboot
mode the device runs, and nor can smply. `FirmwareUpdater` assumes every
device swaps with revert, so it uploads, marks the image for test, resets and
confirms. That assumption is wrong in three ways:

* **On a direct-XIP device without revert, a RAM-load device, or a firmware
  loader, the update fails only after the whole image has been sent.** Zephyr
  builds these devices without the set-state command (protocol-notes §6, S14).
* **On an overwrite-only device the update is permanent, yet smply reports it
  as a test-then-confirm.** `UpdateMode::TestThenConfirm` promises a trial boot
  and a revert, and the device has neither. Nothing tells the caller.
* **On a device with downgrade prevention, an older image is uploaded in full
  and only then refused.** MCUboot erases it at boot, and smply reports a
  revert. Threat T8 in [`security.md`](security.md) is exactly this case.

A direct-XIP product cannot be updated from an nRF Connect SDK package at
all. The package lists one file per slot, and `dfu_package` refuses two files
for one image ([`multi-image.md`](multi-image.md)).

The comparison clients in Zephyr's tools table that drive a full update ask
the device first. Nordic's nRF Connect Device Manager queries the mode before
every upload and changes its flow to match. mcumgr-toolkit detects the
bootloader as its first step and refuses one it does not know. Both were read
only to compare behaviour (CLAUDE.md rule 5).

## Solution

smply asks the device which bootloader it runs and in which mode, using OS
command 8 (bootloader information) with the query `mode`. `FirmwareUpdater`
then never sends a command that mode cannot carry out, and never reports a
safety property the mode does not have:

* A swap mode (scratch, move, offset) updates exactly as today.
* A device that does not answer, or answers "unknown", updates as today. The
  report says the mode was assumed rather than reported. A plan can name a
  fallback mode for this case.
* A mode that cannot revert (overwrite-only, direct-XIP without revert) is
  **refused before anything is sent**, unless the plan explicitly accepts
  that no revert is available. Direct-XIP without revert then runs as
  upload, reset, and a check that the device runs the new image.
* Direct-XIP with revert uploads to the free slot, then tests, resets and
  confirms as usual.
* A multi-image plan sent to a direct-XIP device is refused, because Zephyr
  does not support that combination.
* Single-app, firmware loader, RAM load and single-slot RAM load are refused
  by name. Each needs an update path of its own.
* With downgrade prevention reported, an image whose version is strictly
  lower than the running one is refused before it is sent, unless the plan
  turns the check off.

Every refusal happens before the first byte of the image is sent, and says
which precondition failed. The package reader accepts a plain direct-XIP
package, and the update picks the file built for the device's free slot.

## User Stories

1. As an application developer, I want smply to ask the device which bootloader it runs, so that I don't have to configure it per product.
2. As an application developer, I want to read the bootloader's name, mode and downgrade-prevention flag myself through the OS group, so that I can show or log them without running an update.
3. As an application developer, I want a mode value smply doesn't know to keep its number, so that a newer MCUboot doesn't turn into an error or a silent guess.
4. As an application developer, I want an update to a swap-mode device to behave exactly as it does today, so that nothing changes for the common case.
5. As an application developer, I want an update to a device that doesn't support the bootloader query to behave as it does today, so that older Zephyr builds keep working.
6. As an application developer, I want the update report to say whether the mode was reported by the device, supplied by me, or assumed, so that I know how much to trust what the update did.
7. As an application developer, I want to supply a fallback mode for devices that don't answer, so that I can describe a product whose bootloader I know but whose firmware doesn't report it.
8. As an application developer, I want the device's real answer to win over my fallback, so that a wrong configuration can't make smply send the wrong commands.
9. As an application developer, I want `TestThenConfirm` refused on an overwrite-only device before the upload, so that I'm never told an update can revert when it can't.
10. As an application developer, I want to accept "no revert available" explicitly in the plan, so that I can still update an overwrite-only or direct-XIP-without-revert device once I've decided that is acceptable.
11. As an application developer updating a direct-XIP device without revert, I want smply to upload, reset and then check the running image by hash without sending set-state, so that the update succeeds where today it fails after the upload.
12. As an application developer updating a direct-XIP device with revert, I want the usual test, reset and confirm sequence, so that I keep the trial boot this mode supports.
13. As an application developer updating a direct-XIP device, I want to give one image as two files, one built for each slot, so that smply can send the one that matches the free slot.
14. As an application developer, I want the report to say which slot received the image, so that I can diagnose a direct-XIP image linked for the wrong address.
15. As an application developer updating a direct-XIP device with one file, I want smply to upload it as given and record the slot, so that I can still update a device when I built only the right variant.
16. As an application developer, I want a direct-XIP update to count as already present when either of my two files' hashes is already on the device, so that a repeated update is a no-op.
17. As an application developer, I want a multi-image plan refused on a direct-XIP device before the upload, so that I don't upload something the device can't apply.
18. As an application developer, I want single-app, firmware-loader and RAM-load devices refused by name, so that I know the update path is unsupported rather than broken.
19. As an application developer, I want an older image refused before the upload when the device reports downgrade prevention, so that I don't spend a full transfer on an image the bootloader will erase.
20. As an application developer, I want the downgrade check to compare only major, minor and revision, with an equal version accepted, so that it never refuses an image the device would accept.
21. As an application developer, I want to turn the downgrade check off in the plan, so that I can handle a device whose flag I know to be wrong.
22. As an application developer, I want every pre-upload refusal to share one error code, with the reason in the report, so that I can handle "refused before sending" in one place and still say why.
23. As an application developer, I want a device that boots the old image after a direct-XIP update reported as a rollback, so that I handle it exactly as I handle a revert today.
24. As an application developer, I want a timeout or lost link during the bootloader query to fail the update, so that a link that can't answer one query isn't trusted with an upload.
25. As an application developer, I want a device error during the bootloader query treated as "unknown", so that a device without the command is ordinary rather than broken (finding A8).
26. As a user interface author, I want a distinct update state while the bootloader is queried, so that I can say what the tool is doing.
27. As an application developer using an nRF Connect SDK direct-XIP package, I want the package reader to accept it as one image with two alternatives keyed by slot, so that a package update works on direct-XIP products.
28. As an application developer, I want a QSPI split-image direct-XIP package refused by name, so that I know that layout is unsupported and nothing is sent.
29. As a command-line user of `cli_dfu`, `serial_dfu` or `winrt_ble_dfu`, I want an `--allow-no-revert` flag, so that I can update an overwrite-only device after this release.
30. As a command-line user, I want a `--no-downgrade-check` flag, so that I can bypass the downgrade check when I know better.
31. As a command-line user, I want a `--fallback-mode` flag, so that I can update a device that doesn't report its mode, with the behaviour that mode needs.
32. As a command-line user, I want the tool to print the bootloader mode it found and where that came from, so that I can see why it behaved as it did.
33. As someone evaluating smply, I want `cli_dfu` against the stub device to be able to show a refused update, so that I can see the safety check without hardware.
34. As a maintainer, I want every protocol fact behind this traced to Zephyr's and MCUboot's source in protocol-notes, so that the behaviour can be checked against its primary source.
35. As a maintainer, I want the device-supplied bootloader name bounded before it is stored, so that a hostile device can't make smply allocate on its say-so (CLAUDE.md rule 6).
36. As a maintainer, I want the change of premise recorded in an ADR before any code, so that a later reader knows why the updater follows the device's mode.
37. As an existing user updating overwrite-only devices, I want the behaviour change and the flag that restores it named in the changelog, so that the upgrade to 0.3.0 doesn't surprise me.
38. As a maintainer, I want running against a real direct-XIP device listed as a bench acceptance gap, so that simulator-only coverage is never mistaken for hardware evidence.

## Implementation Decisions

**Protocol facts.** These go into `protocol-notes.md`, each with its primary source:

* The request is `{ "query": "mode" }`, group 0, command 8, read op. The response is `{ "mode": int, "no-downgrade": bool (optional, only when true) }`. With no query, the response is `{ "bootloader": text }`. Sources: Zephyr's group 0 specification and `os_mgmt_bootloader_info()`. The command needs `CONFIG_MCUMGR_GRP_OS_BOOTLOADER_INFO`.
* The mode values are MCUboot's `enum mcuboot_mode`, which runs from 0 to 9: single slot, swap-scratch, upgrade-only, swap-move, direct-XIP, direct-XIP with revert, RAM load, firmware loader, single-slot RAM load, swap-offset. Zephyr's specification table stops at 6 and calls 3 "swap without scratch"; the enum is the authority. Zephyr reports `-1` for any mode it doesn't map, and **it doesn't map RAM load**.
* Set-state has no handler under `DIRECT_XIP`, `RAM_LOAD` and `FIRMWARE_UPDATER`. The "with revert" variants keep it.
* Under direct-XIP an upload goes to the slot opposite the active one. The device checks the image's address only when built with `REJECT_DIRECT_XIP_MISMATCHED_SLOT`. Zephyr's image group doesn't support multi-image under direct-XIP or RAM load.
* `no-downgrade` is a hand-set application Kconfig, documented as mirroring MCUboot's software downgrade prevention. That compares `major.minor.revision`, and the build number only under a build option the device doesn't report. An equal version is accepted.
* An nRF Connect SDK direct-XIP package lists two files for one `image_index`. Each has `slot` as a global slot index (`image × 2 + 0/1`), its own `load_address`, and `version_MCUBOOT+XIP`. The QSPI split-image layout lists four files.

**ADR.** A new ADR, written first with `Status: Proposed`, records that the updater follows the device's reported mode. It states that this is consistent with ADR-0009 (smply still doesn't reimplement MCUboot's logic, it only avoids commands the mode can't carry out) and with ADR-0014 (confirmation stays the application's call). The alternatives it rejects: a caller-supplied mode only, and a caller override of the device's answer.

**OS group.**
* A new `OsManagement::bootloader_info()` returns `BootloaderInfo { name, mode, raw_mode, no_downgrade }`.
* `McubootMode` is an enum of MCUboot's ten values plus `Unknown`. A value outside the enum maps to `Unknown`, and `raw_mode` always carries the number (A2).
* `name` gets a bound in `limits.hpp`, checked before anything is stored.
* The name and the mode come from two requests, one with no query and one with `mode`, because the specification gives them separate response shapes. Whether that is one public call or two is decided at implementation.

**`FirmwareUpdater` interface.**
* `UpdatePlan` gains `fallback_mode` (optional, used only when the device gives no answer), `allow_no_revert` (default false) and `check_downgrade` (default true).
* `UpdateState` gains `QueryingBootloader`, after `QueryingParameters`.
* `UpdateReport` gains `bootloader_mode`, `mode_source` (`Reported`, `Supplied`, `Assumed`), `upload_slot`, and `refusal` (`RevertUnavailable`, `Downgrade`, `UnsupportedMode`, `MultiImageUnsupported`).
* `ErrorCode` gains one value, `UpdateRefused`: the updater refused before transferring anything. `refusal` says why.
* `rolled_back` is widened to "the bootloader booted the old image", which covers a direct-XIP device that rejected the new image.
* `ImageTarget` can carry two alternative sources, one per slot. One source behaves as today.

**Update state machine.**
* The bootloader query runs after the parameters query and before the image-state read.
* A device error on the query (`ENOTSUP` or any `rc`) means unknown. A timeout or transport failure fails the update, with nothing changed on the device.
* The refusal checks run in planning, once the image state is known: the mode, the image count, and the version against the running image. Every refusal comes before the first upload request.
* Direct-XIP picks the alternative whose slot is the free one. "Already present" holds when either alternative's hash is in its slot.
* Direct-XIP without revert (when accepted) skips the test mark and the confirm. Success is the running slot holding the target hash after the reset.
* Direct-XIP with revert runs the usual sequence. Neither direct-XIP variant waits for a swap; the existing disconnect grace still bounds the reset.

**Package reader and `PackageUpdate`.**
* `dfu_package` accepts a plain direct-XIP package (two files for one image, each with `slot`), and returns one `PackageImage` with two alternatives keyed by slot.
* `PackageUpdate` builds the two-source target from it.
* The QSPI split-image layout stays refused, with a message naming it.

**Tools.**
* `cli_dfu`, `serial_dfu` and `winrt_ble_dfu` gain `--allow-no-revert`, `--no-downgrade-check` and `--fallback-mode <mode>`, and print the mode and its source.
* The stub device answers command 8 with a mode given on its command line.
* `winrt_ble_dfu` is checked by the Windows CI job only.

**Documentation in the same change** (ADR-0013): `api.md`, `design.md`, `architecture.md`, `security.md` (T8's mitigation), `multi-image.md` (direct-XIP packages), `protocol-notes.md`, and `CHANGELOG.md`. The release is 0.3.0. The overwrite-only refusal goes under "Changed", with `allow_no_revert` and `--allow-no-revert` named as the way back to the old behaviour.

## Testing Decisions

A good test drives the public interface and checks what a caller can see: the commands the device receives, the states and events the updater emits, and the final report. It never checks the updater's internal steps. A refusal test asserts that **no upload request reached the device**, as well as the error and the reason.

* **`FirmwareUpdater` against `ServerSimulator`**, the main seam: every mode's behaviour is tested here. That covers each swap mode unchanged; unknown and unanswered with and without a fallback; each refusal with and without its opt-in; direct-XIP without revert succeeding with no set-state; direct-XIP with revert; slot selection between two alternatives; already-present with alternatives; the downgrade check at lower, equal and higher versions; a query timeout; and a direct-XIP device that boots the old image. `ServerSimulator` gains a configured mode, with the behaviours it implies: command 8's answer, set-state `ENOTSUP` where Zephyr removes it, uploads to the opposite slot under direct-XIP, and a permanent test under overwrite-only. Prior art: the component update suite's cases for a revert, a refused confirm and a device without mcumgr parameters.
* **OS group against fixed message bytes:** the wire format of both requests, both response shapes in definite- and indefinite-length CBOR (A18), an unknown mode number, a missing `no-downgrade`, an over-long name, and the error shapes. Prior art: the existing OS group unit suite and its message builder.
* **Package reader and `PackageUpdate`:** a direct-XIP package read as two alternatives, the split-image layout refused, and the existing duplicate-image refusal kept for anything that isn't a direct-XIP pair. Prior art: the package reader and package update unit suites, with the zip builder.
* **Examples' ctests against the stub device:** an overwrite-only stub refused by default, and updated with `--allow-no-revert`. Prior art: `cli_dfu`'s and `serial_dfu`'s existing ctests.
* **Not tested here:** a real direct-XIP device. That becomes an acceptance gap under the roadmap's hardware-bench section.

## Out of Scope

* The FS, Shell, Enum and Zephyr-basic management groups.
* Update paths for single-app, the firmware loader, RAM load and single-slot RAM load. They are refused by name, each with a backlog row.
* The QSPI split-image direct-XIP package. It is refused by name, with a backlog row.
* MCUboot serial recovery, which is already a backlog row.
* Hardware downgrade prevention by security counter. The device reports only the software flag.
* Checking a single file's link address against the slot. SMP never reports slot addresses.
* `GLOSSARY.md`. "Bootloader mode" is defined in protocol-notes and `api.md`, so the backlog row about `check_docs.py` R6 and `GLOSSARY.md` is not triggered.

## Further Notes

* The behaviour change for overwrite-only devices is deliberate. It is the reason for the 0.3.0 minor bump allowed by ADR-0016.
* The comparison clients (Nordic's Device Manager, mcumgr-toolkit, smpclient) were read only to compare behaviour. Every fact above is traced to Zephyr, MCUboot or the nRF Connect SDK source.
* `docs.zephyrproject.org` is blocked from the agent container's network, so the specification was read from its source in the Zephyr repository, which is the same text.
