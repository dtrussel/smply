# serial_dfu

`cli_dfu`'s pump loop over a real serial port, with
[`smply::serial_port`](../../transports/serial_port/serial_port_transport.hpp).
`examples/cli_dfu/main.cpp` is still the file to read for the loop itself.
This one adds what a serial link needs to survive a device reset
(roadmap O7, [`design.md`](../../docs/design.md) §13).

## Against a device

```sh
serial_dfu --port /dev/serial/by-id/usb-ZEPHYR_...-if00 --image zephyr.signed.bin
serial_dfu --port /dev/ttyUSB0 --baud 115200 --image zephyr.signed.bin
serial_dfu --port COM4 --image zephyr.signed.bin          # Windows
serial_dfu --port /dev/ttyACM0 --package dfu_application.zip
```

* **Name a USB port by a stable path.** On Linux that is `/dev/serial/by-id/`.
  A USB CDC ACM port disappears when the device resets, and may come back as a
  different `ttyACMn`. The example reopens the path you gave it, so a stable
  path is what makes the reconnect find the device again.
* **The device must run the MCUmgr UART or shell transport**
  (`CONFIG_MCUMGR_TRANSPORT_UART` or `CONFIG_MCUMGR_TRANSPORT_SHELL`), on the
  port you name.
* **Only one process may hold the port.** Close any terminal or logger on it
  first. The adapter opens it exclusively.
* **The message cap is 256 bytes.** That is safe for a device built with the
  default netbuf. See protocol-notes.md A25 before raising it.
* **`--package` sends a multi-image DFU package**, the `dfu_application.zip`
  nRF Connect SDK's sysbuild writes, to one device in one update with one reset
  ([`docs/multi-image.md`](../../docs/multi-image.md)). Image 0 is confirmed
  here; every other image is left to the device to apply and commit, and is
  waited for first. `--commit N=client|device` overrides that per image.

## What it does about a reset

* `UpdatePlan::disconnect_grace` is **2 s**. A hardware UART does not drop when
  the device resets, so the updater moves on when the grace expires. A USB CDC
  port drops, and the adapter reports it at once.
* On `ReconnectRequired` it **closes the old port and opens a new one by path**,
  retrying while the port is absent (`Disconnected`). It gives up at once on
  any other refusal.

The summary line says which shape the reset took (`reset=grace` or
`reset=dropped`), how many distinct ttys the path resolved to
(`devices=`), what the link threw away (`ignored=`, `dropped_lines=`,
`framing_errors=`, `crc_failures=`), and how many images the update had and
the device applied (`images=`, `applied=`).

## In CI: the pseudo-terminal stub

Without `--port` (POSIX only), it starts the stub device from
`examples/stub_device/` behind a pseudo-terminal (`pty_stub.*`) and updates
that. Three ctests run it: one per reset shape, and a two-image package:

| Test | `--stub` | The port across the reset | Must print |
| ---- | -------- | ------------------------- | ---------- |
| `serial_dfu_pty_uart` | `uart` | stays open; boot banners and an over-long log line follow | `reset=grace devices=1`, `ignored` and `dropped_lines` non-zero, no framing errors |
| `serial_dfu_pty_cdc` | `cdc` | vanishes, and returns as a **new** `/dev/pts/N` behind the same symlink | `reset=dropped devices=2`, no framing errors |
| `serial_dfu_pty_package` | `uart`, with `--demo-package` | as `serial_dfu_pty_uart` | `reset=grace`, no framing errors, `images=2 applied=1` |

`--demo-package` gives the stub a second image that it applies itself after
the reset; `--apply-fails` makes that apply fail, and the update then fails
without confirming image 0.

The stub models the device's receive limit too. It refuses a packet the
device's netbuf could not hold together with the serial length prefix and
CRC (protocol-notes A25).

**What this does not show:** a device. Nothing here has run against real
hardware. Roadmap O7 stays open until a bench run measures a real reset.
