# Detailed design

Mechanics of each component. Read [`architecture.md`](architecture.md) first;
wire facts referenced as *(PN §x)* live in [`protocol-notes.md`](protocol-notes.md).

---

## 1. SMP codec (`src/smp/codec.*`)

Pure functions over an 8-byte header. No allocation, no state.

```cpp
struct Header {
    Operation op;      Version  version;
    std::uint8_t flags;  std::uint16_t length;
    Group group;       std::uint8_t seq;  std::uint8_t command;
};

std::array<std::byte, 8> encode(const Header&) noexcept;
Result<Header>           decode(std::span<const std::byte, 8>) noexcept;
```

Encoding: byte0 = `(res=0 << 5) | (version << 3) | op`, all multi-byte fields
big-endian (PN §2). Decoding validates:

* `res` bits (7..5) are zero → else `MalformedMessage`;
* `version ≤ 1` → else `UnsupportedSmpVersion` (0b10/0b11 are reserved);
* `op ≤ 3` → else `MalformedMessage`;
* `flags == 0` is **accepted but recorded** (forward compatibility: unknown
  flags must not break a client), non-zero flags are surfaced in `Header::flags`.

`length` is *not* validated here — bounds are the assembler's job, because only
it knows the configured limit.

`Group` is an enum class over `uint16_t` with named constants for the known
groups and an open range; unknown groups round-trip unchanged.

## 2. Streaming reassembly (`src/smp/assembler.*`)

```cpp
class MessageSink {                       // implemented by SmpClient (P6)
public:
    // payload is borrowed for the duration of this call only. Must not
    // re-enter the assembler.
    virtual void on_message(const Header&, ConstBytes payload) = 0;
};

struct AssemblerLimits {
    std::uint16_t max_payload = limits::kMaxSmpPayload;
    std::size_t   max_buffer  = limits::kMaxAssemblyBuffer;
};

class MessageAssembler {
public:
    explicit MessageAssembler(AssemblerLimits = {});
    Result<void> feed(ConstBytes input, MessageSink&);   // arbitrary chunks
    void         reset() noexcept;                       // on (re)connect
    std::size_t  buffered() const noexcept;
    std::size_t  peak_buffered() const noexcept;         // high-water mark
    std::size_t  capacity() const noexcept;              // for bound assertions
};
```

Algorithm, driven purely by the header length (PN §2). It has two paths, and
which one runs depends only on whether a partial message is already held:

**Fast path — nothing buffered.** Parse directly out of the caller's bytes.

1. Fewer than 8 bytes available ⇒ stash them and return.
2. Decode the header. On failure, `reset()` and return the error.
3. Validate the size (below). On failure, `reset()` and return the error.
4. Fewer than `total_size()` bytes available ⇒ stash them and return.
5. Emit `(header, payload)` as a view **into the caller's buffer**, advance past
   the message, and loop.

A whole message delivered in one call is therefore never copied at all — the
common case for BLE, where a notification frequently completes a response.

**Slow path — a partial message is held.** Top the buffer up from the input in
two stages: first to 8 bytes so the header can be decoded, then to
`total_size()`. If the input runs out at either stage, return and wait. When the
message completes, emit it from the buffer and `clear()` — never `erase()`, so
the capacity is retained and steady-state traffic performs no allocation.

**Size validation**, applied identically on both paths:

* `length > max_payload` ⇒ `MessageTooLarge`;
* `total_size() > max_buffer` ⇒ `MessageTooLarge`.

The second check happens **before** waiting for the remaining bytes, not after
they arrive. Checking afterwards would leave the stream stalled forever, waiting
for bytes that would be refused the moment they turned up.

### Why one message, not a sliding window

An earlier sketch used a single growing buffer with a read cursor, compacted
once the cursor passed the halfway mark. The implementation buffers **at most
one message** instead, which is both simpler and strictly better bounded:

* `buffered()` can never exceed `max_buffer`, because nothing is ever appended
  beyond the current message's `total_size()`, and that was validated first. A
  caller passing a megabyte in one `feed()` still buffers only the trailing
  partial message. With an append-then-parse design the whole megabyte would
  land in the buffer before anything checked it.
* There is no cursor, no compaction and no reallocation in steady state.
* The fast path is zero-copy, which the cursor design could not be.

`capacity()` is exposed so tests can assert the bound is real rather than merely
observed: a hostile peer must not be able to induce an allocation it will never
fill.

### Failure and lifecycle

* **Malformed framing is terminal for the stream.** SMP has no sentinel or
  sync word, so a stream whose framing is violated cannot be resynchronised —
  there is no way to find where the next message starts. The assembler discards
  its buffer and returns the error; the caller is expected to drop the
  connection. Messages successfully parsed *before* the bad one are still
  delivered.
* **`reset()`** discards any partial message. `SmpClient::rebind_transport()`
  calls it, so a truncated message cannot bleed across a reconnect.
* **Re-entrancy is refused.** A sink that calls `feed()` from inside
  `on_message()` would mutate the buffer its own payload points into. The
  assembler returns `InvalidState` rather than allowing the use-after-free.
* The object remains usable after an error — it is left empty, not poisoned.

Properties covered by tests, and by a fuzz target from P13:

* **The fragmentation invariant**: whole delivery, byte-at-a-time, every fixed
  fragment size 1–64, oversized fragments, and seeded random cut points all
  produce an identical sequence of messages.
