# Testing strategy

Goal: **every protocol path is reachable without hardware, without Bluetooth and
without real time.** The core is sans-IO precisely so this is possible.

Framework: **Catch2 v3** ([ADR-0012](decisions/ADR-0012-test-and-fuzz-tooling.md)).

## 1. Test levels

| Level | Location | Runs in | Gate |
| ----- | -------- | ------- | ---- |
| Unit | `tests/unit/` | < 5 s total | every PR, all 3 toolchains |
| Component (full stack over a simulated device) | `tests/component/` | < 20 s | every PR |
| Fuzz (smoke: committed corpus, 20 000 runs per target) | `tests/fuzz/` | ~70 s | every push and PR (Linux/Clang) |
| Fuzz (soak) | same targets | 30 min | nightly |
| The example, end to end | `examples/cli_dfu/` | < 2 s | every push, as **five** tests: `cli_dfu_demo`, `cli_dfu_flaky_reconnect`, `cli_dfu_reconnect_gives_up`, `cli_dfu_package` and `cli_dfu_package_apply_fails` |
| The serial example, end to end | `examples/serial_dfu/` | ~2.5 s | every push, on every Linux preset, as **three** tests: `serial_dfu_pty_uart`, `serial_dfu_pty_cdc` and `serial_dfu_pty_package`. A whole update, reset included, over a pseudo-terminal |
| The serial port adapter over a real tty | `tests/serial_port/` | < 2 s | every push, on every Linux preset, and on `windows-msvc` with only its port-free cases. A pseudo-terminal stands in for the port, so a real I/O thread, a real hang-up and real, bounded waits are involved. That is why it is its own executable and not part of the unit or component suites, which never read the real clock (§2) |
| The Windows targets | `transports/winrt_ble/`, `examples/winrt_ble_dfu/` | — | **no CI job runs them.** `windows-winrt` compiles both and runs `winrt_ble_smoke`, which links the adapter and checks it refuses a bad configuration; the runner has no radio, so nothing crosses GATT there. Their behavioural coverage is the HIL row below, on a bench |
| HIL / interoperability | `tests/hil/` | minutes | manual, from the bench. The nightly self-hosted job is committed and advisory, and **no runner is registered** — see §6 |

The three `cli_dfu` entries are what keep the **application's** half of the
protocol honest. The demo is the happy path; `--flaky-reconnect 3` refuses three
reconnection attempts so the backoff in `smply::dfu_app::ReconnectPolicy` runs
for real; and `cli_dfu_reconnect_gives_up` exhausts the policy, which is the
only exercise anywhere of `FirmwareUpdater::reconnect_failed()` outside the
component suite. That last one passes only on the exact failure message
(`PASS_REGULAR_EXPRESSION`), not on any non-zero exit: it has to fail *through*
`reconnect_failed()` rather than by timing out, or it is green and meaningless.

## 2. Test doubles (`tests/support/`)

### `ManualClock`
`Clock` implementation with `advance(Duration)`. **No unit or component test
may call `std::chrono::steady_clock::now()`.** Nothing enforces this yet; review
does, and the roadmap's backlog has the gate.

### `FakeTransport`
The workhorse (`tests/support/`). A `Transport` that records outbound messages
and lets the test inject inbound bytes with complete control:

```cpp
class FakeTransport final : public Transport {
public:
    // Transport
    Result<void> send(ConstBytes) override;
    std::size_t  max_message_size() const noexcept override;   // configurable
    void         set_listener(TransportListener*) noexcept override;
    void         close() noexcept override;

    // Inspection
    const std::vector<std::vector<std::byte>>& sent() const noexcept;
    std::size_t send_count() const noexcept;
    ConstBytes  last_sent() const;
    void        clear_sent() noexcept;
    bool        closed() const noexcept;
    bool        connected() const noexcept;
    std::size_t suppressed_deliveries() const noexcept;   // contract violations
    std::size_t on_bytes_calls() const noexcept;

    // Configuration
    void set_max_message_size(std::size_t);

    // Injection
    void deliver(ConstBytes);                                // one on_bytes()
    void deliver_fragmented(ConstBytes, std::size_t frag);   // fixed fragments
    void deliver_byte_by_byte(ConstBytes);
    void deliver_concatenated(std::span<const ConstBytes>);  // N msgs, 1 call
    void deliver_split_at(ConstBytes, std::span<const std::size_t> cuts);

    // Faults
    void fail_next_send(Error);                    // one-shot
    void set_busy(bool);                           // => TransportBusy
    void raise_transport_error(Error);             // link stays up
    void disconnect(Error = Error{ErrorCode::Disconnected});
};
```

**It enforces the contract rather than merely implementing it.** Delivering
after `disconnect()` or `close()` is suppressed and counted, so a test that
accidentally depends on a callback the contract forbids fails on
`suppressed_deliveries()` instead of passing for the wrong reason. A double that
is more permissive than the real thing is worse than no double.

Two scenarios need no API of their own: **"no response"** is simply not calling
`deliver()`, which together with `ManualClock` is how every timeout path is
driven; and **"delayed response"** is calling it later. **Malformed headers and
malformed CBOR** are ordinary byte buffers — the transport has no opinion about
content.

### Image doubles (`tests/support/image_builder.hpp`, `fake_image_source.hpp`)

`ImageBuilder` assembles an MCUboot image byte by byte — header fields, a body,
and the two TLV areas — independently of the parser, so a decoder bug cannot be
cancelled out by a matching builder bug. Every malformation knob overrides
**exactly one** field; see §3 "Image file handling" for why that matters.

`FailingImageSource` and `ShortReadingImageSource` break the `ImageSource`
contract in the two ways it forbids, because `MemoryImageSource` cannot: an
error path no test can reach is indistinguishable from one that does not work.

