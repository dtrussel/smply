# ADR-0023 — A lost link or answer around the confirm is recovered by re-reading the device

**Status:** Accepted (2026-09-26). Extends ADR-0022's decision 4 from the two
device waits to `Confirming` and `VerifyingConfirmed`. ADR-0022's other
decisions stand.

## Context

ADR-0022 made the two device waits survive the link: a read that fails with
`Disconnected` asks for a reconnect, and one that times out is asked again.
The confirm around them did not change. A failure of any kind in
`Confirming` or `VerifyingConfirmed` ended the update:
* In `Confirming`, a dropped link or a lost answer failed the update with
  `revert_pending` set, although the device may already have written the
  confirm.
* In `VerifyingConfirmed`, a failed read-back set `revert_pending` even
  after the device had accepted the confirm.

Neither report was a fact. Both were assumptions.

Designing the dual-MCU product's controller update made the gap concrete.
The coordinating MCU commits the other MCU when smply confirms image 0 (S39).
If it takes the link down to do that before smply has read the confirm
back, smply failed an update that had succeeded. The chosen design commits
through a vendor HCI command, which keeps the link up, and `multi-image.md`
asks the device not to drop it. But a radio can drop a link on its own, and
the report should not depend on the device's timing.

smply already recovers in the same way elsewhere:
* an upload suspends on a drop and resumes (`upload_failed`);
* a reset whose answer is lost is treated as accepted (A3);
* a lost mark-for-test response is recovered by re-reading once (A24).

## Decision

**1. In `Confirming` and `VerifyingConfirmed`, a lost link or answer is
re-inspected, not fatal.**
* `Disconnected` → `AwaitingReconnect` (`ReconnectRequired`). After the
  reconnect, `VerifyingBooted` reads the device.
* `Timeout` → one re-read through `VerifyingBooted`, once per update. A
  second lost answer is fatal.
* Any other error, including a refused confirm, is fatal as before.

**2. The re-inspection decides.** It is the existing `inspect_boot`:
* a confirmed image 0 goes on:
  - with a `Device` image: to `AwaitingDeviceCommit`;
  - otherwise: to `Completed`;
* an image still on trial goes back to the confirmation fork.

**3. The application is asked once.** Once it has approved (`confirm()`),
the fork confirms again without a second `ConfirmationRequired`.

**4. Until the re-read, the swap counts as scheduled.** A failure in between
reports `revert_pending`, as before. After a successful re-read,
`revert_pending` reflects what the device reported.

## Alternatives considered

* **Leave it to the device**: require that the link stay up until the
  read-back. That is kept in `multi-image.md` as advice. On its own it is
  fragile, because a radio can drop a link whatever the firmware does.
* **Re-send the confirm on a timeout without reading.** The read is needed
  anyway to know the outcome. Reading first also avoids a second write to an
  image that may already be confirmed, or may have reverted meanwhile. How a
  device answers a repeated confirm was not checked, and this way it does
  not matter.
* **Unbounded re-reads on timeout.** Rejected: a device that has stopped
  answering would hold the update forever. Disconnects stay bounded by the
  application's reconnect policy, as they are for uploads.

## Consequences

* **Single-image updates change too.** A link lost after the confirm now
  reconnects and completes, rather than failing with a revert reported that
  may not come. The events are ones an application already handles.
* **A second `ReconnectRequired` can follow `confirm()`.** `api.md` says so.
* **The report after a lost confirm is decided by the device's own
  answer**, not by an assumption.