* The buffer and its capacity never exceed `max_buffer`, under adversarial
  input including a declared length of `0xFFFF`.
* Unknown groups and unknown flags pass through untouched: the assembler holds
  no opinions the codec does not.

## 3. CBOR façade (`src/cbor/`)

Backend: **QCBOR** ([ADR-0007](decisions/ADR-0007-cbor-library.md)). It is named
in `src/cbor/cbor.hpp` and the two translation units beside it, and nowhere
else; no public header may mention it, and the API-discipline gate enforces
that.

```cpp
namespace smply::cbor {

class Writer {                       // encodes into a caller-owned buffer
public:
    explicit Writer(MutBytes out, unsigned max_nesting = limits::kMaxCborNesting);
    Writer& open_map();  Writer& close_map();
    Writer& put_uint (std::string_view key, std::uint64_t);
    Writer& put_int  (std::string_view key, std::int64_t);
    Writer& put_bool (std::string_view key, bool);
    Writer& put_text (std::string_view key, std::string_view);
    Writer& put_bytes(std::string_view key, ConstBytes);
    Result<ConstBytes> finish();     // sticky error surfaces here
    bool failed() const noexcept;
};

class Reader {                       // bounded, non-allocating, map-key based
public:
    explicit Reader(ConstBytes input, unsigned max_nesting = limits::kMaxCborNesting);
    Result<void> enter_map();                    // the top-level map
    Result<void> enter_map(std::string_view key);// a nested one
    Result<void> leave_map();

    std::optional<std::uint64_t>    uint   (std::string_view key);
    std::optional<std::int64_t>     integer(std::string_view key);
    std::optional<bool>             boolean(std::string_view key);
    std::optional<std::string_view> text   (std::string_view key);
    std::optional<ConstBytes>       bytes  (std::string_view key);

    Result<void> for_each_map_in_array(std::string_view key, std::size_t max_elements,
                                       const std::function<Result<void>(Reader&)>&);
    Result<void> status() const;     // first decode failure, if any
};
}
```

Design points:

* **Absent is not an error.** MCUmgr omits a field rather than sending false or
  zero (PN §6), so a missing key yields `std::nullopt` while a genuine decode
  failure sets a sticky status checked once at the end. Conflating the two would
  force every call site to handle an "error" that is really an absent optional
  field — and, worse, would let a wrong-typed field masquerade as a default.
  QCBOR distinguishes them for us: a lookup miss is `QCBOR_ERR_LABEL_NOT_FOUND`,
  which the façade resets; anything else is recorded.
* **Sticky, first-wins errors.** The first failure is kept, because later ones
  are usually its consequences. A poisoned `Reader` returns `nullopt` from every
  getter thereafter, so a caller that forgets `status()` gets defaults rather
  than garbage — but the group layer always checks it.
* **Named `put_*` rather than overloads.** CBOR distinguishes unsigned from
  negative integers on the wire, and MCUmgr fields have specific types; letting
  overload resolution pick could silently emit a different encoding than the
  protocol asks for.
* **Nothing allocates.** `Writer` encodes into the caller's buffer; `Reader`
  returns views into the caller's input, valid only while it lives. Group code
  copies what it keeps. A device cannot induce an allocation by claiming a size.
* **Arrays are visited, not collected.** `for_each_map_in_array` takes a hard
  element cap, so a device cannot make smply iterate — or make the caller
  accumulate — without bound. An absent array is an empty one, because MCUmgr
  omits `images` entirely when it has no valid image to report.
* **Keys are null-terminated behind the façade.** QCBOR's map API takes a C
  string; copying into a fixed buffer avoids assuming a `string_view` is
  terminated, which is the sort of assumption that works until one call site
  passes a substring. Keys longer than `kMaxKeyLength` (31) fail rather than
  truncate.
* **Nesting is bounded twice.** The façade counts the levels it enters, and
  QCBOR independently enforces its own compile-time `QCBOR_MAX_ARRAY_NESTING`
  of 15. Since that is below `limits::kMaxCborNesting`, QCBOR's bound is the one
  that fires first on hostile input — deep input fails either way, which is what
  matters.

`mgmt_error.*` implements the **dual** error extraction (PN §3), applied to every
response before any group-specific parsing. It handles all four shapes: an empty
or unrelated map (success), `rc: 0` (also success), a flat `rc` (SMP-level or v1
error), and `err: {group, rc}` (v2 group-scoped). Both flat and group-scoped are
always decoded whatever version was requested, because the specification requires
a v2 client to understand SMP-level errors reported as a flat `rc`. When both
appear the group-scoped one wins: it loses less information. The device's `rsn`
text is capped at `limits::kMaxReasonLength`, since it is attacker-controlled.

## 4. Request lifecycle (`src/smp/client.*`)

```cpp
class SmpClient final : private TransportListener {   // final, and not movable:
public:                                               // it registers its address
    SmpClient(Transport&, const Clock& = system_clock(), SmpClientConfig = {});
    ~SmpClient() override;            // completes outstanding requests inline

    RequestHandle request(const RequestSpec&, ResponseCallback);
    void cancel(RequestHandle);
    void poll(TimePoint now);                       // drives deadlines; runs callbacks
    std::optional<TimePoint> next_deadline() const; // nullopt: nothing outstanding
    void rebind_transport(Transport&);              // after reconnect; resets assembler

    bool connected() const;
    std::size_t in_flight() const;
    const SmpClientStats& stats() const;
    const SmpClientConfig& config() const;
};
```

