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
  applied: running on its target, on trial. smply never confirms it. The
  device commits it when smply confirms the `Client` images, and smply waits
  for that too, in `AwaitingDeviceCommit`.

**`Client` images are confirmed only after every `Device` image is running
on trial, and `Device` images are committed only after that confirm.** So
nothing is committed until the whole set has booted, and the set is never
committed half-validated. If a `Device` image fails to apply, the update
fails, the `Client` images stay unconfirmed, and the device reverts them on
its next reset.

## Why "Device" exists: a coordinator in front of other processors

Some products have one MCU that smply talks to and one or more processors
behind it that it updates. This document uses two words for them:
* **the coordinator**: the MCU smply talks to. It runs the MCUmgr server,
  usually owns image 0, and sees every target at every boot;
* **a target**: a processor whose firmware arrives as a `Device` image, one
  image number per target. It sits behind the coordinator on any link the
  coordinator can drive: UART, SPI, I2C, CAN, a shared memory, an HCI
  transport.

For example:
* an application MCU with a Bluetooth controller module;
* a main MCU with a motor-control or sensor-hub MCU;
* a gateway with a cellular or Wi-Fi co-processor;
* several of these at once, as images 1, 2 and so on.

The host sends one package to the coordinator:
* **image 0** is the coordinator's own application, a `Client` image;
* **images 1 … N** are the targets' firmware, staged in coordinator
  partitions, each a `Device` image. After the reset, the coordinator applies
  each to its target in whatever way the target supports.

The coordinator is the only party that sees every MCU at every boot, so it
is the one that can keep the set consistent without a PC present. smply's
part is to deliver every image with one reset, confirm nothing until each
target runs its new image on trial, and report success only once each target
is committed.

**smply knows nothing about the targets.** It never talks to one, and it
does not care how the coordinator reaches one or what bootloader it runs.
It reads the coordinator's standard image-state listing. The contract below
is everything smply needs, and it is the same for every kind of target.

**What a target needs.**
* **A way to run a new image on trial and fall back if it is not
  committed.** MCUboot's test swap is one. Any bootloader with an equivalent
  test / confirm / revert works. A target without one cannot honour "on
  trial", and the coordinator would have to keep the old image and restore
  it itself.
* **Its firmware packaged as an MCUboot image**: an MCUboot header and a
  hash TLV. smply identifies every image by that hash, and the package reader
  checks each image's header. Signing it is the target's business, with the
  target's own key; the coordinator's bootloader never has to validate it
  (see below). A target that does not run MCUboot still gets its firmware in
  that wrapper, and the coordinator unwraps it before applying it.

## The contract a device-committed image must honour

smply reads nothing but the standard image-state listing (group 1, command 0).
For an image `N` that smply treats as `Device`, the coordinator must report
the following. **Slot 0 of image `N` reports what the target actually
runs**, filled in through Zephyr's slot-state hook
(`CONFIG_MCUMGR_GRP_IMG_IMAGE_SLOT_STATE_HOOK`) from what the target itself
reports.

| When | Image `N`, slot 1 (staging) | Image `N`, slot 0 (what the target runs) |
| ---- | --------------------------- | ---------------------------------------- |
| After upload, before the mark | the new image's hash, not pending | the old image's hash |
| After the mark | the new hash, **`pending`** | unchanged |
| After the reset, while applying | the new hash, still `pending` | unchanged |
| **Applied, on trial** | anything not pending | **the new hash, not confirmed** |
| **Committed** | anything | **the new hash, `confirmed`** |
| **Apply failed** | the new hash, **no longer pending** | still the **old** hash |

In words, for each `Device` image:
1. **Accept the upload and the mark** for image `N` like any MCUboot image.
   `CONFIG_MCUMGR_GRP_IMG_UPDATABLE_IMAGE_NUMBER` covers it, and slot 1 is its
   staging area.
2. **After the reset, apply it**, keeping slot 1 `pending` while doing so. The
   target boots it **on trial**. Report that as the new hash in slot 0, not
   confirmed.
3. **Commit it when smply confirms the `Client` images.** Enable
   `CONFIG_MCUMGR_GRP_IMG_STATUS_HOOKS` and handle
   `MGMT_EVT_OP_IMG_MGMT_DFU_CONFIRMED` for image 0 (protocol-notes S39). Then
   confirm the target, through whatever it provides (an SMP client, a vendor
   command, a register write), and report slot 0 confirmed.
   **If image 0 is not on trial when the apply finishes** (the update did not
   change it, so no confirm will come), commit right after the apply. smply
   waits for the commit either way, bounded by `UpdatePlan::apply_timeout`.
4. **Report a failed apply** by clearing `pending` on slot 1 while slot 0
   still shows the old hash. smply then fails the update rather than waiting
   out the timeout.
5. **Finish or roll back at every boot, on its own.** smply cannot do this.
   The rules follow from the order above, and the coordinator always still
   holds the new image in staging:
   * image 0 confirmed and the target on trial (power failed between the two
     commits): **commit** the target;
   * image 0 confirmed and the target back on its old image (its trial was
     reverted by a reset): **apply it again**;
   * image 0 reverted (smply never confirmed it) and the target on trial:
     **reset the target**, so its bootloader reverts it too. Every image is old
     again.
