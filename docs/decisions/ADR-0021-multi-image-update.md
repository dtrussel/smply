# ADR-0021 — Several images on one device in one update, with per-image commit ownership

**Status:** Accepted (2026-09-25). Resolves roadmap O5.

## Context

`UploadOptions::image` has carried an image number since the upload was first
written, and nothing had ever exercised image ≥ 1 (roadmap O5). A real product
now needs it. An STM32H5 runs Zephyr, MCUboot and a BLE host. An Ezurio
BL54L10 runs Zephyr, its own MCUboot and a BLE controller on the H5's UART.
Both must be updated, and "safely" means three things:
* each MCU can roll back;
* the pair is never left mismatched;
* an interrupted update can be resumed.

Two designs were weighed.

* **A coordinator in smply, over two devices.** The host talks to each MCU and
  runs a two-phase commit across them. It is rejected. Confirms on two devices
  cannot be atomic, so the host would own an invariant it can only enforce
  while it is running and connected. A PC that vanishes between the two
  confirms leaves the pair mismatched, and so does a different client (nRF
  Connect Device Manager, `mcumgr`) that never ran the coordinator.
* **The H5 coordinates.** smply delivers a package of two signed images to the
  H5. The H5 stages the BL54L10's image as its own **image 1**. After the reset
  it updates the BL54L10 over UART with Zephyr's upstream SMP client
  (`CONFIG_SMP_CLIENT`, `CONFIG_MCUMGR_GRP_IMG_CLIENT`), which keeps the
  BL54L10's own MCUboot test-and-revert. The H5 sees both MCUs at every
  boot, so it is where "never mismatched" and "resume" can hold without the
  PC. This is the pattern the nRF5340's network core already uses.

The second puts smply's work in one place: **one device, several images, one
reset**. What the protocol does with several images is in protocol-notes §6
("Several images on one device"), read from Zephyr's `img_mgmt` and
MCUboot's loader.

## Decision

**1. `FirmwareUpdater` updates several images of one device in one update.**
A new `start()` overload takes a list of `ImageTarget { image, source,
commit }`. The existing single-image `start()` becomes that list with one
entry, so existing callers see no change. The flow:
1. Stage every image: plan, upload, verify it is present, and mark it for
   test.
2. Reset **once**.
3. Verify the images.
4. The application's confirmation window (ADR-0014, unchanged).
5. Confirm.

Every image is marked before the reset because MCUboot evaluates the
dependency TLVs at that boot. An image whose dependency is not yet staged
has its TEST swap downgraded to NONE and is not booted at all (protocol-notes
§6).

**2. Commit ownership is per image: `Client` or `Device`.**
* A `Client` image is committed by smply: test, reset, verify it booted,
  confirm. That is today's flow.
* A `Device` image is committed by the device. smply stages and marks it,
  then waits in a new state, `AwaitingDeviceApply`. It reads image state on
  an interval until the image's primary slot reports the target hash,
  confirmed, or until a timeout.

What "applied" looks like on the wire is the **device contract** in
[`../multi-image.md`](../multi-image.md). smply only reads the standard
image-state listing, so the contract costs the H5 a slot-state hook, not a new
protocol.

**3. Order: `Client` images are confirmed only after every `Device` image is
reported applied.** If a `Device` image fails or times out, the `Client`
images are left unconfirmed, and the update fails with `revert_pending`. The
device reverts them on its next reset, and the device's own boot-time logic
decides what to do about the other MCU. smply never confirms half a package.

**4. The confirm names its image by hash.** A hashless confirm targets the
running image (protocol-notes §6), so it can never confirm image ≥ 1, and for
image 0 it names the same slot the hash does. Every confirm now carries the
target hash.

**5. The package reader is support code, not core.** `support/dfu_package/`
reads the zip and `manifest.json` layout that nRF Connect SDK's sysbuild
writes, stored and not deflated, with an `image_index` per file. It is **not
installed**. The core stays free of file formats (O4, ADR-0016), and a
product with its own container keeps using the image-list `start()`
directly. The reader is hand-written, with no new dependency, bounded
throughout, and fuzzed.

**6. Several devices in one update stays out of scope.** It is a roadmap row,
for a product without a coordinating MCU.

## Alternatives considered

* **A coordinator over several devices in the core.** Rejected above: the
  invariant belongs to the device that can see both MCUs at every boot.
* **smply confirms image 1 by hash** (`CONFIG_MCUMGR_GRP_IMG_ALLOW_CONFIRM_
  NON_ACTIVE_IMAGE_*` on the H5). Rejected for this product, because it makes
  smply the owner of a commit whose rollback happens on another chip. Nothing
  in the design forbids it: a caller can mark image 1 `Client` on a device that
  allows it.
* **Send the package as one opaque file** (FS group) and let the H5 unpack it.
  Rejected: it needs group 8, which is a non-goal, and moves signature
  checking off MCUboot. Two signed images through the image group need
  nothing new.
* **A deflate-capable zip reader.** Rejected until someone ships a deflated
  package. The layout it would read is written stored, and deflate would
  mean a new dependency under ADR-0011.

## Consequences

* O5 is resolved. `plan.upload.image` is now honoured throughout. Pending,
  holder and active slots are scoped to it, and a plan for an image the device
  does not have fails cleanly on the first upload packet (`NoFreeSlot`): the
  listing omits an absent image and an empty one alike, so Planning cannot
  tell them apart.
* `UpdateState` gains `AwaitingDeviceApply` and `UpdateReport` gains per-image
  results. That is a source break for exhaustive `switch`es, allowed in 0.x and
  recorded in the CHANGELOG.
* **The residual risk moves to the device, where it can be handled.** If the H5
  loses power after smply confirmed image 0 but before the BL54L10 committed,
  only the H5's boot-time logic can finish or roll back. The contract says it
  must, and smply cannot.
* `ServerSimulator` models N image pairs, the confirm-denial rules and a
  device-committed image. The component suite covers each failure ordering.