`TransportListener` is inherited **privately**: only a transport calls those
three methods, and nothing above the client should be able to synthesise bytes.

**Lifetime.** Every transport a client has been bound to must outlive it. The
destructor detaches from the transport it holds, and `rebind_transport()`
detaches from the one it replaces; a transport destroyed first leaves those
calls dangling. Declaring the transport before the client is enough, and is what
the tests do. The transport contract states the converse — a listener outliving
its transport — but not this direction; see the P6 follow-up item in
[`roadmap.md`](roadmap.md).

The rule extends to callback captures. The destructor completes outstanding
requests, so a callback runs *during* destruction and everything it refers to
must still be alive then. Declaring captures before the client is enough. This
is easy to get wrong and hard to see: the resulting stack-use-after-scope is
reported by Clang's AddressSanitizer and **not** by GCC's, so the two sanitizer
jobs are not interchangeable.

### Callbacks never run inside the call that caused them

`request()` and `cancel()` are calls the *application* makes, and neither
invokes a callback before returning. A request that cannot even be attempted —
link down, table full, payload too large, transport refused it — returns an
invalid handle and its callback is queued for the next `poll()`. So
`handle = client.request(...)` always assigns before anything can observe the
result, which is what lets the upload and DFU machines store the handle they are
about to be told about.

The single exception is the destructor: there is no later `poll()` to defer to,
so it drains queued completions, fails everything still outstanding with
`Cancelled`, and drains again — all inline, before it returns. No callback can
fire after the client is gone.

Transport callbacks are the mirror image: `on_disconnected()` fails every
pending request *inline*, because the application did not ask for it and should
learn at once.

### Send path

Allocate `seq` → encode header + already-encoded CBOR payload into a reusable
send buffer → `transport.send(whole_message)`. Only on success does the request
enter the pending table with `deadline = now + timeout`. A failed send retires
the sequence number before deferring the failure: whether the transport put the
bytes on the wire before failing is not knowable, and a late answer should be
dropped quietly rather than counted as unsolicited.

The payload bound is a single comparison against `max_smp_payload`. A second
check against the largest encodable length would be dead code — `max_smp_payload`
is a `uint16_t`, so it cannot exceed the 16-bit length field — and a
`static_assert` says so where a future widening would trip over it.

### Sequence allocation

A monotonically incrementing `uint8_t`. The allocator skips numbers currently
pending **and** numbers in the retired set, and reports exhaustion rather than
reusing one. With `max_in_flight` and `max_retired_seqs` both bounded well below
256 that path is unreachable, but the allocator does not pretend otherwise.

### Receive path

`on_bytes()` → `MessageAssembler::feed()` → for each complete message:

1. Look up `seq` in the pending table. Not found → if it is in the retired set,
   count `late` and drop (PN §9 A4); otherwise count `unmatched` and drop.
   Never fatal (A5).
2. Verify `header.group`, `header.command` and `header.op ==
   response_to(request.op)`. On a mismatch the **message** is discarded with a
   `mismatched` bump and the request is left pending to time out normally.
   Completing it would let a stale or hostile message answer a live request with
   someone else's data. The spec does not define this case (A4); discarding is
   the only choice that cannot mis-complete a request.
3. Groups at 64 and above are user-defined and may carry any payload encoding,
   so they are handed up undecoded. For every other group, extract `MgmtError`
   (PN §3): a reported failure completes the request with
   `ErrorCode::ProtocolError`, the preserved `MgmtError`, and the optional
   length-capped `rsn` text.
4. Otherwise complete with the raw payload span; the group layer decodes it.

Completion always: remove from the table → retire the `seq` → invoke the
callback exactly once. Removal happens *before* the callback runs, so a callback
that issues a new request may reuse the slot safely.

Framing that cannot be parsed at all is terminal for the stream: SMP has no sync
word, so nothing after a bad length can be correlated. Every outstanding request
is failed and `malformed` is counted. The link itself stays open — the client
does not presume to close a transport it does not own.

### Timeouts

`poll(now)` drains queued completions, snapshots the expired handles, completes
each one that is still live, then drains again. Snapshotting matters: a timeout
callback may cancel or complete another expiring request, and the generation
check on re-resolution is what stops the sweep from completing it twice.

`next_deadline()` returns `std::nullopt` when nothing is outstanding, and
`TimePoint::min()` when a completion is already queued — the signal to an event
loop that it should poll rather than wait.

### Cancellation

`cancel(handle)` removes the request from the table immediately, so a response
arriving before the next `poll()` cannot complete it, and defers the `Cancelled`
callback. It does not try to abort the transport write; the device may still
answer, and that answer is dropped by the retired set. A stale or already
completed handle is a no-op — handles carry a generation counter, so a handle
cannot cancel a newer request that reused its slot.

### Disconnect and rebind

`on_disconnected()` fails every pending request with `Disconnected`, resets the
assembler and marks the client unbound; subsequent `request()` calls fail fast
until `rebind_transport()`. `on_transport_error()` completes nothing: it reports
a recoverable condition not tied to any one request, so an outstanding request
either still gets its answer or times out. It is recorded for diagnostics.

`rebind_transport()` fails anything still outstanding, detaches from the old
transport, resets the assembler so a message truncated by the drop cannot bleed
into the new session, and marks the client connected again.

### Counters

