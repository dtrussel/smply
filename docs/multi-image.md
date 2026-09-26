# Multi-image updates and the device contract

How smply updates several images of one device in one update, and what the
device must do for an image that **it** commits rather than smply.
[ADR-0021](decisions/ADR-0021-multi-image-update.md) is the decision, and
[ADR-0022](decisions/ADR-0022-device-images-commit-after-client.md) sets when a
device-committed image is committed and what "applied" means.
[`protocol-notes.md`](protocol-notes.md) §6 ("Several images on one device")
holds the protocol facts this relies on.

## The shape

One device, several MCUboot images, **one reset**:

```
stage every image  →  reset once  →  verify  →  wait for device-applied images
      (upload, verify present,         (Client images      (Device images running
       mark for test, per image)        booted the target)  on trial: poll state)
                    →  confirmation window  →  confirm Client images  →  wait for the
                       (the application's      (the device then commits   device's commit
                        call, ADR-0014)         its own images)           (poll state)
```

Each image is either:
* **`Client`**: smply commits it. It is marked for test, the reset swaps it in,
  smply verifies it booted, and it is confirmed by hash after the window. This
  is the ordinary single-image update, image 0 of a typical device.
* **`Device`**: the device commits it. smply stages and marks it. After the
  reset it waits, in `AwaitingDeviceApply`, for the device to report the image
  applied: running on the other MCU, on trial. smply never confirms it. The
  device commits it when smply confirms the `Client` images, and smply waits
  for that too, in `AwaitingDeviceCommit`.

**`Client` images are confirmed only after every `Device` image is running
on trial, and `Device` images are committed only after that confirm.** So
nothing is committed until the whole set has booted, and the pair is never
committed half-validated. If a `Device` image fails to apply, the update
fails, the `Client` images stay unconfirmed, and the device reverts them on
its next reset.

## Why "Device" exists: a coordinating MCU

The motivating product is an STM32H5 (Zephyr, MCUboot, BLE host) with an Ezurio
BL54L10 (Zephyr, its own MCUboot, BLE controller) on its UART. The host
application sends one package to the H5:
* **image 0** is the H5's application, a `Client` image;
* **image 1** is the BL54L10's application, staged in an H5 partition, a
  `Device` image. After the reboot the H5 updates the BL54L10 over UART with
  Zephyr's SMP client (`CONFIG_SMP_CLIENT`, `CONFIG_MCUMGR_GRP_IMG_CLIENT`),
  which keeps the BL54L10's own test-and-revert.

The H5 is the only party that sees both MCUs at every boot, so it is the one
that can keep the pair consistent without a PC present. smply's part is to
deliver both images atomically enough: nothing is confirmed until both are in
place.

## The contract a device-committed image must honour

smply reads nothing but the standard image-state listing (group 1, command 0).
For an image `N` that smply treats as `Device`, the device must report the
following. **Slot 0 of image `N` reports what the other MCU actually runs**,
filled in through Zephyr's slot-state hook
(`CONFIG_MCUMGR_GRP_IMG_IMAGE_SLOT_STATE_HOOK`) from what the BL54L10 itself
reports.

| When | Image `N`, slot 1 (staging) | Image `N`, slot 0 (what the other MCU runs) |
| ---- | --------------------------- | ------------------------------------------ |
| After upload, before the mark | the new image's hash, not pending | the old image's hash |
| After the mark | the new hash, **`pending`** | unchanged |
| After the reset, while applying | the new hash, still `pending` | unchanged |
| **Applied, on trial** | anything not pending | **the new hash, not confirmed** |
| **Committed** | anything | **the new hash, `confirmed`** |
| **Apply failed** | the new hash, **no longer pending** | still the **old** hash |

In words:
1. **Accept the upload and the mark** for image `N` like any MCUboot image.
   `CONFIG_MCUMGR_GRP_IMG_UPDATABLE_IMAGE_NUMBER` covers it, and slot 1 is its
   staging area.