### `ServerSimulator` (`tests/support/server_simulator.*`)
A deterministic in-memory MCUmgr device: groups 0 and 1 per
[`protocol-notes.md`](protocol-notes.md) §§5-7, including the awkward parts --
offset correction in both directions, session resume by `sha`, the `off == 0`
restart, `match` on the final chunk, reset-then-drop, and the MCUboot swap and
revert bookkeeping across a simulated reboot.

**It does not answer from inside `send()`.** `Transport::send()` may not deliver
inbound bytes before it returns, so a simulator that replied inline would
re-enter reassembly, which the assembler refuses (`design.md` §2). It watches
`FakeTransport::sent()` and answers from `pump()`, which is the device's turn:

```cpp
while (!done) {
    sim.pump(clock.now());     // the device consumes and answers
    client.poll(clock.now());  // the client sees the answer
    clock.advance(1ms);
}
```

`tests/component/harness.hpp` wraps that loop as `run_until()`, with an
iteration budget so a stalled state machine fails in bounded time rather than
hanging CI. A test driving a simulator must not call `clear_sent()`.

`ServerConfig` describes a *device* -- what the firmware was built with:

```cpp
struct ServerConfig {
    std::uint32_t buf_size = 256;            // MCUmgr parameters
    bool supports_mcumgr_params = true;      // else ENOTSUP
    bool supports_slot_info     = false;     // CONFIG_MCUMGR_GRP_IMG_SLOT_INFO
    bool image_check_enabled    = true;      // emits "match"; enables rule 9a
    bool single_image           = true;      // omits "image" in state (one image only)
    std::uint32_t image_count   = 1;         // CONFIG_MCUMGR_GRP_IMG_UPDATABLE_IMAGE_NUMBER
    bool allow_confirm_non_active_image_secondary = false;  // the three A27
    bool allow_confirm_non_active_image_any       = false;  // Kconfigs, off
    bool allow_confirm_non_active_slot            = false;  // as in Zephyr
    bool translate_v1_errors    = true;      // the A16 rebuild for a v1 request
    std::uint32_t slot_size     = 0;         // 0 = unbounded
    Duration response_delay{0};
};
```

The SMP **version is deliberately not a device setting**: the version on the
wire is the client's to choose (`SmpClientConfig::smp_version`), and a real
server answers in the version it was asked in. What a device does choose is
whether it *translates* an image-group code for a v1 request (A16), which is
what `translate_v1_errors` models.

Scripted misbehaviour lives in methods rather than in the config, so that a
config stays a description and does not become a script:
`answer_offset_once()`, `fail_next()`, `drop_next_response()`,
`reset_busy_once()`, `device_commits()`, plus `load_slot()`, `reboot()` and
`rebind_transport()` for the device's own state. `answer_offset_once()` changes
only the *answer*, never the flash: a client that follows the correction is put
right on the next round trip, and one that computes its own offsets flashes a
corrupt image -- which is exactly the asymmetry the acceptance test relies on.

`ServerSimulator` is how the update state machine gets end-to-end coverage
without hardware. It is **not** a reference implementation and is never linked
into the library.

**It models `image_count` image pairs**, numbered globally as Zephyr does:
image `n` owns slots `2n` and `2n + 1`. The multi-image rules are
[`protocol-notes.md`](protocol-notes.md) §6 ("Several images on one device"),
each with a `[multi]` test in `test_simulator.cpp`:
* each image's primary is listed `active`;
* set-state finds a hash in any image;
* a hashless confirm names the running image (0) only;
* confirming a non-running image is denied unless the A27 knobs allow it;
* the upload's first-packet `image` picks that image's secondary, and an
  image the device lacks gets `NoFreeSlot`;
* every image swaps at the same reboot.

`device_commits(image, outcome, reads)` makes one image **device-committed**,
the staged image of a second MCU in `docs/multi-image.md`. A reboot swaps it in
as an unconfirmed trial. After `reads` more state reads, the device finishes:
confirmed, or the old image swapped back with nothing pending.

## 3. Required unit coverage

### `Dispatcher` (`src/util/`)
The only concurrent component in the tree, and the only place where a test can
be wrong without being red — a race that does not happen today happens in
somebody's adapter next year. So the properties are asserted structurally
wherever possible rather than by hoping a schedule interleaves usefully:
`drain()` returns the count and runs each closure exactly once, in post order;
`clear()` and the destructor drop without running; a closure posted from inside
`drain()` runs on the *next* drain; a closure that reposts itself takes one turn
per drain; a re-entrant `drain()` returns 0 and loses nothing; the wake callback
fires on `post()`, on the posting thread, and not on `drain()`; a capture may
`post()` from its destructor, on both `drain()` and `clear()` (which is how a
lock held across destruction would show up — as a deadlock, not a failure).

The one genuinely racy case, many producers against one consumer, asserts a
conservation law no interleaving may break: everything posted runs exactly once.
`linux-clang-tsan` is what looks for the rest.


### SMP codec
Round-trip of all `(op, version, group, seq, command, length)` combinations
(property-style over a bounded generator); reserved `res` bits set ⇒ error;
version `0b10`/`0b11` ⇒ `UnsupportedSmpVersion`; big-endian byte order asserted
against hand-written golden vectors; unknown group IDs round-trip.

### MessageAssembler
The fragmentation invariant: for a fixed sequence of N messages, the emitted
output is identical for whole delivery, byte-at-a-time, every fixed fragment
size in `[1, 64]`, and randomised cut points (seeded, deterministic). Plus:
`length` over the limit ⇒ `MessageTooLarge` and no allocation growth; truncated
tail stays buffered; `reset()` discards it; buffer never exceeds `max_buffer`.

### CBOR façade
Encode/decode round-trip for every request and response shape; absent keys ⇒
`nullopt`; wrong CBOR type for a key ⇒ sticky decode error, not a wrong value;
nesting past the limit ⇒ error; truncated buffer at every prefix length of a
valid encoding ⇒ error, never a crash or a bogus value; `MgmtError` extraction
for v1 flat `rc`, v2 `err:{group,rc}`, v2-with-flat-`rc` (protocol-notes §3),
and success (neither present).