`SmpClientStats` records `sent`, `received`, `unmatched`, `late`, `mismatched`,
`timeouts`, `cancelled` and `malformed`. They exist so tests can assert that a
message was *dropped for a named reason* rather than silently mishandled — the
distinction that separates a deliberate discard from a bug.

## 5. Management groups

Thin, stateless-except-where-noted wrappers that own only encoding and decoding.
A group allocates no sequence numbers, sets no deadlines and interprets no `rc`:
`SmpClient` has already done all three by the time a response reaches it. If a
group looks like it needs to know about correlation, the seam is in the wrong
place.

```cpp
class OsManagement {                       // src/groups/os/
    RequestHandle reset(const ResetOptions&, Callback<void>);
    RequestHandle reset(Callback<void>);
    RequestHandle mcumgr_parameters(Callback<McumgrParameters>);
    RequestHandle echo(std::string_view, Callback<std::string>);
};

class ImageManagement {                    // src/groups/image/
    RequestHandle get_state(Callback<ImageState>);
    RequestHandle set_state(const SetStateRequest&, Callback<ImageState>);
    RequestHandle erase(const EraseOptions&, Callback<void>);
    RequestHandle erase(Callback<void>);
    RequestHandle get_slot_info(Callback<SlotInfo>);
    UploadHandle upload(ImageSource&, const UploadOptions&,   // see §6
                        std::function<void(UploadProgress)>, Callback<UploadResult>);
    void         resume(const UploadHandle&, Callback<UploadResult>);
    void         cancel(const UploadHandle&) noexcept;
};
```

Decoding rules applied uniformly (PN §6): absent boolean ⇒ `false`, and a
*present* `false` is just as ordinary — only a frugal-list build omits them;
absent `"image"` ⇒ `0`; array elements are bounded by `limits::kMaxImages` and
`limits::kMaxSlotsPerImage`; a `hash` outside 1…`limits::kMaxImageHashLength`
(64) bytes is rejected; `version` strings longer than
`limits::kMaxVersionStringLength` are rejected. `"slot"` and `"version"` are
required, because the specification does not mark them optional and the server
always writes both.

`ImageState`/`ImageSlot` are plain value structs with `std::optional` where the
protocol genuinely distinguishes absent from default (e.g. `hash`).

**Two hash types, on purpose.** `Hash` is a fixed 32-byte SHA-256 — the upload
`sha`, which smply computes over the whole file. `ImageHash` is what the device
*reports* for a slot: MCUboot's `IMAGE_TLV_SHA`, whose length is 32 or 64
depending on the bootloader (PN §6, §7). Two types rather than one means the
classic MCUmgr client bug — passing the file hash where the image hash belongs —
fails to compile instead of failing on hardware. `ImageHash::from(const Hash&)`
exists for the one legitimate crossing: comparing a hash read out of a file's
TLVs against what a device reports.

### Four rules every group follows

1. **Requests encode into a stack buffer**, sized from the constant that bounds
   the input rather than from what the caller passed. Nothing in the CBOR façade
   allocates, and a device cannot induce an allocation by claiming a size.
2. **`status()` is checked before any decoded field is trusted.** A `nullopt`
   getter means the field was absent, which is normal — MCUmgr omits rather than
   sending zero. A wrong-typed field poisons the reader and makes *every* field
   look absent, so skipping the check turns a malformed response into a
   successful one full of defaults.
3. **Views are copied before they escape the callback.** A decoded string points
   into the assembler's buffer, which is valid only for that call.
4. **A callback never runs inside the call that started the operation** — argument
   rejections included. `SmpClient::defer()` exists for exactly that case: a
   group that rejects an argument has no request to attach the failure to.

### Reset is acceptance, not completion

`OsManagement::reset()`'s callback fires when the device *accepted* the command.
Zephyr answers first and reboots afterwards by design, and the gap between the
two is implementation-defined (PN §5). Losing the response entirely is normal
too. Learning that the device actually restarted means waiting for a transport
disconnect, which is the DFU machine's job (§8), not this layer's.

`force` is sent as a CBOR **boolean** and omitted entirely when false. The
specification says integer; the server decodes a boolean and silently discards
anything else, so an integer would leave the client believing it had forced a
reset that was not forced (PN §9, A15).

### A group error code may not survive the trip

`ImageManagement` surfaces the image group's own codes through `image_error()`,
the exact counterpart of `smp_error()`. Both return `nullopt` rather than
guessing, because a v1 `rc` and a group-scoped `rc` are different numbering
spaces. What is specific to group 1 is how often the group detail is simply not
there: over SMP v1 — smply's default — a server built with
`CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL` translates the code onto
`mcumgr_err_t` and rebuilds the response, so `HashNotFound` reaches the client
as `SmpError::Unknown` (PN §9, A16). Callers check both accessors, and treat an
absent image code as normal rather than as a malformed reply.

## 6. Upload state machine (`src/groups/image/upload_session.*`)

The most intricate part of the library, and deliberately a **pure function** so
it can be exhaustively unit-tested with no client, transport or clock:

