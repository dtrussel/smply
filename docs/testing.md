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

### `ServerSimulator`
A deterministic in-memory MCUmgr server built on `FakeTransport`. It implements
groups 0 and 1 per [`protocol-notes.md`](protocol-notes.md), including the
awkward parts: offset correction, session resume by `sha`, `off == 0` restart,
`match` on the final chunk, reset-then-drop, and slot/hash bookkeeping across a
simulated reboot and swap. It is configurable to emulate real-world variation:

```cpp
struct ServerConfig {
    Version   version = Version::V1;      // v1 rc vs v2 err
    std::uint32_t buf_size = 256;
    bool      supports_mcumgr_params = true;   // else ENOTSUP
    bool      supports_slot_info     = false;
    bool      image_check_enabled    = true;   // emits "match"
    bool      single_image           = true;   // omits "image" in state
    Duration  response_delay{0};
};
```

`ServerSimulator` is how the update state machine gets end-to-end coverage
without hardware. It is **not** a reference implementation and is never linked
into the library.

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
Table-driven over `(state, response) → step`:
happy path to completion · server returns a **larger** offset than sent ·
server returns a **smaller** offset (rewind) · server returns the **same**
offset repeatedly ⇒ bounded then `Fail` · server returns `off == 0` mid-upload
⇒ full first packet re-sent with `len`+`sha`, bounded by `max_restarts` ·
`off > image_size` ⇒ `MalformedMessage` · missing `off` on success ⇒
`MalformedMessage` · `match == false` ⇒ `ImageMismatch` · `match` absent ⇒ ok ·
`EBUSY`/`ENOMEM` ⇒ bounded retry, other `rc` ⇒ immediate `Fail` · chunk timeout
⇒ retransmit the identical request, bounded · final chunk shorter than
`chunk_size` · image smaller than one chunk · chunk sizing when
`buf_size` is tiny ⇒ `MessageTooLarge` rather than a sub-32-byte chunk ·
resume after disconnect with a matching `sha`.

### Image file handling
MCUboot header parse: golden 32-byte headers; wrong magic; truncated; absurd
`ih_hdr_size`/`ih_img_size`. TLV scan: valid protected + unprotected areas;
`it_tlv_tot` larger than the file; zero-length TLV (infinite-loop guard);
missing `IMAGE_TLV_SHA256`. SHA-256 against NIST vectors plus a multi-megabyte
streaming case with every buffer boundary.

### Update state machine (pure function)
Every transition in [`design.md`](design.md) §8, and every row of its
failure/recovery table, driven directly as `(state, event)` pairs — no client,
no transport. Exhaustive switch coverage is checked by the branch-coverage gate.

## 4. Component tests

Full stack (`FirmwareUpdater` → `ImageManagement`/`OsManagement` → `SmpClient` →
`FakeTransport` → `ServerSimulator`) under `ManualClock`:

* clean update, `TestThenConfirm`, verifying the exact command sequence issued;
* clean update, `ConfirmImmediately` and `UploadOnly`;
* upload interrupted by a disconnect at every 10 % boundary, then resumed;
* server forces a restart from `off == 0` mid-upload;
* image already present in the secondary slot ⇒ upload skipped;
* image already active and confirmed ⇒ immediate `Completed`;
* invalid image (server rejects the first chunk with a magic error);
* reset response lost, link simply drops ⇒ still succeeds (protocol-notes §9 A3);
* reset returns `EBUSY` ⇒ retried with `force`;
* device boots the **old** image ⇒ `RolledBack`;
* `IMAGE_CONFIRMATION_DENIED` ⇒ terminal with a clear report;
* cancellation in every non-terminal state;
* the same scenarios with `ServerConfig::version = V2` and with
  `supports_mcumgr_params = false`.

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