6. **Refuse to run with a mismatched partner.** Do not drive a target that
   runs a version the coordinator cannot work with. An MCUboot dependency TLV
   in image 0 (`IMAGE_TLV_DEPENDENCY`: image id and minimum version) can carry
   that rule, signed with the image. smply reads and reports it, and refuses a
   package whose own images do not satisfy it (ADR-0022), but it does not
   check the device.
7. **Leave image 0's confirm to smply.** A Zephyr application that confirms
   itself at boot breaks the order: image 0 is committed before the targets
   run their new images, and smply, which then reads image 0 confirmed,
   cannot tell that from an image confirmed earlier.
8. **Keep the link up from smply's confirm until it has read the confirm
   back.** smply follows its confirm with an image-state read. Start the
   targets' commits from a work item, once `MGMT_EVT_OP_CMD_DONE` reports
   that read done (group 1, command 0; protocol-notes S46). smply recovers
   from a drop between the two (ADR-0023), but it costs a reconnect.
9. **After a failed apply, reset.** smply fails the update with
   `revert_pending` and does not reset the device itself. Until something
   does, image 0 stays on trial.

**With several targets**, each image follows the table on its own. smply
confirms image 0 only once every target runs on trial, and completes only
once every target is committed.

**Keep the coordinator's own bootloader away from image `N`**, since it is
not the coordinator's image and is signed with another key (protocol-notes
S40). Either use MCUboot's image-access hooks, as nRF Connect SDK does for
the nRF5340 network core, or build MCUboot for one image and let the
coordinator's application own image `N`'s slot and trailer.

**If the coordinator's MCUboot does swap image `N` into its own flash**,
slot 0 must still report the target's real state, not the coordinator's
copy. Otherwise "applied, on trial" and "still applying" look the same, and
smply would confirm image 0 before the target runs the new image.

### The link can drop while a target is being updated

If smply's link to the coordinator runs through a target, it is gone while
that target is updated. A Bluetooth controller module is the usual case,
and a cellular or Wi-Fi modem is another. smply expects that. In
`AwaitingDeviceApply` and `AwaitingDeviceCommit`:
* a read that fails with `Disconnected` asks the application to reconnect
  (`ReconnectRequired`), as after the reset;
* a read that times out is simply retried;
* on the reconnect, smply reads the state again and carries on.

**While the link is down, the application's reconnect policy bounds the wait,
not `apply_timeout`.** Size it for the whole outage: the transfer of the
target's image, plus the target's reboot, plus the link coming back (for
Bluetooth, advertising). A policy that gives up after a few seconds fails the
update even though the device is fine. If the link does not run through a
target, none of this arises.

smply polls every `UpdatePlan::apply_poll_interval` and bounds each wait,
the apply and the commit, with `UpdatePlan::apply_timeout`. Size it for the
slowest apply, including a transfer over the coordinator's link to the
target, which may be slow (a UART, or a bootloader's recovery protocol).

### Worked example: an application MCU with a Bluetooth controller module

The product that motivated this design, kept here as one concrete case. It
is a sketch of product firmware, which smply neither contains nor tests.
* **The parts.** The coordinator is an STM32H5 (Zephyr, MCUboot, the BLE
  host). The target is an Ezurio BL54L10 (Zephyr, its own MCUboot, the BLE
  controller) on the H5's UART. That UART normally carries H4 HCI, and there
  is an update GPIO beside the BL54L10's reset line.
* **The images.** Image 0 is the H5's application (`Client`). Image 1 is the
  BL54L10's application (`Device`), staged in an H5 partition. The BLE link
  to smply runs through the BL54L10, so it drops while the controller is
  updated (the section above).
* **Apply through the target's own bootloader.** Stop the BLE host, raise
  the update GPIO and reset the BL54L10: its MCUboot enters serial recovery
  before choosing an image (protocol-notes S42). Upload to its secondary slot
  (`image` 2 in serial recovery's numbering, S43) with Zephyr's SMP client,
  mark it for test, lower the GPIO and reset: the new controller boots on
  trial. Restart the BLE host.
* **Commit through the running controller, not the bootloader.** Serial
  recovery's set-state only schedules the secondary slot (S44), and leaving
  recovery reverts an unconfirmed trial. A vendor-specific HCI command
  handled by the controller application, which then calls
  `boot_write_img_confirmed()`, confirms it at runtime. The link to smply
  stays up.
* **Never build the BL54L10's MCUboot with `BOOT_SERIAL_PIN_RESET`**: every
  reset the H5 uses to revert a trial would then land in recovery (S42).

Another kind of target changes only the last three points: how the
coordinator reaches it, applies the image and commits it. The contract and
smply's behaviour stay as they are.

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
allocated, and builds the target list: image 0 `Client`, every other image `Device`,
and `set_commit()` changes that per image.

## What is not supported

* **Several devices in one update.** Each `FirmwareUpdater` updates one device.
  A product without a coordinator MCU needs a host-side coordinator. That is
  on the roadmap, and ADR-0021 records why it was not the choice here.
* **A deflated package.** `support/dfu_package/` reads the stored zip that nRF
  Connect SDK's sysbuild writes, and refuses deflate with a clear error.

## Nothing here has run on hardware

The contract, the state machine and the package reader are tested against
`ServerSimulator`'s device-committed image mode (one target, and two behind
one coordinator), the stub device, and the fuzzer; a package written by nRF Connect SDK's own `generate_zip.py` has been
read and installed against the stub. No coordinator firmware implements the
contract yet.