```cpp
struct UploadState {
    std::uint64_t confirmed_off = 0;   // server-acknowledged offset (authoritative)
    std::uint64_t in_flight_off = 0;   // what the outstanding request asked for,
    std::uint32_t in_flight_len = 0;   // so a timeout can repeat it exactly
    bool          in_flight_first_packet = false;
    std::uint32_t consecutive_no_progress = 0;
    std::uint32_t restarts = 0;
    std::uint32_t retries  = 0;
    bool          first_packet_pending = true;  // next request must be a full first packet
    Phase         phase = Phase::Idle;
};

enum class Action { SendChunk, Complete, Fail };
struct Step { Action action; UploadRequest request; Error error; std::optional<bool> match; };

Step plan_next  (const UploadState&, const UploadConfig&);          // what to send
void record_sent(UploadState&, const UploadRequest&);               // what went out
Step on_response(UploadState&, const UploadResponse&, const UploadConfig&);
```

**A restart is not its own action.** It is "set `first_packet_pending`, zero
`confirmed_off`, then send", and giving it a separate `Action` would let a
driver handle it and forget to send — a hang rather than an error. The chunk
size lives in `UploadConfig`, not in the state: it is negotiated once and never
changes.

`record_sent` exists because `plan_next` is `const`: the driver tells the state
what it actually put on the wire, which is what makes a byte-identical
retransmission possible.

### Chunk sizing

```
budget      = min(server_buf_size (OS params, PN §5) or default 256,
                  transport.max_message_size() when it has an opinion,
                  limits::kUploadChunkMax)
overhead    = 8 (SMP header) + cbor_overhead_first_packet(len, sha, image, upgrade)
chunk_size  = clamp(budget - overhead, 32, limits::kUploadChunkMax)
```

**`server_buf_size` is supplied by the caller**, in `UploadOptions`, not fetched
by the image group. It belongs to the OS group, and `SmpError::NotSupported`
from that command is a normal answer to fall back from (A8) rather than an
upload failure — keeping the fallback in one place is worth more than saving the
caller a line. A present-but-zero value is ignored like an absent one.

`cbor_overhead_first_packet` is computed exactly, by encoding a probe map with
the real `len`/`sha`/`image` values and a zero-length `data` bstr, then adding
the bstr header for `chunk_size`. Using the *first-packet* overhead for every
chunk wastes a handful of bytes on subsequent chunks and guarantees the first
one fits — a deliberate simplification. The floor of 32 enforces PN §6 rule 2
(first chunk must carry the 32-byte MCUboot header); if the computed size is
below 32 the upload fails immediately with `MessageTooLarge` rather than
looping.

### Request construction

* `off == 0` **or** `first_packet_pending` ⇒ include `len`, `sha` (when
  available), `image`, and `upgrade` (when requested) — PN §6 rule 7.
* otherwise ⇒ only `off` and `data`.
* `data` is `[off, off + chunk_size)` clipped to the image size, read through
  `ImageSource::read(off, span)`.

### Response handling — `on_response`

Let `rsp_off` be the server's `"off"` (PN §6 rule 5: **authoritative**).

| Condition | Action |
| --------- | ------ |
| protocol error `rc != 0` | `Fail` with the `MgmtError`. Exception: `EBUSY`/`ENOMEM` within the retry budget ⇒ re-send the *same* request after a backoff. |
| `"off"` absent on a success | `Fail(MalformedMessage)` — a success response must carry it. |
| `rsp_off > image_size` | `Fail(MalformedMessage)` — hostile/buggy device. |
| `rsp_off == image_size` | upload byte-complete → check `"match"` (below) → `Complete`. |
| `rsp_off == 0 && image_size > 0` | server restarted the session. `restarts++`; if over `max_restarts` ⇒ `Fail(UpdateFailed)`. Else set `confirmed_off = 0`, `first_packet_pending = true`, `SendChunk`. Also what a device that forgot the session answers, and what a **retransmitted final chunk** gets once the server has reset — where the first packet then completes the upload immediately via the already-present check (PN §6 rule 9a). |
| the request was a first packet | adopt `rsp_off` whatever it is, and do **not** charge the no-progress budget: adopting the device's answer is the entire point of sending a first packet. |
| `rsp_off > confirmed_off` | normal progress (may be **more** than we sent — accept it). `confirmed_off = rsp_off`; `consecutive_no_progress = 0`; `SendChunk`. |
| `rsp_off <= confirmed_off` | server rewound or repeated. `consecutive_no_progress++`; if over the budget ⇒ `Fail`. Else `confirmed_off = rsp_off`, set `first_packet_pending = (rsp_off == 0)`, `SendChunk` from `rsp_off`. |

`"match"`: absent ⇒ ignored (PN §9 A6, depends on `CONFIG_IMG_ENABLE_IMAGE_CHECK`);
`false` ⇒ `Fail(ImageMismatch)`; `true` ⇒ recorded on the result.

### Failures that are not responses

* **Timeout** — retry the same request up to `max_chunk_retries` (default 3).
  The `off` is unchanged, so a retransmission is always safe: either the server
  never saw it, or it saw it and will answer with the offset it actually has,
  which the table above handles.

  The **payload** is byte-identical; the SMP header is not, and must not be. The
  timeout retired the old sequence number, so a reply carrying it would be
  discarded as late (PN §4). A retransmission is a new request carrying the same
  bytes.
* **Disconnect** — `on_done` fires once with `Disconnected`, and the session is
  **kept**: `confirmed_off` and `sha` survive, so `ImageManagement::resume()`
  can send a first packet with the same `sha` once the application has rebound
  the transport. The server replies with its offset and the session continues
  from there (PN §6 rule 6); if the device forgot the session it answers
  `off == 0` and the restart path applies.

  Earlier drafts of this section said the session was "suspended" and no
  callback fired. That breaks the promise that completion happens exactly once —
  a caller who never resumes would wait forever — so the completion is reported
  and the *session*, not the callback, is what survives.

  **Adopting an offset from a first packet never counts against the no-progress
  budget.** A resume that correctly lands back on the offset it already had
  would otherwise look like a server that has stopped advancing.