2. **After the reset, apply it**, keeping slot 1 `pending` while doing so. The
   other MCU boots it **on trial** (its own MCUboot's test swap). Report that as
   the new hash in slot 0, not confirmed.
3. **Commit it when smply confirms the `Client` images.** Enable
   `CONFIG_MCUMGR_GRP_IMG_STATUS_HOOKS` and handle
   `MGMT_EVT_OP_IMG_MGMT_DFU_CONFIRMED` for image 0 (protocol-notes S39). Then
   confirm the BL54L10 through the SMP client, and report slot 0 confirmed.
   **If image 0 is not on trial when the apply finishes** (the update did not
   change it, so no confirm will come), commit right after the apply. smply
   waits for the commit either way, bounded by `UpdatePlan::apply_timeout`.
4. **Report a failed apply** by clearing `pending` on slot 1 while slot 0
   still shows the old hash. smply then fails the update rather than waiting
   out the timeout.
5. **Finish or roll back at every boot, on its own.** smply cannot do this.
   The rules follow from the order above, and the H5 always still holds the
   new image in staging:
   * image 0 confirmed and the BL54L10 on trial (power failed between the two
     commits): **commit** the BL54L10;
   * image 0 confirmed and the BL54L10 back on the old image (its trial was
     reverted by a reset): **apply it again**;
   * image 0 reverted (smply never confirmed it) and the BL54L10 on trial:
     **reset the BL54L10**, and its MCUboot reverts it too. The pair is old and
     old again.
6. **Refuse to run with a mismatched partner**, e.g. do not start the BLE host
   against a controller of the wrong version. An MCUboot dependency TLV in
   image 0 (`IMAGE_TLV_DEPENDENCY`: image id and minimum version) can carry
   that rule, signed with the image. smply reads and reports it, and refuses a
   package whose own images do not satisfy it (ADR-0022), but it does not
   check the device.
7. **Leave image 0's confirm to smply.** A Zephyr application that confirms
   itself at boot breaks the order: image 0 is committed before the other
   MCU runs the new image, and smply, which then reads image 0 confirmed,
   cannot tell that from an image confirmed earlier.
8. **Keep the link up from smply's confirm until it has read the confirm
   back.** smply follows its confirm with an image-state read. Start the
   other MCU's commit from a work item, once `MGMT_EVT_OP_CMD_DONE` reports
   that read done (group 1, command 0; protocol-notes S46). smply treats a
   drop between the two as a failed update.
9. **After a failed apply, reset.** smply fails the update with
   `revert_pending` and does not reset the device itself. Until something
   does, image 0 stays on trial.

**Keep the H5's own bootloader away from image `N`**, since it is not the
H5's image and is signed with another key (protocol-notes S40). Either use
MCUboot's image-access hooks, as nRF Connect SDK does for the nRF5340 network
core, or build MCUboot for one image and let the H5 application own image
`N`'s slot and trailer.

**If the H5's MCUboot does swap image `N` into H5 flash**, slot 0 must still
report the other MCU's real state, not the H5's copy. Otherwise "applied, on
trial" and "still applying" look the same, and smply would confirm image 0
before the BL54L10 runs the new image.

### One way to build the H5 side

For the H5 and BL54L10, whose UART normally carries H4 HCI, with an update
GPIO beside the BL54L10's reset line. It is a sketch of product firmware,
which smply neither contains nor tests.
* **Apply through the BL54L10's own bootloader.** Stop the BLE host, raise
  the update GPIO and reset the BL54L10: its MCUboot enters serial recovery
  before choosing an image (protocol-notes S42). Upload to its secondary slot
  (`image` 2 in serial recovery's numbering, S43), mark it for test, lower the
  GPIO and reset: the new controller boots on trial. Restart the BLE host.
* **Commit through the running controller, not the bootloader.** Serial
  recovery's set-state only schedules the secondary slot (S44), and leaving
  recovery reverts an unconfirmed trial. A vendor-specific HCI command
  handled by the controller application, which then calls
  `boot_write_img_confirmed()`, confirms it at runtime. The link to smply
  stays up.
* **Never build the BL54L10's MCUboot with `BOOT_SERIAL_PIN_RESET`**: every
  reset the H5 uses to revert a trial would then land in recovery (S42).

### The link can drop while the other MCU is being updated

On the product, the BLE link runs through the BL54L10, so it is gone while
the controller is being updated. smply expects that. In `AwaitingDeviceApply`
and `AwaitingDeviceCommit`:
* a read that fails with `Disconnected` asks the application to reconnect
  (`ReconnectRequired`), as after the reset;
* a read that times out is simply retried;
* on the reconnect, smply reads the state again and carries on.

**While the link is down, the application's reconnect policy bounds the wait,
not `apply_timeout`.** Size it for the whole outage: the UART transfer of
the controller image, plus the controller's reboot, plus advertising. A
policy that gives up after a few seconds fails the update even though the
device is fine.

smply polls every `UpdatePlan::apply_poll_interval` and bounds each wait,
the apply and the commit, with `UpdatePlan::apply_timeout`. Size it for the
slowest apply, including a transfer through the other MCU's bootloader over
its UART.

## The package

`support/dfu_package/` (`smply::dfu_package`, not installed) reads the package
nRF Connect SDK's sysbuild writes, `dfu_application.zip`
([`protocol-notes.md`](protocol-notes.md), S38), and returns one
`PackageImage` per file, sorted by `image_index`, each a view into the archive
ready for a `MemoryImageSource`. It checks the manifest instead of believing it:
* `size` against the file;
* `version_MCUBOOT` against the image's own MCUboot header;
* each `image_index` given once. A direct-XIP build writes two files for one
  image, which are alternatives rather than a set, and is refused.

`image_index` is a string in these manifests (`"1"`), because
`generate_zip.py` keeps every sysbuild value a string unless it starts with
`0x`; a number is accepted too. A manifest with one file and no `image_index`
reads as image 0.

It also reports each image's dependency TLVs (`ImageDependency`: image and
minimum version), so an application can show the compatibility rule the
package carries. smply does not enforce them. The device's MCUboot does, at
the one reset.

Which images are `Device` images is the application's call, not the
package's: nothing in the manifest says who commits an image.

**A package whose own images disagree is refused.** If an image carries a
dependency on another image in the package, at a minimum version the package
does not carry, `read_package()` refuses it before anything is sent. The
comparison is MCUboot's default one, which ignores the build number
(protocol-notes S41).

## Trying it

```sh
cli_dfu --demo-package                  # a generated two-image package
cli_dfu --demo-package --apply-fails    # the device fails to apply image 1
cli_dfu --package dfu_application.zip   # a real package, against the stub
serial_dfu --port /dev/ttyACM0 --package dfu_application.zip
```

Without `--port` both examples run against `examples/stub_device/`, given a
second image it commits itself. `support/dfu_app/package_update.hpp`
(`PackageUpdate`) is the few lines between a package file and
`FirmwareUpdater::start()`: it reads the file, bounded before anything is
allocated, and builds the target list with the default above.

## What is not supported

* **Several devices in one update.** Each `FirmwareUpdater` updates one device.
  A product without a coordinating MCU needs a host-side coordinator. That is
  on the roadmap, and ADR-0021 records why it was not the choice here.
* **A deflated package.** `support/dfu_package/` reads the stored zip that nRF
  Connect SDK's sysbuild writes, and refuses deflate with a clear error.

## Nothing here has run on hardware

The contract, the state machine and the package reader are tested against
`ServerSimulator`'s device-committed image mode, the stub device, and the
fuzzer; a package written by nRF Connect SDK's own `generate_zip.py` has been
read and installed against the stub. No H5 implements the contract yet.
