# Testing strategy

Goal: **every protocol path is reachable without hardware, without Bluetooth and
without real time.** The core is sans-IO precisely so this is possible.

Framework: **Catch2 v3** ([ADR-0012](decisions/ADR-0012-test-and-fuzz-tooling.md)).

## 1. Test levels

| Level | Location | Runs in | Gate |
| ----- | -------- | ------- | ---- |
| Unit | `tests/unit/` | < 5 s total | every PR, all 3 toolchains |
| Component (full stack over a simulated device) | `tests/component/` | < 20 s | every PR |
| Fuzz (smoke: fixed corpus, short) | `tests/fuzz/` | < 60 s | every PR (Linux/Clang) |
| Fuzz (soak) | same targets | 30 min | nightly |
| HIL / interoperability | `tests/hil/` | minutes | manual + nightly on a self-hosted runner |

## 2. Test doubles (`tests/support/`)

### `ManualClock`
`Clock` implementation with `advance(Duration)`. **No test may call
`std::chrono::steady_clock::now()`.** A CI grep enforces this in `tests/unit`
and `tests/component`.

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
    bool single_image           = true;      // omits "image" in state
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
`reset_busy_once()`, plus `load_slot()`, `reboot()` and
`rebind_transport()` for the device's own state. `answer_offset_once()` changes
only the *answer*, never the flash: a client that follows the correction is put
right on the next round trip, and one that computes its own offsets flashes a
corrupt image -- which is exactly the asymmetry the acceptance test relies on.

`ServerSimulator` is how the update state machine gets end-to-end coverage
without hardware. It is **not** a reference implementation and is never linked
into the library. It models **one image and two slots**, and refuses an upload naming any other
image with `NoFreeSlot` rather than quietly writing slot 1; a second image pair
is follow-up work, recorded in the roadmap.

## 3. Required unit coverage

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
first chunk taking `kFirstChunkTimeout` and the rest `chunk_timeout` · a
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

### Update state machine (pure function)
Every transition in [`design.md`](design.md) §8, and every row of its
failure/recovery table, driven directly as `(state, event)` pairs — no client,
no transport. Exhaustive switch coverage is checked by the branch-coverage gate.

## 4. Component tests (`tests/component/`)

A second executable, so "the unit suite is green but the stack is not" is
something CTest can say. Two files:

**`test_simulator.cpp`** checks the double against the specification it claims
to implement, driven with hand-built requests and **no smply client at all** --
a simulator bug is then diagnosed directly rather than through three layers of
library. Every §6 rule from the server side, both error shapes side by side, and
each optional command present and absent.

**`test_round_trip.cpp`** drives the real stack
(`ImageManagement`/`OsManagement` → `SmpClient` → `FakeTransport` →
`ServerSimulator`) under `ManualClock`. Shipped as of P11:

* **an upload reproduces the source image byte for byte, in both SMP
  versions** -- the phase's acceptance criterion;
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

Waiting on `FirmwareUpdater` (P12), which is what these need to be written
against: the `TestThenConfirm` / `ConfirmImmediately` / `UploadOnly` mode
matrix and its exact command sequence; a lost reset response
(protocol-notes §9 A3); the device booting the **old** image ⇒ `RolledBack`;
`IMAGE_CONFIRMATION_DENIED` reported as terminal; cancellation in every
non-terminal state.

## 5. Fuzzing

libFuzzer, `-fsanitize=fuzzer,address,undefined`, Linux/Clang.

| Target | Input | Assertion |
| ------ | ----- | --------- |
| `fuzz_header` | 8 bytes | no crash; decode/encode round-trip when decode succeeds |
| `fuzz_assembler` | arbitrary stream, split at fuzzer-chosen points | no crash, no unbounded memory, buffer ≤ limit |
| `fuzz_cbor_image_state` | arbitrary CBOR | no crash; bounded allocation |
| `fuzz_cbor_upload_response` | arbitrary CBOR | no crash |
| `fuzz_mcuboot_header` | arbitrary bytes | no crash |
| `fuzz_tlv_scan` | arbitrary bytes | no crash; terminates (iteration cap) |
| `fuzz_smp_client_rx` | arbitrary stream fed to a live client with a pending request | no crash; the pending request is never completed with a wrong-seq response |

Corpora in `tests/fuzz/corpus/<target>/`, seeded with: valid encodings produced
by the unit tests, every malformed case named above, and regression inputs. A
crash found in CI is committed to the corpus with the fix.

## 6. Hardware interoperability (`tests/hil/`)

Opt-in target `smply_hil` (`SMPLY_BUILD_HIL=ON`), never part of the PR gate.
Requires a Zephyr board running MCUboot + `smp_svr` (BLE transport), described
in `tests/hil/README.md` together with the exact west/Kconfig snapshot used, so
results are reproducible.

Cases: clean update · interrupted upload (link dropped mid-transfer) then resume ·
resume after a full application restart · image already present · deliberately
corrupted image ⇒ rejected · test boot then confirm · test boot then reset
without confirm ⇒ **rollback observed** · reset · erase · slot-info and
mcumgr-params presence/absence.

Cross-check: the same firmware and device are updated with Zephyr's supported
`mcumgr-client`, and both the resulting image-state output and a captured
`btmon`/HCI trace are compared against smply's. Divergence is a defect in smply
or a new entry in [`protocol-notes.md`](protocol-notes.md) — never a silent
adjustment.

## 7. Determinism rules

* No wall-clock time in unit or component tests (`ManualClock` only).
* No sleeps, no threads, no network, no filesystem outside `tests/data/`.
* Randomised tests use a fixed seed printed on failure and reproducible from it.
* Tests assert on structured values, never on formatted error strings.