* **Cancellation** — the session terminates; no cleanup command is sent (the
  device's stale session is harmless and is superseded by the next upload's
  `sha`).
* **A failure before the first request goes out** — an unreadable source, a
  budget too small for a chunk — is **deferred**, not reported inline. Rule 4 of
  §5 has no exception for the first chunk, and `upload()` returns an invalid
  handle in that case, exactly as `SmpClient::request()` does for a request it
  could not attempt.

Progress is reported as `{ confirmed_off, image_size }` on every confirmed
advance — never on send, so progress never moves backwards spuriously.

## 7. MCUboot image handling (`src/image/`)

Boundary rationale: [ADR-0009](decisions/ADR-0009-mcuboot-boundary.md).

Two public headers, because they are two different things: `image_source.hpp` is
an interface the application *implements*, and `mcuboot_image.hpp` is a set of
functions it *calls*. An application writing a custom source has no reason to
see the parsing API.

```cpp
// smply/image_source.hpp
class ImageSource {                       // application-provided
public:
    virtual std::uint64_t size() const noexcept = 0;
    virtual Result<std::size_t> read(std::uint64_t off, MutBytes out) = 0;
};
class MemoryImageSource;                  // provided; wraps a borrowed span

// smply/mcuboot_image.hpp
struct McubootImageInfo {                 // parsed from the first 32 bytes (PN §7)
    std::uint32_t header_size, image_size, protected_tlv_size, flags;
    ImageVersion  version;                // major.minor.revision.build
    bool          encrypted;              // IMAGE_F_ENCRYPTED_AES128|256
};
Result<McubootImageInfo>         parse_mcuboot_header(ConstBytes first32);
Result<Hash>                     sha256(ImageSource&);            // upload "sha"
Result<std::optional<ImageHash>> find_image_tlv_hash(ImageSource&,
                                                     const McubootImageInfo&);
```

`find_image_tlv_hash` returns an `ImageHash`, not a `Hash`, and accepts
`IMAGE_TLV_SHA256`, `SHA384` and `SHA512`: its whole purpose is to be compared
with `ImageSlot::hash` from the device, whose length depends on how the
bootloader was built (PN §6, §7). Returning the 32-byte type would make the
comparison need a conversion, which is exactly where the two hashes get
confused.

What smply **does**: validate the magic `0x96F3B83D` and header size, read the
version for reporting and for pre-flight comparison against the device, compute
the file's SHA-256 (streaming, 4 KiB at a time, no full-file buffering), and —
optionally — scan the TLV area for `IMAGE_TLV_SHA256` so the uploaded file can
be correlated with a device slot entry without trusting the device's word.

What smply **does not** do: verify signatures, decrypt, evaluate dependency
TLVs, or reimplement any swap logic.

TLV scanning follows MCUboot's own layout rules exactly (PN §7, from
`bootutil_tlv_iter_begin()` rather than from a diagram): the protected and
unprotected areas are contiguous and walked as one run, `ih_protect_tlv_size`
includes the protected area's own four-byte header and must equal it exactly,
and every offset is bounded against the file before a read.

It is defensive in the way that parsing attacker-supplied content demands, but
the reason for the iteration cap is worth stating precisely: **the scan
terminates without it.** Every advance is `4 + it_len`, so it is at least the
four-byte entry header and the offset strictly increases — a zero-length entry
cannot spin. `limits::kMaxImageTlvs` bounds the *work* a crafted file can
demand, not the loop.

Encrypted images (`IMAGE_F_ENCRYPTED_*`) are not scanned at all: the bytes on
the device are not the bytes in the file, so a correlation could never succeed
(PN §9 A13). That is `std::nullopt`, not an error.

SHA-256 is ~150 lines of FIPS 180-4 written for this project, pinned by the NIST
vectors, rather than a dependency on a crypto library (ADR-0009; see
[`dependencies.md`](dependencies.md)).

## 8. Firmware update state machine (`src/dfu/`)

Pure `(state, event) → (state, effects)` core (`update_state_machine.*`) driven
by `FirmwareUpdater`, which owns the effects (issuing requests, emitting
callbacks).

### States

