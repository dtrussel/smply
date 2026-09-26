# ADR-0022 — A device-committed image stays on trial until the client images are confirmed, and the wait survives the link

**Status:** Accepted (2026-09-26). Supersedes ADR-0021's decision 2 on what
"applied" means, and decision 3 on the order of commits. ADR-0021's other
decisions stand.

## Context

ADR-0021 lets one update carry an image the device commits itself: the H5
stages a BL54L10's firmware as its image 1, applies it after the reset, and
commits it. smply confirms image 0 only once the device reports image 1
**committed**.

A review against the product's architecture document, *Dual-MCU firmware
update over one BLE link*, found three problems with that.

1. **The order admits a mismatched pair.** The BL54L10 is committed before
   anyone has validated the pair. If the application then declines to confirm
   the new H5 image, the H5 reverts on its next reset and the BL54L10 stays
   new. The document's rule is the opposite: commit both only after the pair
   passes.
2. **The BLE link *is* the BL54L10.** While the controller is being updated,
   the link to smply is gone. smply's wait for the apply treated a failed read
   as fatal, so on the product's own link the update would fail every time
   the controller went down.
3. **Image index 1 carries a cost ADR-0021 did not record.** The document
   advises against overloading image-index semantics and suggests a custom
   SMP group. ADR-0021 weighed an opaque file, but never a custom group, and
   did not say what the H5's own bootloader must do about an image 1 that is
   not its own.

Primary sources checked for this decision (protocol-notes S39–S41):
* Zephyr calls `MGMT_EVT_OP_IMG_MGMT_DFU_CONFIRMED` with the confirmed image's
  number once a confirm has been written, under
  `CONFIG_MCUMGR_GRP_IMG_STATUS_HOOKS`. So the H5 can act on smply's confirm.
* MCUboot offers image-access hooks under `MCUBOOT_IMAGE_ACCESS_HOOKS`,
  among them `boot_image_check_hook` and `boot_perform_update_hook`. They let
  a bootloader leave an image slot alone.
* MCUboot compares versions by major, minor and revision, and by the build
  number only when built with `MCUBOOT_VERSION_CMP_USE_BUILD_NUMBER`.

## Decision

**1. "Applied" means running on trial, not committed.** For a `Device`
image, slot 0 must report what the second MCU **actually runs**, and the
contract gains a state:

| When | slot 1 | slot 0 |
| ---- | ------ | ------ |
| After the mark, and while applying | the new image, `pending` | the old image |
| **Applied, on trial** | not pending | **the new image, not confirmed** |
| **Committed** | anything | **the new image, `confirmed`** |
| Apply failed | the new image, not pending | the old image |

**2. The device commits its image when smply confirms the client images.**
On `DFU_CONFIRMED` for image 0, the H5 confirms the BL54L10 through its SMP
client, then reports slot 0 confirmed. If image 0 is not on trial when the
apply finishes (the package did not change it, so no confirm will come), the
H5 commits right after the apply. smply still sends exactly one kind of
confirm, for `Client` images. The pair commit belongs to the one device that
sees both MCUs at every boot, as ADR-0021 intended.

**3. smply waits for the commit.** After the client images read confirmed, a
new state, `AwaitingDeviceCommit`, polls until every `Device` image reads
confirmed, bounded by `UpdatePlan::apply_timeout`, which is re-armed for
this phase. `ImageReport::committed` records it. `Completed` therefore means
the whole set is committed. A commit that never comes fails the update with
`revert_pending` false: image 0 is already confirmed, so nothing reverts,
and the device's boot-time logic owns the rest.

**4. The waits survive the link.** In `AwaitingDeviceApply` and
`AwaitingDeviceCommit`:
* a read that fails with `Disconnected` asks the application to reconnect
  (`ReconnectRequired`), and the deadline keeps running;
* a read that times out is retried at the next poll;
* any other error is still fatal.

After the reconnect, `VerifyingBooted` re-reads the device and the machine
returns to whichever wait the table calls for.

**5. The BL54L10 stays image index 1.** The custom-group alternative is
recorded below, as is the cost on the H5 of keeping image 1.

**6. A package whose own images disagree is refused.** `read_package()`
refuses a package in which an image declares a dependency on another image
the package carries, at a lower version than the minimum. The comparison is
MCUboot's default one: major, minor, revision, and no build number. So the
check never refuses what a default bootloader would accept.

## Alternatives considered

* **Keep the order, and have the H5 restore the old BL54L10 image if image 0
  reverts.** smply would not change. It was rejected because the mismatch
  window stays open until the restore, and the H5 would have to keep the old
  controller image. Under the chosen order it always still holds the *new*
  one, so recovery is "re-apply or commit", never "restore".
* **smply confirms image 1 as well**, as a `Client` image the H5 forwards.
  Two confirms from the host cannot be atomic. That is the host-owned
  invariant ADR-0021 rejected.
* **Report the commit without waiting for it.** `Completed` would then mean
  "image 0 confirmed, the controller perhaps still on trial". An application
  would show success too early.
* **Only document that the H5 must not start BLE until the controller is
  back.** It is fragile: any timing slip fails the whole update.
* **A custom SMP group (≥ 64) for the second MCU**, as the architecture
  document suggests. The semantics would be clean, and the H5's bootloader
  would never see image 1. It was rejected because upload, state and confirm
  would be re-implemented on both sides, and no standard tool (mcumgr, nRF
  Connect Device Manager) could drive it. The price of keeping image 1 is on
  the H5 and is now written down: its MCUboot must leave image 1 alone. That
  is either through the image-access hooks, as nRF Connect SDK does for the
  nRF5340 network core, or by building MCUboot for one image and letting the
  H5 application own image 1's slot and trailer.

## Consequences

* `UpdateState` gains `AwaitingDeviceCommit`, and `ImageReport` gains
  `committed`. Both join the unreleased 0.x break of ADR-0021.
* The device contract in `multi-image.md` changes. No H5 implements it yet,
  so the cost is documentation.
* **The variant where the H5's own MCUboot swaps image 1** is allowed only if
  the H5 reports the second MCU's real state in slot 0. Otherwise "on trial"
  and "still applying" look the same.
* A BL54L10 reset during the confirmation window reverts its trial. That is
  safe, because the H5 still holds the new image and re-applies it, but an
  application should not leave the window open for long.
* With the link able to drop during the apply, the application's reconnect
  policy bounds the outage, not `apply_timeout`. It must be sized for the
  controller's UART transfer and reboot.
* A correction to ADR-0021's text, recorded here because accepted ADRs are
  immutable: its consequence line saying that an absent image "fails cleanly
  in Planning" was edited in place during stage B. The fact is that it fails
  on the first upload packet with `NoFreeSlot`, because the listing omits an
  absent image and an empty one alike.
