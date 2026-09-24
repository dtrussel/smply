# ADR-0017 — MCUmgr serial framing: portable, in `transports/serial/`, with a receiver

**Status:** Accepted (2026-09-19)

## Context

`architecture.md` §11 has listed "no serial/UART transport" as a known
limitation since P0, and §12 put UART first on its value-ordered list of
unscheduled work. The gap matters more than the list suggests: a large share of
Zephyr devices have no radio at all, so for them smply has no transport a
person could use, only a contract they could implement.

**MCUmgr's serial transport is not a byte pipe the way GATT is.** The Bluetooth
transport specification says an SMP message spanning several GATT packets gets
*no additional framing* (protocol-notes §8), which is why
`transports/common/ble_framing.hpp` is a sender with no receiver: the bytes
arriving from a notification are already SMP bytes. The serial transport adds a
real encapsulation — two-byte markers, base64, a big-endian length prefix, a
CRC and a newline terminator — and none of it is SMP. A client that cannot
produce and consume that encapsulation cannot speak to the device at all.

Three things make it a library feature rather than each application's problem.
It is **protocol**, read out of Zephyr's `serial_util.c` rather than invented.
It is **portable**: no byte of it needs a tty, a `termios` or a `CreateFile`.
And it is **fully testable without hardware**, which matters here more than
usual — `quality-gates.md` §3 records that the one platform adapter in the tree
is outside both clang-tidy and cppcheck, so anything that ends up inside a port
gets MSVC `/W4` and a bench and nothing else.

There is also evidence that getting it right is not trivial. P17c could not
make `mcumgr-client` complete an upload over the bench peer's shell transport
in three configurations, and demoted it from a third client to an oracle
(protocol-notes §9). The peer's console carries an echoing shell and a
deferred log backend on the same stream, and a decoder with no opinion about
lines that are not frames cannot work there.

Four questions had to be answered, and each could have gone the other way.

## Decision

**1. The portable framing ships; the port does not.** `transports/serial/` is
header-only and platform-independent — `crc16.hpp`, `base64.hpp` and
`serial_framing.hpp` — and contains no file descriptor, no `termios`, no
`CreateFile`, no thread. Opening the port, reading it, and marshalling inbound
bytes onto the client context with `smply::Dispatcher` stay with the
application, exactly as [ADR-0005](ADR-0005-transport-abstraction.md) intends
and [ADR-0004](ADR-0004-threading-model.md) requires. ADR-0005's own
consequences already named "a future UART transport does its own base64/CRC
framing and still satisfies this contract unchanged" as the design's acceptance
test; this is that test, and the contract needed no change.

The division is not squeamishness about platform code — `transports/winrt_ble/`
exists. It is that the protocol half and the platform half have very different
evidence available to them. The protocol half gets unit tests on every preset,
clang-tidy, cppcheck, a coverage gate and a fuzz target. The platform half
would get a bench. Putting a CRC where the bench is the only witness would be a
choice to know less.

**2. There is a receiver here, and [ADR-0006](ADR-0006-reassembly-location.md)
is untouched.** ADR-0006 says the core reassembles and "transports do no
SMP-level work whatsoever". `SerialDeframer` does no SMP-level work: it never
reads the 8-byte header, never consults `length`, never counts payload bytes.
It undoes an encapsulation that exists *below* SMP and hands whole packets to
`TransportListener::on_bytes()`, where `MessageAssembler` parses them exactly
as it parses bytes off a GATT notification.

That distinction is the whole reason `ble_framing.hpp` can say "this file has a
sender and no receiver" and this one cannot. Both statements follow from the
same rule applied to two transports that differ in fact: one adds framing and
the other does not. A serial adapter that instead handed base64 text to
`MessageAssembler` would not be honouring ADR-0006, it would be breaking the
transport contract.