### SmpClient
Correlation on `(seq, group, command, op)`; response with the wrong seq ⇒
dropped, request still pending, counter bumped; wrong group ⇒ same; duplicate
response ⇒ second dropped; late response after timeout ⇒ dropped via the retired
set and **not** attributed to a later request that reused the seq; timeout fires
exactly at the deadline under `ManualClock`; `cancel()` completes once with
`Cancelled`; stale `RequestHandle` cannot cancel a newer request; disconnect
fails all pending with `Disconnected`; destruction with pending requests
completes them and fires no callback afterwards; `TransportBusy` surfaces as
such; a callback that starts a new request does not corrupt the table.

### Groups
Every field of image-state decoded including the "absent means false" rule and
the single-image "absent image ⇒ 0" rule; hostile responses (`images` not an
array, 10 000 entries, 4 KiB version string, 200-byte hash) ⇒ bounded error;
`set_state` encoding with and without `hash`; reset with/without `force`.

### UploadSession (pure function — the densest suite)
Table-driven over `(state, response) → step`, with no client, transport or
clock:
happy path to completion · server returns a **larger** offset than sent ·
server returns a **smaller** offset (rewind) · server returns the **same**
offset repeatedly ⇒ bounded then `Fail` · server returns `off == 0` mid-upload
⇒ full first packet re-sent with `len`+`sha`, bounded by `max_restarts` ·
`off > image_size` ⇒ `MalformedMessage` · missing `off` on success ⇒
`MalformedMessage` · `match == false` ⇒ `ImageMismatch` · `match` absent ⇒ ok ·
**`off == image_size` on the very first response** (the device already holds the
image) ⇒ `Complete` with nothing transferred · `EBUSY`/`ENOMEM` ⇒ bounded retry,
other `rc` ⇒ immediate `Fail` · chunk timeout ⇒ retransmit the identical
request, bounded · a retransmitted first packet stays a first packet · progress
resets both the retry and the no-progress budgets · final chunk shorter than
`chunk_size` · image smaller than one chunk · resume adopts the device's offset
**without** charging the no-progress budget, upwards or downwards.

Chunk sizing: the first-packet overhead against a hand-derived CBOR envelope ·
the overhead growing with each field the packet carries · each of the three
budget inputs winning in turn · a transport with no opinion not shrinking it ·
`buf_size` too small ⇒ `MessageTooLarge` rather than a sub-32-byte chunk · a
budget of exactly 32 bytes of payload accepted.

### The upload driver (over `FakeTransport`)
What reaches the wire and when the callbacks fire, not what the rules are:
the first packet byte-for-byte against a hand-derived vector · later packets
carrying only `off` and `data` · `upgrade` present only when asked for · the
each of the three deadlines going to the right request — `kFirstChunkTimeout`
for a first packet, `kFinalChunkTimeout` for the chunk whose `off + length`
reaches the image size (A19), `chunk_timeout` for the rest · a
timeout retransmitting an **identical payload under a new sequence number** ·
progress only on confirmed advance, and not on a repeated offset · completion
**exactly once** under success, cancel, double-cancel, disconnect and
destruction · a stale handle unable to cancel a later upload · a second
concurrent upload refused · resume re-sending a first packet and continuing
from the device's offset · an empty source, an out-of-range chunk size, a
short-reading source and a failing source all refused · a wrong-typed `off`
⇒ `CborDecode` · an absent `sha` computed from the source.

### Image file handling
Fixtures are **built in code**, not checked in: `tests/support/image_builder.hpp`
writes the header and TLV areas field by field, little-endian, independently of
the parser — and one hand-written 32-byte header literal is asserted against the
builder so the builder itself is pinned. There is no `tests/data/`; every
fixture is reviewable in the diff.

The builder is deliberately permissive *and* deliberately fine-grained: each
malformation knob overrides exactly one field. An earlier version let one call
change both `ih_protect_tlv_size` and the protected area's own `it_tlv_tot`,
which kept them agreeing — so three tests passed without ever reaching the check
they were named after. **A knob that changes two fields at once cannot express
an inconsistency.**

MCUboot header parse: golden 32-byte header, field by field; wrong magic; the
v1 magic (too old, distinct from foreign); every truncation from 0 to 31 bytes;
`ih_hdr_size` below 32; `ih_img_size` above `limits::kMaxImageSize` and exactly
at it; both encryption flags; an unknown flag carried through rather than
rejected.

TLV scan: unprotected only; protected + unprotected; `ih_protect_tlv_size`
disagreeing with the protected area's own total; an area smaller than its own
four-byte header; `it_tlv_tot` past the end of the file; an entry length
overrunning the area; SHA-384 and SHA-512 found at their own lengths; two hash
TLVs ⇒ error; a hash TLV of the wrong length for its type ⇒ error; no hash TLV
and an encrypted image ⇒ `nullopt`, not an error; a zero-length entry advances
and terminates; the entry cap at N and at N+1.

SHA-256 against the NIST vectors (empty, `"abc"`, the 56- and 112-byte examples,
a million `'a'`), then the incremental property — every split of a message, a
byte at a time, and every length across a block boundary — and the streaming
path at sizes around the 4 KiB read chunk.

`ImageSource`: reads at, across and past the end; any offset order; and two
deliberately broken sources (`tests/support/fake_image_source.hpp`) — one that
fails every read, one that always reads short — because `MemoryImageSource`
cannot do either, and an error path no test can reach is indistinguishable from
one that does not work.

The shared reader under both parsers (`src/image/source_reader.hpp`,
`test_source_reader.cpp`) is also tested directly: `read_exact` refuses a read
past the end *before* asking the source, passes on a source's failure, and
refuses a short read; the little-endian loads use bytes with the high bit set,
so a load that sign-extends shows up as a wrong value.

