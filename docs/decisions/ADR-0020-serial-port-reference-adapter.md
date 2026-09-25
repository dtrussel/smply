# ADR-0020 — A reference serial port adapter, in `transports/serial_port/`, not installed

**Status:** Accepted (2026-09-25). Qualifies decision 1 of
[ADR-0017](ADR-0017-serial-framing-placement.md). The framing's placement and
packaging are unchanged; what changes is that the tree now also carries a port.

## Context

ADR-0017 shipped MCUmgr's console framing as portable, header-only code and
left the port to the application: `termios` or `CreateFile`, a reader thread,
and a `Dispatcher` to marshal what it reads. It rejected shipping a POSIX
adapter "for this phase", for two reasons. No CI job could open a tty against
a device. And an adapter would be the second directory in the tree outside
clang-tidy and cppcheck. It filed the adapter as follow-up work.

Both reasons have weakened, and a third fact has become more pressing.

* **A pseudo-terminal is a real tty.** `posix_openpt()` gives a byte stream
  with `termios`, non-blocking reads, `poll()`, and a hang-up when the other
  end closes. A stub device behind the framing on one end, and the adapter
  plus `FirmwareUpdater` on the other, runs a whole update in a container.
  That includes the reset, in both shapes O7 describes. It is not a device,
  but it is far more than "agreement with a transcription of the C", which is
  all ADR-0017 could claim.
* **The POSIX half is analysable.** It compiles on the Linux runner, so it is
  in the compile database clang-tidy and cppcheck already use. Only the Win32
  half would be unanalysed, as `transports/winrt_ble/` is.
* **Without a port, smply has no usable transport for most Zephyr devices**, the
  ones with no radio. "Implement `Transport` yourself; the framing is here" is
  true, but the part that goes wrong is exactly the part left to the
  integrator. That part is the reader thread, the shutdown ordering, the
  borrowed-buffer copy across threads, and what a vanished USB port looks like.
  The WinRT adapter took a bench and three defects to get the same things
  right.

## Decision

**1. A reference adapter lives in `transports/serial_port/`, as target
`smply::serial_port`, and is not installed.** It has the same status as
`smply::winrt_ble`: source to read and copy, built and tested in-tree, and
outside the stable surface of ADR-0016 clause 4. ADR-0016 clause 1's list of
four installed targets is unchanged, so no ADR is superseded. The reason for
leaving it out is also winrt_ble's: part of the target (the Win32 half) is
seen only by MSVC `/W4`, so a compatibility promise about its surface would
rest on that alone. Everything a third-party serial adapter needs from the
package is already installed: the core, `Dispatcher`, the framing, `SendQueue`
and `LinkState`.

ADR-0017's decision 1 said the port "stays with the application". It still
may: nothing in the core or the package requires the adapter, and
`transports/serial/` is unchanged. What is withdrawn is the claim that the
tree does not carry one. ADR-0017's status line records that.

**2. One header, two implementations, and no OS type in the header.**
`serial_port_transport.hpp` declares `SerialPortTransport`, `SerialPortConfig`
and `SerialLinkCounters`, and hides everything platform-specific behind an
opaque `State`, as `winrt_ble_transport.hpp` does. `posix/` implements it with
`termios` and `poll()` (Linux and macOS). `win32/` implements it with
`CreateFile` and overlapped I/O. The two files mirror each other section by
section, so the one that nothing can run here is reviewable against the one
that CI exercises.

**3. One I/O thread per open port, in the adapter.** It reads, splits lines,
deframes, and writes. Inbound packets reach the client context only through
the application's `Dispatcher`, copied, by closures that hold the adapter's
state and ask `LinkState::may_deliver()` first. Send admission is
`SendQueue`. `close()` stops the thread and joins it, and never drains or
clears the `Dispatcher`. This is ADR-0004 unchanged: the thread is the
adapter's, and the core still starts none. One thread rather than a reader
plus a writer gives one join and one shutdown order to get right.

**4. Lint covers the POSIX half; only `transports/serial_port/win32/` is
excluded,** as an exact directory prefix beside the three existing ones. It is
excluded for the same reason they are: those translation units do not exist
in a Linux compile database. `tools/verify_gates.sh` plants a portable decoy
next to it to prove the exclusion is no wider than the directory.

**5. A reset over serial needs no change in the core (O7).** The adapter
reports what the medium tells it and assumes nothing more.
* A USB CDC ACM port disappears when the device resets. The read fails, the
  adapter reports `on_disconnected`, and `FirmwareUpdater` leaves
  `AwaitingDisconnect` at once.
* A hardware UART does not drop. The updater leaves `AwaitingDisconnect` when
  `UpdatePlan::disconnect_grace` expires, which is already the documented
  path (design.md §8, "grace timeout with the link still up"). A serial
  application should shorten that grace to a few seconds.
* Either way, on `ReconnectRequired` the application opens a **new**
  transport **by path**, retrying until the path exists, and rebinds. That
  covers a port that returns under the same name, one that stays open, and
  one that returns renamed when the application names it by a stable path
  such as `/dev/serial/by-id/…`.

This is designed for, tested against a pseudo-terminal in both shapes, and
**not measured on hardware**. O7 stays open until a bench run measures it.

**6. `max_message_size()` reports the configured cap, which is at most
`kMaxSerialPacket` and defaults to 256.** It is never `kMaxRawPerFrame`, since
a message spans as many frames as it needs. The default is the Zephyr default
of `CONFIG_MCUMGR_TRANSPORT_UART_MTU` and `CONFIG_MCUMGR_TRANSPORT_SHELL_MTU`.
It is also below `CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE − 4` for the default
netbuf. That matters because a serial packet must fit the device's netbuf
together with its length prefix and CRC (protocol-notes §9, A25).

## Alternatives considered

* **Install it.** Rejected under decision 1. The Win32 half would carry a
  SemVer promise backed only by MSVC `/W4`, and an installed consumption check
  for it could run on Windows only. It is the same reasoning ADR-0016 gives
  for `winrt_ble`, and it is to be revisited on the same trigger: a consumer
  who asks.
* **Put the adapter in the example.** No target, so an integrator copies
  files. Rejected because the tests would then compile example sources, and
  the adapter would be outside the target structure that lets lint, the
  sanitizers and the pty tests treat it as library code.
* **A reader thread and a writer thread.** Simpler loops, two joins, and a
  shutdown order between them. Rejected for the single `poll()` /
  `WaitForMultipleObjects` loop, whose wake-up handle carries both "a message
  is waiting" and "stop".
* **Let the adapter declare the link dropped on a UART reset** (a
  `drop_link()` the application calls on `DisconnectExpected`). It is faster
  than waiting out the grace period, but it is new API that fabricates a link
  loss the medium never reported. Rejected in favour of a shorter
  `disconnect_grace`, which the plan already carries.

## Consequences

* smply can update a device over a serial port with code from this repository
  alone. `examples/serial_dfu/` shows it, and runs as a ctest against a stub
  device behind a pseudo-terminal, in both reset shapes.
* The tree has four directories outside clang-tidy and cppcheck, not three.
  All are platform code, and each is proven to be excluded exactly.
* The Win32 half is compile-only in CI (`windows-msvc`), with a link-and-call
  smoke test that opens a port that does not exist. Like the WinRT adapter, it
  is unproven until a bench runs it.
* `transports/serial/`'s evidence gets stronger (a real tty stream in CI), and
  ADR-0017's remark that "no adapter exists yet" is no longer true. Its status
  note says so.