**3. No new installed target.** The headers ship under the existing
`smply::transport_common`, via one more `install(DIRECTORY)` line.
[ADR-0016](ADR-0016-installed-package-and-versioning.md) clause 1 names the
package's three targets and says "and nothing else"; clause 4 makes the stable
surface "`include/smply/` plus the installed transport headers". Adding a
directory keeps both sentences true. Adding a `smply::transport_serial` would
have contradicted clause 1 verbatim and required superseding an ADR accepted
the day before — to buy a consumer the ability to not link a header-only
target, which costs nothing to link.

Clause 3's spelling rule applies unchanged: `transports/` is the build-tree
include root and the installed root is `<prefix>/include/smply/transports`, so
an adapter writes `#include "serial/serial_framing.hpp"` in both. That spelling
is now a compatibility promise, which means **renaming this directory is a
breaking change**.

**4. A line with no recognised marker is ignored, and does not disturb a packet
in progress.** This is the one behavioural decision that is not forced by the
wire format, and it is taken from the server: `mcumgr_serial_process_frag()`'s
`default:` arm returns without freeing its receive context. A continuation
frame arriving with nothing in progress *is* fatal there, and is here, because
it means the two ends disagree about whether a packet is open.

The alternative — any non-frame line is a framing error — is simpler and would
make the module useless on the configuration it is most likely to meet. A
Zephyr console with `CONFIG_SHELL_BACKEND_SERIAL` and `CONFIG_LOG_BACKEND_UART`
emits prompts, echo and log lines between frames as a matter of course; that is
the exact stream P17c could not complete an upload over. Tolerating it is not
laxity about hostile input: an ignored line is *not decoded*, so nothing a
device can put in one reaches the CRC, the length or the buffer.

## Alternatives considered

* **Put the framing in `transports/common/`.** It is portable and header-only,
  which is what that directory is for, and it would have needed no CMake change
  at all (`transports/CMakeLists.txt` picks headers up from the include root).
  Rejected because `common/` means *common to adapters*, and every symbol in it
  today — `fragment_size()`, `SendQueue`, `LinkState` — is either
  transport-agnostic or BLE. Serial framing is neither: a BLE adapter must
  never use it. A sibling directory says that in the path.
* **A `smply::transport_serial` target, installed.** Rejected under decision 3.
* **Ship a POSIX adapter too**, so there is something end-to-end in the tree.
  Rejected for this phase: no CI job can open a tty against a device, so its
  only evidence would be a bench that does not yet have a serial peer
  configured, and it would be the second directory in the tree outside
  clang-tidy and cppcheck. Filed as follow-up work instead, with the framing
  it would use already tested.
* **Reuse a third-party base64.** Rejected under
  [ADR-0011](ADR-0011-build-and-dependencies.md): a new pinned dependency, an
  SBOM entry and a licence review, for forty lines whose correctness is settled
  by seven published test vectors.
* **A naive 93-bytes-per-frame split.** Legal, and a conforming receiver
  accepts it. Rejected because Zephyr's transmitter does something subtler —
  it defers a data byte rather than splitting the CRC across two frames — and
  matching it byte for byte means any device that tolerates Zephyr's own output
  tolerates smply's. The encoder is tested against a transcription of the C for
  exactly this.

## Consequences

* smply has a serial story: an application supplies ~50 lines of port code and
  gets the protocol. `docs/design.md` §12 shows the loop.
* `transports/serial/` is inside clang-tidy, cppcheck, the coverage gate and
  `tools/coverage.sh`'s filter, and has a libFuzzer target
  (`fuzz_serial_deframe`) over the decoder — the eighth, and the first over a
  transport.
* The installed surface grows by three headers under a target that already
  shipped. `tests/consumption/smoke.cpp` round-trips a packet through them, so
  a prefix that installed `common/` and forgot `serial/` fails all three
  consumption modes.
* **No adapter exists yet**, so nothing in CI or on a bench has put a serial
  byte on a wire. What is proved is agreement with a transcription of Zephyr's
  source, which is a strictly weaker claim than P17's, and `architecture.md`
  §11 says so rather than implying a working transport.
* A device reset over a serial link is an open question: the port may stay
  open, or vanish and return. `FirmwareUpdater`'s disconnect and reconnect
  states assume a link that drops. Recorded as **O7** rather than guessed at,
  because settling it needs a port.