`support/dfu_package/` (`test_dfu_package.cpp`), the multi-image package reader
of ADR-0021, over archives written byte by byte by `tests/support/zip_builder.hpp`
and images by `image_builder.hpp`, neither sharing code with the reader. Most
cases are refusals, because the package is untrusted: deflate, encryption and
zip64 refused by name; a CRC, size or name mismatch; every truncation and every
flipped byte of a real archive; the JSON bounds on depth, count, length and
size, and RFC 8259's edge cases; `image_index` as a string (what
`generate_zip.py` writes), as a number, absent, out of range and repeated; the
manifest's size and `version_MCUBOOT` checked against the file and its MCUboot
header; dependency TLVs from both areas, with a broken area refused; and a
package whose image depends on a newer version of another image it carries
refused, by MCUboot's default comparison, so the build number does not count
(ADR-0022).

`support/dfu_app/`'s `PackageUpdate` (`test_package_update.cpp`), the few lines
both examples share between a package file and `FirmwareUpdater::start()`: one
target per image in index order, image 0 committed by the client and the rest by
the device, the per-image override refused for an image the package lacks, the
file read whole and a bad one refused, and `parse_commit()`'s accepted and
refused forms.

`support/dfu_app/`'s `FileImageSource`, the source every example reads firmware
through (`test_file_image_source.cpp`): a missing or empty file is refused, reads
are clamped at the end, end of file is zero bytes and later reads still work,
and a file that shrinks after `open()` is a broken source. `support/` is outside
the coverage filter ([`quality-gates.md`](quality-gates.md) §6), so these tests
add proof, not a gate number.

### Update state machine (pure function)
Every transition in [`design.md`](design.md) §8, and every row of its
failure/recovery table, driven directly as `(state, event)` pairs — no client,
no transport. Exhaustive switch coverage is checked by the branch-coverage gate.

`tests/unit/test_update_state_machine.cpp`. Beyond the happy
path: the planner's four cases and their `UploadOnly` variants; a slot table
with no active slot, and one whose slot reports no hash; `ImageAlreadyPending`
recovered exactly once; `Busy` reset forced exactly once; a lost reset response
treated as success; a rollback recognised **from the flags** and distinguished
from a swap that simply has not happened; the confirmation fork in both modes;
cancellation from every non-terminal state; and an illegal event in **every**
state, because "this one silently swallows a stray event" is precisely the hole
a spot check leaves.

The `[multi]` cases cover the image list (ADR-0021): every image staged before
the one reset; the mark recovery spent once per image; the three answers of
the device contract (applied, still applying in either variant, failed); the
timeout and a failed read while waiting; a revert of one `Client` image; a
resume after the reset that stages nothing; each `Client` image confirmed in
turn and all read back confirmed; `UploadOnly` over two images; and a context
with no image to work on. ADR-0022 added:
* a device image on trial counts as applied;
* the commit wait after the confirm: its success, a trial the other MCU
  reverts, and the timeout;
* the commit wait reached directly when no client image is on trial, after
  the reset or on resume;
* in both waits: a dropped link asks for a reconnect, a lost read is
  retried, and anything else is fatal.

The `[confirm]` cases for ADR-0023 cover `Confirming` and `VerifyingConfirmed`:
* a dropped link reconnects;
* a lost answer is re-read once, and a second is fatal with `revert_pending`;
* any other error is still fatal;
* the re-inspection completes a confirmed image;
* it re-confirms one still on trial without asking the application twice,
  and asks when no approval is on record;
* on two images, a link lost after the confirm goes on to the commit wait.

`test_firmware_update.cpp`'s `[multi]` cases drive the same against the
two-image simulator, each asserting there was exactly one reset:
* a resume in the wait, and a resume in the confirmation window;
* an image-1 upload interrupted after image 0 finished;
* for ADR-0022: the commit observed after the confirm; a commit that never
  comes (`Timeout`, nothing to revert); the link dropped mid-apply and
  reconnected; a lost read retried; and a resume after the confirm that
  waits for the commit;
* for ADR-0023: a confirm whose answer is lost, and one sent into a
  dropped link, each ending committed with one approval.

Two single-image `[confirm]` cases run the same recoveries. A link lost
before the confirm reconnects and confirms, and the application is asked
once. A confirm whose answer is lost is read back through `VerifyingBooted`
and completes.

### The example as a test (`examples/cli_dfu/`)

`cli_dfu_demo` runs the example with `--quiet` and checks its exit code, which is
zero only for an update that reached `Completed`. That makes "the example
performs a complete simulated update end to end" a check rather than a claim, and
it covers ground no other suite does:

* it is the **only multi-threaded program in the tree**, so it is what the
  `linux-clang-tsan` job has to work with beyond `Dispatcher`'s unit tests;
* it is the only place `Dispatcher`'s **wake callback** has a real caller;
* it runs on a **real clock**, not `ManualClock` — so a deadline that only works
  because a test advanced time by exactly 1 ms would show up here;
* it exercises the **application's half of the reconnect protocol**: a dropped
  link, a fresh transport, `rebind_transport()`, `resume_after_reconnect()`.

Two more run the multi-image update of ADR-0021 end to end, from a package
built in memory (`--demo-package`) through `smply::dfu_package` and
`PackageUpdate` to the image-list `start()`, against a stub with a second image
it commits itself. Each passes on its output lines, never on the exit code:
* `cli_dfu_package`: `update Completed`, and `image 1 (device): committed`;
* `cli_dfu_package_apply_fails`: the stub fails to apply image 1, so the update
  must fail with "device did not apply an image", report image 1 not applied,
  and warn that the next reset reverts image 0. Both patterns were checked
  against the other test's output, so neither passes on the wrong outcome.

Its device is `examples/stub_device/stub_device.*`, shared with `serial_dfu`, and that device is **not** a
protocol reference — `ServerSimulator` is. The stub answers the five commands one
clean update needs and no more. Its one extension is a second image the device
commits itself, the simulator's device-committed mode cut down to success and
one failure, so the multi-image examples have a device to take a package. If the two ever disagree, the simulator is right;
growing the stub to match it would be building a second test double outside
`tests/`.

