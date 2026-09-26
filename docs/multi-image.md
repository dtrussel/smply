# Multi-image updates and the device contract

How smply updates several images of one device in one update, and what the
device must do for an image that **it** commits rather than smply.
[ADR-0021](decisions/ADR-0021-multi-image-update.md) is the decision.
[`protocol-notes.md`](protocol-notes.md) §6 ("Several images on one device")
holds the protocol facts this relies on.

## The shape

One device, several MCUboot images, **one reset**:

```
stage every image  →  reset once  →  verify  →  wait for device-applied images
      (upload, verify present,         (Client images      (Device images:
       mark for test, per image)        booted the target)  poll image state)
                                   →  confirmation window  →  confirm Client images
                                      (the application's call, ADR-0014)
```

Each image is either:
* **`Client`**: smply commits it. It is marked for test, the reset swaps it in,
  smply verifies it booted, and it is confirmed by hash after the window. This
  is the ordinary single-image update, image 0 of a typical device.
* **`Device`**: the device commits it. smply stages and marks it, then after the
  reset it waits, in `AwaitingDeviceApply`, for the device to report the image
  applied. It never confirms it.

**`Client` images are confirmed only after every `Device` image is reported
applied.** If one is not, the update fails, the `Client` images stay
unconfirmed, and the device reverts them on its next reset.

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
following. Zephyr's slot-state hook (`CONFIG_MCUMGR_GRP_IMG_IMAGE_SLOT_STATE_HOOK`)
is where an H5 fills in what the BL54L10 actually runs.

| When | Image `N`, slot 1 (staging) | Image `N`, slot 0 (what is applied) |
| ---- | --------------------------- | ------------------------------------ |
| After upload, before the mark | the new image's hash, not pending | the currently applied image's hash |
| After the mark | the new hash, **`pending`** | unchanged |
| After the reset, while applying | the new hash, still `pending` | unchanged |
| **Applied** | anything not pending | **the new hash, `confirmed`** |
| **Apply failed** | the new hash, **no longer pending** | still the **old** hash |

In words:
1. **Accept the upload and the mark** for image `N` like any MCUboot image:
   `CONFIG_MCUMGR_GRP_IMG_UPDATABLE_IMAGE_NUMBER` covers it, and slot 1 is its
   staging area.
2. **After the reset, apply it**, and keep slot 1 `pending` while doing so.
3. **Report success** by showing the new hash in slot 0, `confirmed`, once the
   target MCU has committed it.
4. **Report failure** by clearing `pending` on slot 1 while slot 0 still shows
   the old hash. smply then fails the update rather than waiting out the
   timeout.
5. **Finish or roll back an interrupted apply at every boot, on its own.**
   smply cannot do this. If power fails after smply confirmed image 0 and
   before the BL54L10 committed, only the H5 is there when it comes back.
6. **Refuse to run with a mismatched partner**, e.g. do not start the BLE host
   against a controller of the wrong version. An MCUboot dependency TLV in
   image 0 (`IMAGE_TLV_DEPENDENCY`: image id and minimum version) can carry
   that rule, signed with the image. smply reads and reports it but does not
   enforce it.

**The same rules hold if the H5's MCUboot swaps image `N` itself**, which is
how nRF Connect SDK handles the nRF5340 network core. There, MCUboot swaps the
staged image into slot 0 at the reset, as an unconfirmed test. The H5
application then forwards slot 0 to the BL54L10, and confirms image `N` through
MCUboot's own API once the BL54L10 has committed. smply sees:
* slot 0 with the new hash and **not confirmed**: still applying, so it keeps
  waiting;
* the new hash, **confirmed**: applied;
* slot 0 back on the old hash after a revert, with slot 1 no longer pending:
  failed.

Nothing in smply depends on which variant the device uses.

smply bounds the wait with `UpdatePlan::apply_timeout` and polls every
`UpdatePlan::apply_poll_interval`. Size the timeout for the slowest apply:
a UART upload of the whole image to the second MCU, plus its reboot.

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