```
                          ┌──────┐
                          │ Idle │
                          └───┬──┘  start()
                              ▼
                    ┌──────────────────┐
                    │ QueryingParams   │  OS mcumgr-params (optional; ENOTSUP ok)
                    └───────┬──────────┘
                            ▼
                    ┌──────────────────┐
                    │ InspectingImages │  IMG get-state  → learn active/pending/slots
                    └───────┬──────────┘
                            ▼
                    ┌──────────────────┐   already-running target image
                    │ Planning         ├──────────────────────────────► Completed
                    └───────┬──────────┘   already-uploaded ─► VerifyingUpload
                            ▼
                    ┌──────────────────┐◄── resume_after_reconnect()
              ┌────►│ Uploading        │
              │     └───────┬──────────┘
              │  disconnect │ byte-complete
              │     ┌───────▼──────────┐
              │     │ VerifyingUpload  │  IMG get-state → secondary slot hash present?
              │     └───────┬──────────┘
              │             ▼
              │     ┌──────────────────┐  IMG set-state{hash, confirm=false}
              │     │ MarkingForTest   │  (or confirm=true in ConfirmImmediately mode)
              │     └───────┬──────────┘
              │             ▼
              │     ┌──────────────────┐  OS reset
              │     │ Resetting        │
              │     └───────┬──────────┘
              │             ▼
              │     ┌──────────────────┐  emits DisconnectExpected
              │     │ AwaitingDisconnect│ ← link drop OR grace timeout
              │     └───────┬──────────┘
              │             ▼
              │     ┌──────────────────┐  emits ReconnectRequired; the APPLICATION
              └─────┤ AwaitingReconnect│  reconnects and calls resume_after_reconnect()
                    └───────┬──────────┘
                            ▼
                    ┌──────────────────┐  IMG get-state → active slot must carry
                    │ VerifyingBooted  │  the target hash
                    └───────┬──────────┘
                            ▼
                    ┌──────────────────┐  emits ConfirmationRequired; the
                    │ AwaitingConfirmation│ APPLICATION validates and calls
                    └───────┬──────────┘  confirm() (skipped when the mode is
                            │             ConfirmImmediately)
                            ▼
                    ┌──────────────────┐  IMG set-state{confirm=true}
                    │ Confirming       │
                    └───────┬──────────┘
                            ▼
                    ┌──────────────────┐  IMG get-state → confirmed == true
                    │ VerifyingConfirm │
                    └───────┬──────────┘
                            ▼
                    ┌──────────────────┐        ┌──────────┐     ┌───────────┐
                    │ Completed        │        │ Failed   │     │ Cancelled │
                    └──────────────────┘        └──────────┘     └───────────┘
```

`Failed` and `Cancelled` are reachable from every non-terminal state.
`RolledBack` is a distinguished `Failed` reason detected in `VerifyingBooted`
when the active image is the *old* one and no pending image remains — MCUboot
performed a `REVERT` (PN §7).

### Modes

* `UpdateMode::TestThenConfirm` (**default**) — the flow above, stopping at
  `AwaitingConfirmation` so the application can validate the running image and
  call `confirm()`. Safe twice over: a device that fails to boot never reaches
  the window, and one the application refuses to confirm reverts on the next
  reset.
* `UpdateMode::ConfirmImmediately` — the identical sequence, confirmed without
  asking ([ADR-0014](decisions/ADR-0014-confirmation-is-the-applications-call.md)).
  What an unattended updater wants. It is **not** a permanent swap up front:
  P11 established that a confirm on any slot that is not the running one is
  refused with `IMAGE_CONFIRMATION_DENIED` unless the build sets
  `CONFIG_MCUMGR_GRP_IMG_ALLOW_CONFIRM_NON_ACTIVE_SLOT`
  ([`protocol-notes.md`](protocol-notes.md) §7), so that flow cannot be built.
* `UpdateMode::UploadOnly` — stops after `VerifyingUpload`; the application
  decides when to activate.

### Application-facing events

`FirmwareUpdater` never touches a connection. It communicates intent through a
single event stream:

| Event | Meaning | Application must |
| ----- | ------- | ---------------- |
| `Progress{sent, total}` | upload advanced | update UI |
| `ConfirmationRequired` | the new image is running, unconfirmed | validate it, then `confirm()` — or `cancel()` and let it revert |
| `StateChanged{from,to}` | any transition | update UI |
| `DisconnectExpected` | reset accepted; the link is about to drop | stop treating a drop as an error |
| `ReconnectRequired{hint_delay}` | reconnect now | re-establish the link, `rebind_transport()`, then `resume_after_reconnect()` |
| `Finished{Result<UpdateReport>}` | terminal | release resources |

### Failure and recovery per state

| State | Failure | Recovery |
| ----- | ------- | -------- |
| `QueryingParams` | `ENOTSUP` / timeout | **not fatal** — fall back to defaults (PN §9 A8) |
| `InspectingImages` | any error | fatal; nothing has been changed on the device |
| `Uploading` | timeout | chunk retry (design §6) |
| `Uploading` | disconnect | suspend; `ReconnectRequired`; resume via `sha` (PN §6 rule 6) |
| `Uploading` | server `off == 0` | restart from the first packet, bounded by `max_restarts` |
| `VerifyingUpload` | target hash absent from any slot | fatal `ImageMismatch` — the device did not store what we sent |
| `MarkingForTest` | `IMAGE_ALREADY_PENDING` | re-read state **once**; the planner then sees our own image already marked and steps straight to `Resetting` |
| `MarkingForTest` | `IMAGE_SETTING_TEST_TO_ACTIVE_DENIED` | fatal, with a clear diagnostic |
| `Resetting` | `EBUSY` | one retry with `force = 1` (PN §5) |
| `Resetting` | no response but the link drops, or the request times out | **treated as success** (PN §9 A3): the device may reset before its answer goes out, and the verify after the reboot is the real check |
| `AwaitingDisconnect` | grace timeout with the link still up | proceed to `AwaitingReconnect` anyway; the verify step is the real check |
| `AwaitingReconnect` | application reports failure | fatal, but the device is in a *pending* state — the report says so |
| `VerifyingBooted` | active image is the old one | `RolledBack` |
| `VerifyingBooted` | active image is ours, `confirmed == true` already | skip `Confirming` |
| `AwaitingConfirmation` | the application cancels, or never confirms | terminal; the device reverts on its next reset — `UpdateReport::revert_pending` says so |
| `Confirming` | `IMAGE_CONFIRMATION_DENIED` | fatal; the device will revert on the next reset — the report says so |