### The serial example as a test (`examples/serial_dfu/`)

Two ctests run a whole update over a pseudo-terminal, against the same stub
device, behind `pty_stub.*`. That makes them the first test in which smply's
serial framing crosses a real byte stream in both directions for a complete
update. They differ only in the shape of the reset, which is roadmap O7's
question:
* `serial_dfu_pty_uart`: the port stays open. The updater leaves
  `AwaitingDisconnect` on the grace timer. Boot banners arrive as ignored
  lines, and an over-long log line is dropped and counted.
* `serial_dfu_pty_cdc`: the port vanishes, and the adapter reports the
  hang-up. It returns as a different `/dev/pts/N` behind the same symlink. The
  example reopens the path, and `devices=2` proves it reached the new tty.
* `serial_dfu_pty_package`: the UART shape, with the demo's two-image package
  (ADR-0021). `images=2 applied=1 committed=1` at the end of the summary line
  says both were staged, and image 1 was applied and then committed by the
  stub.

Both pass on one summary line (`PASS_REGULAR_EXPRESSION`), never on the exit
code alone: an update that completed with the wrong reset shape, a framing
error or a refused send is a failure. Each was run 50 times under TSan and
under ASan with no failure. The stub also bounds inbound packets at
`kBufSize − 2` in declared length, which is the device's real serial limit
(protocol-notes A25).

### BLE framing, link state and send admission (`transports/common/`)

The part of the BLE transport that needs no radio, and therefore the part that
is tested everywhere rather than only on the bench — which is the reason to keep
putting things here: this directory is unit-tested, linted and
coverage-measured on every platform, while the adapter beside it gets MSVC and a
bench. Fragment sizing at the 23-byte minimum
ATT MTU (⇒ 20) and at the sizes real stacks negotiate; the clamp at both ends,
including a PDU of zero, which without it would give a fragment size of zero and
an adapter that never progresses; short, exact-multiple and remainder messages,
because an exact multiple is where an off-by-one puts a zero-length GATT write on
the air; and a many-fragment message reassembling byte for byte, which is what
"no additional framing" (protocol-notes §8) actually means.

**Two `[hardware-golden]` cases** (`tests/unit/test_image_group.cpp`) pin the
device's own bytes: an image-state and a slot-info response captured from the
NUCLEO-WB55RG, indefinite-length exactly as the reference server emits them
(§9 A18). They encode a rule worth stating, because hand-built goldens are
naturally definite-length and hid A18 completely: **build every new
response golden in both CBOR encodings**, and where a real device response
exists, pin that too.

Plus `LinkState`: `close()` idempotent, and callbacks refused from the moment it
*begins* rather than when it ends — the rule adapters get wrong, because there
are usually callbacks already in flight at that point.

And `SendQueue`, which is send admission as a value: one writer,
one message allowed to wait, a third refused. The cases that earn their keep are
the ones about the *claim* rather than the queue — that the writer keeps it while
a message waits, that it is released only by finding nothing left, and therefore
that "a message waits but nobody is writing" is unrepresentable. A second writer
would interleave two messages' fragments, and with no framing to resynchronise on
(§8) the damage would be a mis-framed message rather than an error anyone
reports. What these cases cannot check is the adapter's *use* of the type: that
every call is made under its mutex, never across a suspension point, and that a
writer starts on exactly one admission. That is bench territory
([`design.md`](design.md) §10 says which parts the bench discharged).

### Serial framing (`transports/serial/`)

The same argument as the section above, one transport over: every byte of
MCUmgr's console framing is protocol rather than platform, so all of it is
tested on every preset instead of on a bench that does not exist yet. The two that carry the most weight are the ones whose oracle is not
this repository.

**Neither oracle is smply's code, and that is the whole design.** The CRC is
checked against the published CRC-16/XMODEM check value (`0x31C3` over
`"123456789"`) and base64 against RFC 4648's seven vectors. Both would pass a
round trip through the wrong variant — a reflected CRC round-trips perfectly
and matches no device. And `SerialPeer`, in the test file's anonymous
namespace, is a transcription of Zephyr's `mcumgr_serial_tx_pkt()` and
`mcumgr_serial_process_frag()` written from the C with its own base64, its own
CRC and its own `int` arithmetic. The encoder is required to be **byte-identical
to it for every packet size from 1 to 300**, and the decoder to accept what it
produces — which is a far stronger claim than "this decodes what this encoded".
It is the same reason `test_ble_framing.cpp` writes its own hex parser.

Beyond that: a hand-computed single-frame golden; the frame-size sequences at
91, 92, 182, 183, 184 and 185 bytes, which are where the reference's
refuse-to-split-the-CRC arm fires and where a naive 93-bytes-per-frame encoder
silently diverges; and four invariants asserted on *every* frame of every round
trip — within 127 bytes, correctly marked as opening or continuation,
newline-terminated, and whole base64 quartets, since the receiver decodes each
frame on its own.

`LineSplitter` gets the fragmentation invariant in the shape
`test_assembler.cpp` established: the same lines out for whole delivery, for
every fixed chunk size from 1 to 64, for CRLF and bare LF. A reader that
depends on where a read happened to end works on a fast host and fails on a
slow one.

`SerialDeframer` gets one case per bound and one per framing rule: an
over-long line dropped with **`capacity()` untouched** rather than merely
emptied afterwards; a declared length above the cap refused while at most one
frame's worth has been accumulated; a body longer than its own header, in both
the opening frame and a continuation; an orphan continuation; undecodable
base64; a corrupted CRC costing one packet and not the stream. Plus the case
this module exists for — **log lines and a shell prompt interleaved between the
frames of one packet, and the packet still arrives**, which is the stream over
which a third-party client could not complete an upload on the bench.

What none of it checks is a port. No CI job and no bench has put a serial byte
on a wire, and `architecture.md` §11 says so rather than implying a working
transport.

### The serial port adapter (`transports/serial_port/`, `tests/serial_port/`)

