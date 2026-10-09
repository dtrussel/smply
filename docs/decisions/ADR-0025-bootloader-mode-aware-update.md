# ADR-0025 — The updater follows the MCUboot mode the device reports

**Status:** Accepted (2026-10-09)

## Context

`FirmwareUpdater` was designed around one MCUboot behaviour: swap with revert.
It uploads into the secondary slot, marks the image for test, resets, verifies
that the device booted the new image on trial, and confirms it. The rollback
safety net in [`../protocol-notes.md`](../protocol-notes.md) §7 is the reason
`UpdateMode::TestThenConfirm` is the default.

MCUboot has ten modes (§7, S56), and only four of them behave like that:

* **Direct-XIP without revert, RAM load and the firmware loader** have no
  set-state handler (S10). The update fails at `MarkingForTest`, after the
  whole image has been sent.
* **Upgrade-only (overwrite)** accepts the test request and then copies the
  image permanently. There is no trial and no revert, but smply reports an
  ordinary test-then-confirm. The default mode's promise is broken without a
  word.
* **Downgrade prevention** erases an older image at boot (S41, S56). smply
  learns about it only after a full transfer and a reset, as a revert.
* **Direct-XIP** uploads into the slot opposite the active one, and the file
  must be linked for that slot (§7, S10). An nRF Connect SDK package carries
  one file per slot (S58), and the package reader refuses it.

Since Zephyr added OS command 8 (bootloader information, §5), a device can
report its mode and its downgrade prevention. The comparison clients that
drive a full update use it: Nordic's nRF Connect Device Manager changes its
upgrade mode on direct-XIP, and mcumgr-toolkit detects the bootloader before
anything else. They were read only to compare behaviour.

## Decision

**1. The updater asks, and the device's answer wins.** A new
`QueryingBootloader` state sends command 8 with `query: "mode"`, after the
parameters query and before the first image-state read.
* A reported mode is used as reported.
* `UpdatePlan::fallback_mode` is used only when the device gives no answer. It
  never overrides an answer.
* With neither, the mode is unknown.
* `UpdateReport` records the mode and where it came from (`Reported`,
  `Supplied`, `Assumed`).

**2. An unknown mode updates as today.** That covers no answer (`ENOTSUP`, any
device `rc`, a bootloader that is not MCUboot) and `-1`, which Zephyr also
reports for its RAM-load builds (A38). A timeout or transport failure on the
query fails the update, with nothing on the device changed, as a failed
image-state read already does.

**3. Every check that can refuse runs before the first upload request.** A
refusal ends the update with one new `ErrorCode::UpdateRefused`, and
`UpdateReport::refusal` names the precondition that failed.

**4. Behaviour by mode.**

| Mode | Behaviour |
| ---- | --------- |
| swap using scratch, move or offset | unchanged |
| upgrade-only | refused (`RevertUnavailable`) unless `UpdatePlan::allow_no_revert`; then as today |
| direct-XIP | refused (`RevertUnavailable`) unless `allow_no_revert`; then upload, reset, and success only if the running slot holds the target hash. No set-state is sent |
| direct-XIP with revert | upload into the free slot, test, reset, confirm |
| direct-XIP, more than one image | refused (`MultiImageUnsupported`) |
| single slot, firmware loader, RAM load, single-slot RAM load | refused (`UnsupportedMode`) |

Neither direct-XIP variant waits for a swap. A device that boots the old image
after a direct-XIP update is reported as `rolled_back`, whose meaning becomes
"the bootloader booted the old image".

**5. Downgrade prevention is checked before the transfer.** When the device
reports `no-downgrade`, an image whose `major.minor.revision` is strictly lower
than the running image's is refused (`Downgrade`). The build number is ignored,
so the check never refuses an image the device would accept.
`UpdatePlan::check_downgrade = false` turns it off.

**6. A direct-XIP image may be given as one file per slot.** An `ImageTarget`
can carry two alternative sources, and the updater sends the one for the free
slot. The image counts as already present when either alternative's hash is in
its slot. `support/dfu_package` returns a plain direct-XIP package as one image
with two alternatives keyed by slot.

**This is consistent with ADR-0009.** smply still does not implement swap,
revert or trailer logic. It avoids commands the reported mode cannot carry out,
and compares the header version it already parses with the version the device
already reports, to pre-empt a known rejection. That is the same reason ADR-0009
gives for checking the header magic.

**This is consistent with ADR-0014.** Where a trial exists, confirmation stays
the application's call. Where none exists, the application opts in to that
explicitly.

## Alternatives considered

**The caller always supplies the mode.** No extra round trip, and it works on
devices without command 8. Rejected because a wrong setting sends exactly the
wrong commands, and the device knows the answer. It survives as the fallback.

**A caller-supplied mode overrides the device.** Rejected for the same reason:
the only time an override matters is when it contradicts the device, and then
it is the override that is wrong.

**Refuse an unknown mode.** Safest in principle. Rejected because it breaks
every device that updates today without command 8, and Zephyr's own RAM-load
builds, which report `-1`.

**Report the mode, but never refuse.** Keeps every existing flow working.
Rejected because an overwrite-only update would still be reported as a
test-then-confirm, the very gap this ADR exists to close.

**One `ErrorCode` per refusal.** Rejected. `ErrorCode` says what kind of failure
happened ("refused before sending anything"). Which precondition failed is
detail, and belongs in the report beside the mode.

## Consequences

* **Behaviour changes for overwrite-only devices.** The default
  `TestThenConfirm` is refused there until the caller sets `allow_no_revert`.
  That is a breaking change, made in 0.3.0 under ADR-0016's `0.x` rule and
  named in the changelog.
* Every update costs one more round trip: the bootloader query.
* Direct-XIP products can be updated, from a single file or from an nRF Connect
  SDK package, without the late failure at set-state.
* Threat T8 ([`../security.md`](../security.md)) gains a client-side check.
  The device's own check stays authoritative, and hardware downgrade prevention
  by security counter is not reported, so smply does not check it.
* `ServerSimulator` models each mode in scope. Until a direct-XIP device is on
  the bench, that is the only evidence, and the roadmap says so.
* A Zephyr RAM-load device reports `-1` (A38), so it is treated as unknown
  and still fails at set-state, as it does today. Only a device that reports
  mode 6 is refused by name. Fixing that is Zephyr's mapping, not smply's.
* The single-slot, firmware-loader and RAM-load update paths, and the QSPI
  split-image direct-XIP package, are refused by name and left as backlog rows.