Every terminal outcome yields an `UpdateReport` recording the final device
image state, the number of bytes transferred, restart/retry counts, and, on
failure, the state it failed in plus the underlying `Error`.

## 9. Transport contract

Normative contract; full signatures in [`api.md`](api.md). Rationale in
[ADR-0005](decisions/ADR-0005-transport-abstraction.md).

| Question | Answer |
| -------- | ------ |
| What is one outbound unit? | **Exactly one complete SMP message** (8-byte header + `length` payload bytes). Fragmenting it is the transport's job. |
| Does `send()` block? | No. It returns once the message is accepted for transmission. |
| Backpressure? | `send()` may return `ErrorCode::TransportBusy`. The core does not queue; with `max_in_flight = 1` there is at most one message outstanding, so a busy transport simply fails that request. |
| How is inbound data delivered? | `TransportListener::on_bytes(span)` with **arbitrary** chunk boundaries. The core reassembles (ADR-0006). |
| Ordering? | The transport **must** preserve byte order. GATT and UART both do. |
| Buffer lifetime? | Borrowed for the duration of the call, in both directions. A transport that defers a send must copy. |
| Concurrency? | Single-threaded: all calls in and out happen on the client context (architecture §5). |
| Cancellation? | The core never cancels an in-flight write. `close()` stops all callbacks before returning. |
| Failure reporting? | Recoverable/one-off ⇒ `on_transport_error(Error)`; link is gone ⇒ `on_disconnected(Error)`. After `on_disconnected` no further callbacks may be issued. |
| Size hint? | `max_message_size()` — the largest whole SMP message this transport can carry. `0` means "unknown"; the core then uses its configured default. |

## 10. WinRT BLE adapter design (`transports/winrt_ble/`)

Windows-only target `smply::winrt_ble`. Consumes `smply::smply`; no WinRT type
appears in any header under `include/smply/`, and the core builds with the
target absent (enforced by CI, [`quality-gates.md`](quality-gates.md)).

### Mapping

| smply concept | WinRT operation |
| ------------- | --------------- |
| open | `BluetoothLEDevice::FromBluetoothAddressAsync` → `GetGattServicesForUuidAsync(SMP_SERVICE)` (PN §8) → `GetCharacteristicsForUuidAsync(SMP_CHAR)` |
| enable notifications | `WriteClientCharacteristicConfigurationDescriptorAsync(Notify)` + subscribe `ValueChanged` |
| `send(message)` | split into `mtu − 3` fragments; each fragment `WriteValueWithResultAsync(buf, GattWriteOption::WriteWithoutResponse)` (PN §8) |
| fragment size | `GattSession::MaxPduSize − 3`; clamp to `[20, 512]`; re-read on `MaxPduSizeChanged` |
| `max_message_size()` | a configured cap (default 1024) — *not* the MTU; a whole SMP message may span many fragments |
| inbound | `ValueChanged` → copy the `IBuffer` → post to `Dispatcher` → `on_bytes()` on the client context |
| disconnect | `ConnectionStatusChanged == Disconnected` → post → `on_disconnected()` |
| errors | `GattCommunicationStatus != Success`, `hresult_error` → `on_transport_error` / `on_disconnected` |

### Threading, lifetime, shutdown

* WinRT raises `ValueChanged` and `ConnectionStatusChanged` on **thread-pool
  threads**. The adapter never calls the core from them; every inbound event is
  copied into a `smply::Dispatcher` queue and replayed on the client context
  (architecture §5).
* Event tokens are held in `winrt::event_revoker`s so revocation is exception-safe
  and happens before the owning object is destroyed.
* `close()` is the **only** safe shutdown path: revoke tokens → wait for
  in-flight async operations to observe a cancellation flag → drain the
  dispatcher queue → mark closed. It is synchronous and idempotent, and after it
  returns no callback can fire. The destructor calls it.
* `winrt::apartment_context`/`resume_background` are used inside the adapter
  only; no coroutine crosses the core boundary.
* Write-without-response has no flow control at the GATT level. The adapter
  paces fragments using `GattCharacteristic::WriteValueWithResultAsync`'s
  completion, and surfaces `TransportBusy` if the stack rejects a write.

The example `examples/winrt_ble_dfu/` is a console application: scan by name or
address → connect → build `SmpClient` + `FirmwareUpdater` → run a simple pump
loop (`poll()` + `Dispatcher::drain()`), print progress, handle
`ReconnectRequired` by reconnecting and calling `resume_after_reconnect()`.

## 11. Robustness rules (checklist for reviewers)

* Every length from the device is compared against a configured bound *before*
  it is used to size, index or allocate.
* All arithmetic on offsets and lengths uses `std::uint64_t` with explicit
  overflow checks (`off + len < off` and `off + len > size`).
* No `reinterpret_cast` over device data; the MCUboot header is decoded field by
  field from a byte span, never by casting to a struct.
* No owning raw pointers; no `new`/`delete`; no C-style casts.
* Every `switch` over an internal enum is exhaustive with no `default`, so adding
  a state is a compile error at every decision point.
* Public entry points validate their arguments and return `InvalidArgument`
  rather than asserting.