Its pure parts are in the unit suite (`test_serial_port_config.cpp`):
* configuration bounds, the baud table and the errno mapping;
* `frame_message()` against `SerialFramer`;
* `SerialInbound` over arbitrary read boundaries, console noise, over-long
  lines, CRC failures and `reset()`.

`tests/serial_port/test_serial_port.cpp` puts the adapter on the slave end of a
pseudo-terminal and plays the device on the master end:
* frames written and deframed both ways;
* noise ignored and counted, and an over-long line dropped and counted;
* a hang-up delivering exactly one `on_disconnected()`;
* `close()` idempotent, and silent even for a packet already queued in the
  `Dispatcher`;
* `TransportBusy` from a pty nobody reads, with `close()` still returning;
* `open()` refusing a missing path (`Disconnected`), a non-tty
  (`InvalidArgument`) and a bad configuration.

**Wait after the traffic.** "An idle link stays up after traffic" sleeps
after an exchange and checks that no disconnect arrived. That is the only
case that failed when the adapter mistook an empty `VMIN = 0` read for end of
file. Every other case had already checked its packets before the spurious
disconnect was drained (handoff.md, "Serial ports").

**Check the tty's own state, not what root is allowed to do.** "close gives up
exclusive use" reads `TIOCGEXCL` after `close()`. A reopen test would pass as
root whatever the adapter left behind, and root is what this development
container runs as (handoff.md, "Serial ports").

**Assert only what the queue guarantees.** A message admitted by
`StartWriter` waits in `SendQueue` until the I/O thread takes it. So a third
offer can be refused before any offer was ever deferred, and
`send.deferred >= 1` is *not* implied by `TransportBusy`. An early version
asserted it, and failed in 15 of 50 runs.

## 4. Component tests (`tests/component/`)

A second executable, so "the unit suite is green but the stack is not" is
something CTest can say. Four files:

**`test_simulator.cpp`** checks the double against the specification it claims
to implement, driven with hand-built requests and **no smply client at all** --
a simulator bug is then diagnosed directly rather than through three layers of
library. Every §6 rule from the server side, both error shapes side by side, and
each optional command present and absent.

**`test_round_trip.cpp`** drives the real stack
(`ImageManagement`/`OsManagement` → `SmpClient` → `FakeTransport` →
`ServerSimulator`) under `ManualClock`. It covers:

* **an upload reproduces the source image byte for byte, in both SMP
  versions**;
* progress advances monotonically and ends at the total;
* image already in the target slot ⇒ complete on the first packet, no data
  sent (rule 9a);
* a device without the image check re-uploads and reports no verdict;
* the device reboots mid-upload ⇒ restart from zero, still byte-exact;
* the server names an offset *ahead* of what was sent ⇒ followed, corrected,
  still byte-exact;
* a disconnect mid-upload ⇒ one `Disconnected`, then `resume()` on a new
  transport completes it;
* `mcumgr_parameters()` drives the chunk size; `ENOTSUP` ⇒ the conservative
  default, and both uploads land byte-exact;
* test → reboot → confirm keeps the new image; without the confirm the second
  reboot **reverts**;
* confirming a slot that is not running is denied (§7);
* erase ⇒ the state read simply omits the slot;
* a reset is accepted before the link drops, and an `EBUSY` reset is retried
  with `force`;
* destroying `ImageManagement` mid-upload completes the callback exactly once.

**`test_firmware_update.cpp`** drives the whole update into the
simulator, with a small `Application` helper playing the part the updater
refuses to play: reconnecting after the reset, and deciding whether the new
image is good. Shipped:

* a clean update in **both** SMP versions, asserting the command sequence and
  that the device ends up running the new image, confirmed;
* all three modes, including that `ConfirmImmediately` never asks and
  `UploadOnly` never resets;
* an image already in the secondary slot ⇒ no upload; already running and
  confirmed ⇒ immediate completion;
* an upload interrupted by a disconnect ⇒ two trips through the application,
  and the flash still byte-exact;
* `EBUSY` reset retried with `force`; a **lost** reset response treated as
  success (A3); a device that never drops the link released by the grace timer;
* the device reverting ⇒ `rolled_back`, recognised from the flags;
* a refused confirm, and an application that declines to confirm ⇒ both
  terminal with `revert_pending` set;
* an application that cannot reconnect; a file that is not MCUboot firmware, is
  unreadable, has a broken TLV area, or carries no hash TLV — each refused
  before a single byte goes on the wire;
* cancellation mid-update, destruction mid-update, and a callback that outlives
  the updater while the client is still alive.

**`test_async.cpp`** drives `smply::asyncutil` (ADR-0019) against the same
simulator. It covers:
* a coroutine that reads, uploads and reads again, in order;
* two uploads back to back from one coroutine. The second starts from inside the
  first's completion callback, where the coroutine resumes, which is the chained
  callback the adapter relies on being legal;
* an error delivered to the coroutine rather than lost;
* a `Task` destroyed while suspended: the operation still completes and nothing
  resumes a freed frame, which the sanitizer presets check;
* one task awaiting another; an exception rethrown by `result()`; and an
  operation that completes inline, which must not suspend;
* a worker thread blocking on `post_for_future()` while the test thread pumps,
  under TSan in `linux-clang-tsan`; and a cleared dispatcher breaking the
  promise instead of hanging.

## 5. Fuzzing (`tests/fuzz/`)

libFuzzer on Linux/Clang, behind the `linux-clang-fuzz` preset. The whole tree
is compiled with `-fsanitize=fuzzer-no-link,address,undefined` so every
translation unit is instrumented, and each target adds `-fsanitize=fuzzer` at
link time, which is what supplies libFuzzer's `main()`.

```sh
cmake --preset linux-clang-fuzz
cmake --build --preset linux-clang-fuzz
build/linux-clang-fuzz/tests/fuzz/fuzz_tlv_scan tests/fuzz/corpus/fuzz_tlv_scan
```

The targets are **not** part of `ctest`: a fuzz target runs until it is told to
stop. CI drives them explicitly instead — see §5.3.

