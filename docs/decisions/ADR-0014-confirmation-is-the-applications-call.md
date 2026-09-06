# ADR-0014 — Confirmation is the application's call

**Status:** Accepted (2026-09-06)

## Context

MCUboot's rollback safety net works like this: an image marked *for test* is
swapped in on the next reset and runs unconfirmed. If the device resets again
without something confirming it, MCUboot reverts to the previous image
([`../protocol-notes.md`](../protocol-notes.md) §7). The window between "the new
image is running" and "the new image is confirmed" is the only opportunity
anybody has to decide the update actually worked.

[`../design.md`](../design.md) §8 originally spent that window on nothing:
`Confirming` followed `VerifyingBooted` automatically, so `FirmwareUpdater`
confirmed the moment it saw the device had booted the new image. The other mode
it offered, `UpdateMode::ConfirmImmediately`, was to skip the trial entirely by
sending `set-state{hash, confirm = true}` before the swap.

P11 read the server implementation and found that second mode cannot work.
`img_mgmt_set_next_boot_slot()` refuses a confirm on any slot that is not the
running one, with `IMG_MGMT_ERR_IMAGE_CONFIRMATION_DENIED`, unless the firmware
was built with `CONFIG_MCUMGR_GRP_IMG_ALLOW_CONFIRM_NON_ACTIVE_SLOT` — which is
off by default (§7). The mode was a documented API that fails on ordinary
hardware.

That left `ConfirmImmediately` with no meaning, and exposed a second problem:
the two remaining modes were indistinguishable. `TestThenConfirm` confirmed
automatically, so it was *also* "confirm immediately" — just via a longer route.
The name promised a test that nothing performed.

## Decision

**The default mode stops after verifying the boot and hands the decision to the
application.**

* `UpdateMode::TestThenConfirm` (default) runs upload → mark for test → reset →
  reconnect → verify booted, then enters a new state `AwaitingConfirmation` and
  emits `UpdateEvent::Kind::ConfirmationRequired`. The update advances only when
  the application calls `FirmwareUpdater::confirm()`.
* `UpdateMode::ConfirmImmediately` runs the identical sequence and confirms
  without asking. It is the honest version of the old flow, and it is what an
  unattended updater wants.
* `UpdateMode::UploadOnly` is unchanged.

An application that never confirms is not a hang: it may `cancel()`, and either
way the device reverts on its next reset. The terminal `UpdateReport` says so
explicitly, because "the update is finished and the device will undo it" is a
state a caller must be able to distinguish from success.

## Alternatives considered

**Drop `ConfirmImmediately` and keep confirming automatically.** Smaller API,
and nothing that misleads. Rejected because it permanently forecloses the
validation window: an application that wants to run a self-test before
committing would have to abandon `FirmwareUpdater` for `UploadOnly` and drive
set-state, reset and the reconnect protocol itself — which is most of what this
class exists to do.

**Attempt the permanent swap and fall back on `CONFIRMATION_DENIED`.** Keeps the
mode's original intent and works on a build with the Kconfig set. Rejected
because on every ordinary device it spends a round trip on a command designed
to fail. Shipping a documented mode whose normal path is an error response
teaches callers the wrong thing about the protocol.

## Consequences

* `FirmwareUpdater` gains one public method (`confirm()`), `UpdateState` one
  value (`AwaitingConfirmation`) and `UpdateEvent::Kind` one (`ConfirmationRequired`).
* **The default mode does not finish on its own.** Every caller of the default
  must handle `ConfirmationRequired` or the update sits in
  `AwaitingConfirmation` until it is cancelled. This is deliberate — an
  unattended caller should be choosing `ConfirmImmediately` and saying so — but
  it is the one place where the obvious default is not the passive one, and
  `api.md`'s example shows the handler.
* The rollback net still protects the un-validated case in either mode: an
  image that fails to boot never reaches `VerifyingBooted`, so it is never
  confirmed, so MCUboot reverts.
* [`../design.md`](../design.md) §8's diagram, mode list and failure table, and
  the `smply/dfu/firmware_updater.hpp` section of [`../api.md`](../api.md), are
  updated in the same change. This ADR supersedes nothing; it decides a question
  those documents had answered only by omission.