### 5.1 The targets

Every one asserts a property beyond "no crash". "No crash" is what ASan and
UBSan give for free; an assertion is what makes the target notice a bound that
has quietly stopped holding.

| Target | Input | Property asserted |
| ------ | ----- | ----------------- |
| `fuzz_header` | 8+ bytes | decoding is *faithful*: anything accepted re-encodes to the bytes it came from, and decodes again to the same header. A client and a device that disagree about what was on the wire is how a response reaches the wrong request. |
| `fuzz_assembler` | a stream, split at fuzzer-chosen points (the first byte is the fragment size) | buffering never exceeds `max_buffer`, at every step and at the peak; a completed message's payload length equals its header's; the buffer returns to empty between messages |
| `fuzz_cbor_image_state` | arbitrary CBOR, delivered as a correlated response to a real `get_state` | `kMaxImages`, `kMaxVersionStringLength` and `kMaxImageHashLength` all hold on the decoded result — no container or string sized by the device |
| `fuzz_cbor_upload_response` | arbitrary CBOR, delivered into a live upload | the session never reports more transferred than the image holds, whatever offset the device claims (protocol-notes §6, rule 5) |
| `fuzz_mcuboot_header` | arbitrary bytes | the trailer offset a parsed header implies is never below the header itself — the arithmetic that indexes the file cannot be made to point backwards |
| `fuzz_dfu_package` | arbitrary bytes as a DFU package, and the same bytes as a JSON document | a package that reads has its images sorted by index, each index once and at most `kMaxImageIndex`, each image a view inside the input, each dependency list within `kMaxImageTlvs`, and no image depending on a newer version of another image in the package. The seed corpus holds a package written by nRF Connect SDK's own `generate_zip.py` (S38), so mutation starts from the real layout, plus its manifest alone, a one-image package, a deflated one, and one whose images disagree |
| `fuzz_tlv_scan` | arbitrary bytes as a `MemoryImageSource` | the scan terminates, and any hash it returns is 32 or 64 bytes — `IMAGE_SHA_LEN`, not a length the file chose (protocol-notes §6) |
| `fuzz_smp_client_rx` | an arbitrary stream fed to a live client with a request pending | a completed request is only ever completed by a response matching its `seq`, `group` and `command`; unmatched responses never exceed received ones |
| `fuzz_serial_deframe` | a console stream, read in fuzzer-chosen chunks (the first byte is the read size) | neither the line buffer nor the packet buffer exceeds its bound, at every step; a framing error leaves nothing partial behind; a delivered packet is CRC-verified and within the cap. The only target over a transport, and the first whose input is *expected* to be mostly noise — a shared console carries far more shell and log output than frames |

Three of these — the two CBOR targets and `fuzz_smp_client_rx` — go through a
real `SmpClient` rather than calling a decoder directly, because the decoders
are file-local. That is the better target anyway: it fuzzes framing,
correlation, error extraction and the group decode together, which is the path
a device actually drives.

### 5.2 Corpora

`tests/fuzz/corpus/<target>/`, seeded from the vectors the unit suites already
build by hand (`tests/support/message_builder.hpp`,
`tests/support/image_builder.hpp`, `support/minicbor/minicbor.hpp`) — so
seeding was extraction, not invention — plus inputs the soak found worth
keeping.

A crash reproducer is committed to the corpus **with its fix**. That is what
makes the corpus a regression suite rather than a cache: the smoke job replays
every committed input on every pull request, so a fixed input coming back is a
build failure, not a rediscovery.

### 5.3 How CI runs them

| Job | When | What | Blocking |
| --- | ---- | ---- | -------- |
| `linux-clang-fuzz-smoke` (`ci.yml`) | every push and pull request | 20 000 runs per target over the committed corpus | yes |
| `nightly-fuzz-soak` (`nightly-fuzz.yml`) | 03:17 UTC daily, or on demand | 30 minutes per target, corpus and any reproducer uploaded, one issue opened per target on a find | no — advisory |

Both copy the corpus out of the tree before running: libFuzzer writes what it
discovers into the directory it is given, and what the committed corpus contains
is a decision for a person, not for a CI run.

There is deliberately no one-off soak target: the standing nightly job replaces
it. A schedule outlives a measurement, and the same corpus that finds nothing
today may find something tomorrow.

## 6. Hardware interoperability (`tests/hil/`)

Opt-in target `smply_hil` (`SMPLY_BUILD_HIL=ON`, preset `windows-hil`), never
part of the PR gate. Requires the bench in `tests/hil/README.md`: a NUCLEO-WB55RG
running MCUboot + the pinned `smp_svr` over BLE, reachable from a Windows host,
with the exact west manifest, Kconfig snapshots and coprocessor firmware recorded
so results are reproducible.

**The serial cases have never run.** `test_hil_serial.cpp` puts the serial
port adapter (its Win32 half) on the bench's console UART. The `serial` group
(echo and image state) is in the default run, and the exploratory
`serial-update` group is not. Both were written without the bench.
`tests/hil/README.md` says what their first run must show.

**Shape.** `test_hil_cases.cpp` is a Catch2 suite over the public API **plus
two headers no application consumer gets**: `support/dfu_app/reconnect_policy.hpp`
and `transports/winrt_ble/winrt_ble_transport.hpp` (`tests/hil/support/rig.hpp`).
That is not a leak in the seam — the rig has to *be* an application, and an
application owns its reconnect policy and constructs its own adapter. It is
worth stating exactly because one of those, the adapter's `send_counters()`, is
a per-adapter diagnostic rather than part of the `Transport` contract. The suite
is
driven through `support/rig.*` — the example's pump loop made callable one
operation at a time (connect, read state, upload, resume, drop the link, run a
whole update, reconnect on a policy). Every case reads the bench from the
environment and **SKIPs** without it; every case works out its own target (the
image not currently running), so it can start from either of the two firmware
images. `run_hil.py` supervises: a baseline reflash over ST-LINK and a UART
capture per *group*, a hard deadline and a JUnit report per case, the case's
timeline and `HIL-METRIC` lines kept as evidence, and a **pass / fail /
unavailable** verdict — a skipped case, a missing probe or an unreadable report
is unavailable, never a pass ([ADR-0015](decisions/ADR-0015-hardware-evidence.md)).

**Cases.** Fourteen `TEST_CASE`s, tag `[hil]`, which `run_hil.py` organises into
thirteen groups — the resume-after-restart scenario is two cases that must run
in order without a reflash between them. **Twelve groups run unattended**
(`--cases all`); the thirteenth is manual and is excluded from it. Three
numbers, and they are all different, so each is worth stating rather than
picking one: *fourteen* is what `ctest` would list, *thirteen* is what the
supervisor schedules, *twelve* is what a green unattended run reports.

Presence of params, slot info and echo · clean update (test then confirm) ·
confirm-immediately · an image the device already runs is not uploaded (the
updater's pre-flight) · the server's own already-present check on a re-upload
(rule 9a) · interrupted upload (the application closes the link at half way)
then `resume()` · resume after an application restart (two cases, run without a
reflash between them) · corrupted image refused (one body byte flipped after
signing) · test boot then reset without confirm ⇒ **rollback observed** · reset
(disconnect seen, device back) · erase, including of a slot marked for test ·
**an image-group refusal carrying only what the peer's SMP version allows** —
cheap, writes no flash, and is the measurement that resolved open question O2
(§9 A24); run it as `--cases o2` with `SMPLY_HIL_SMP_VERSION` set to 1 and to 2
and compare the bundles · reconnection gives up when the device does not
return — **manual**: a person powers the board off when the case prints its
`HIL-MARK` line, which is why it is excluded from `--cases all`. An
earlier design had the supervisor erase the device with the programmer on that
line; that raced the reconnect, because STM32CubeProgrammer toggles reset to
attach and the device re-advertises before the erase halts it. The give-up path
itself is covered deterministically on every push by `cli_dfu
--flaky-reconnect 99`. The cases record measurements as `HIL-METRIC` lines, which `run_hil.py` scrapes
into each case's `summary.json` entry: admission (`deferred_sends`,
`refused_sends`), the peer's buffering (`buf_size`, `buf_count`), timing
(`close_ms`, `disconnect_seen_ms`, `upload_ms`, `update_ms`, `reconnect_ms`,
`give_up_ms`, `reboot_total_ms`), resume (`resumed_from`, `abandoned_at`), the
trial-boot slot listing (`slots_listed_during_trial`) and the requested
`smp_version`.

Two of those are not diagnostics but the evidence a green run rests on. **A
green sequential suite means nothing unless `deferred_sends > 0`**: zero
everywhere says the send-admission race did not occur that run, not that the
mailbox absorbed it (§9 A22). And `buf_count` is the input open question O3 was
waiting for.

**Cross-check** — `tests/hil/crosscheck.py`. From the same baseline, the same
update is installed by smply, by `smpmgr` over BLE and by `mcumgr-client` over
the UART shell transport. All three are third-party tools for behavioural
comparison, never protocol references (ADR-0015 decision 3); every divergence is
traced to Zephyr or MCUboot source and recorded in
[`protocol-notes.md`](protocol-notes.md) §9 **before** any smply behaviour
changes.

`mcumgr-client` over UART is the **oracle**: device state is read back through a
path none of the BLE clients touch. Note what that does and does not cover — the
oracle is used by `crosscheck.py`, and **the case suite above is not oracled**.
Its cases read device state through smply itself, which is a weaker
arrangement and is why the cross-check begins by proving the oracle can tell two
states apart at all (flash A, flash B, flash A, and require that the diff names
exactly what changed and invents nothing). A comparison whose instrument cannot
detect disagreement is not a comparison.

Two tiers, computed independently:

* **Tier A** — normalised image state at three checkpoints per client: after the
  baseline flash, **during the trial boot**, and after the confirm. The middle
  one carries the information. Reading only before and after cannot fail, because
  every client is already required to end in the same place; between the reset
  and the confirm the active slot holds the new image *unconfirmed* while the
  fallback holds the old one *confirmed*, so four fields distinguish a client
  that took a shortcut. Every client therefore installs and confirms as two
  separate invocations — which is what `winrt_ble_dfu`'s `--mode test-only` and
  `--mode confirm-only` are for.
* **Tier B** — decoded SMP operation sequences from an HCI capture (BTVS +
  Wireshark; **BTVS needs an elevated shell**), compared between the two BLE
  clients. Sequence numbers, fragmentation, timing and chunk size are recorded
  and deliberately **not** compared; what is compared is that the required
  operations appear in the same order, as a subsequence rather than an equality
  — smply's updater reads image state before uploading and another client need
  not. The SMP reassembly and CBOR decoding are ours
  (`tests/hil/tools/smp_decode.py`), from §2 and §8, never inferred from the
  tools being compared.

Tier B being unavailable never turns a Tier A pass into a failure. Without a
capture the run's correct outcome is **exit 2 with Tier B unavailable**, and
that is reported rather than omitted (ADR-0015) — a capture is never silently
skipped, and a capture holding zero packets is a distinct outcome from one that
was never attempted.

The advisory `hil.yml` workflow is committed and **not commissioned**: no
self-hosted runner exists yet, so the suite has only run from the bench by hand.
Commissioning it is in the roadmap's acceptance gaps.

## 7. Determinism rules

* No wall-clock time in unit or component tests (`ManualClock` only).
* No sleeps and no network. Threads only where a test exists to exercise them:
  `test_dispatcher.cpp` and the future adapter in `test_async.cpp`, both run
  under TSan.
* No filesystem, except a uniquely named temporary file that the test itself
  removes (`test_file_image_source.cpp`).
* Randomised tests use a fixed seed printed on failure and reproducible from it.
* Tests assert on structured values, never on formatted error strings.
