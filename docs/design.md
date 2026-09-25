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
namespace smply::smp {                    // internal: not in include/smply/

class MessageSink {                       // implemented by SmpClient
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

} // namespace smply::smp
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

Properties covered by tests, and by `fuzz_assembler`, which asserts the buffer
bound at every step and at the peak:

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
* **…and each element is decoded by a child reader over its own bytes.** Not by
  entering the element and leaving it with `ExitMap()`, which is the obvious
  shape and is wrong here. QCBOR — pinned 1.6.1 and `master` alike — mishandles
  two consecutive indefinite-length breaks, which is what an indefinite-length
  map at the end of an indefinite-length array produces: it swallows the array's
  break as well, and then either fails with `QCBOR_ERR_BAD_BREAK` or, worse,
  silently reads the *parent map's* following entries as further array elements.
  That is not a hypothetical encoding — Zephyr's zcbor emits it unless
  `CONFIG_ZCBOR_CANONICAL` is set, and nothing in MCUmgr sets it, so it is what
  a real device sends (PN §9 A18; `dependencies.md` has the minimal
  reproduction). `for_each_map_in_array`
  therefore peeks, bounds each element's byte range with `QCBORDecode_Tell`
  around `QCBORDecode_VGetNextConsume`, and hands that range to a child
  `Reader` — which is why `Reader` keeps its own `input_` span. **Do not tidy it
  back into enter/exit**, and build any new response golden in *both* encodings:
  `test_cbor.cpp` pins both shapes, and its `[hardware-golden]` cases carry a
  device's exact bytes.
* **Keys are null-terminated behind the façade.** QCBOR's map API takes a C
  string; copying into a fixed buffer avoids assuming a `string_view` is
  terminated, which is the sort of assumption that works until one call site
  passes a substring. Keys longer than `kMaxKeyLength` (31) fail rather than
  truncate.
* **Nesting is bounded twice, and smply's bound must be the one that binds.**
  The façade counts the levels it enters against `limits::kMaxCborNesting`, and
  QCBOR independently enforces its compile-time `QCBOR_MAX_ARRAY_NESTING` of
  15. `kMaxCborNesting` is **14**, strictly below QCBOR's, and the gap is
  load-bearing rather than cautious: "deep input fails either way" is not true.
  When QCBOR refuses first the refusal arrives through
  `Reader::enter_map(key)`, whose QCBOR-error path is deliberately *not* sticky
  so that it can double as a probe for the optional `err` map (below). So with a
  cap at or above QCBOR's, an over-deep document would make the reader stop
  descending with `status()` **clean**: silently missing fields rather than a
  decode failure, invisible to a caller following the house rule of checking
  `status()` at the end. Fourteen
  and not fifteen, because reaching smply's cap needs a document one level
  deeper than the cap — equal is not enough. `limits.hpp` carries the same
  reasoning at the constant.

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
its transport — but not this direction; the roadmap's backlog has an item for
moving both obligations into the contract.

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

A group command is always the same five steps: encode a request, build a
`RequestSpec`, send it, and on the answer either pass the failure on or decode
the payload. `src/groups/common.hpp` holds those steps once (`groups::send()`,
`reject()`, `enter_response()`), so a new group is its command enumeration,
its encoders and its decoders. The rules below are what those encoders and
decoders must still get right themselves.

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
it can be exhaustively unit-tested with no client, transport or clock. Like
every internal type it lives in a namespace named after its component,
`smply::upload` (`architecture.md` §3):

```cpp
namespace smply::upload {

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

} // namespace smply::upload
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
| `rsp_off == image_size` | upload byte-complete → check `"match"` (below) → `Complete`. **If the request was a first packet, this is the server's own already-present check (PN §6 rule 9a), not a transfer that finished**, and the two are indistinguishable from `off` alone — both report the whole image. `Step::completed_on_first_packet` records which, and surfaces as `UploadResult::already_present` — **but only if the session had acknowledged nothing yet** (`UploadState::progressed`). A first packet re-sent after a rule-9b `off == 0` completes the same way, and the image the server then "already holds" is the one this session transferred (PN §9 A19, seen on every hardware update before the final chunk had its own deadline). |
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

  **The deadline is not one number.** `upload_driver.cpp` picks one of three
  per request, because two chunks in an upload are answered by a device doing
  far more than storing bytes:

  | The request | Deadline | Why |
  | ----------- | -------- | --- |
  | a first packet | `first_chunk_timeout` (30 s) | the server erases the slot synchronously before answering (PN §9 A7; 6.6 s measured) |
  | `off + length == image_size` | `final_chunk_timeout` (30 s) | with `CONFIG_IMG_ENABLE_IMAGE_CHECK` the server hashes the **whole image** out of flash before encoding the response (A19; ~25 KiB/s, 5.0 to 5.6 s for 134 160 bytes) |
  | anything else | `chunk_timeout` (5 s) | an ordinary store-and-acknowledge |

  A first packet that is *also* the last chunk takes the first-chunk deadline;
  the erase dominates.

  `final_chunk_timeout` exists because of what happens without it, and the
  failure is worth knowing because it does not look like one: the last chunk
  times out, the retransmission was answered `off == 0` (rule 9b — the
  server has already reset the session), the re-sent first packet completes
  immediately via the already-present check (rule 9a), and the **update
  succeeds while reporting the transfer as skipped**. A green run hiding a
  timeout (PN §9 A19). The companion fix is `UploadState::progressed`, which is why
  `already_present` now means "this session moved nothing" rather than "the
  server answered on a first packet".

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
* **`TransportBusy`** — treated as a hard failure of the upload, and
  `is_transient()` (`src/groups/image/upload_session.cpp`) deliberately **does
  not** list it. That looks wrong for something the contract calls a retry
  request, so the reason is recorded rather than left to be re-derived: nothing
  below `FirmwareUpdater` owns a clock. `SmpClient::request()` defers a send
  error and `deliver_deferred()` drains in a loop, so a retry decided here would
  spend all of `max_chunk_retries` inside a single `poll()` with **zero elapsed
  time** — a busy-wait dressed as a retry, which cannot help a medium that needs
  a moment. The right fix for a transport that is genuinely behind is a
  clock-driven backoff, which touches ADR-0003 and ADR-0004 and so needs an ADR
  of its own. Until then the transport absorbs the handover window itself
  (§10), and a `TransportBusy` that still reaches here means the link stalled
  and failing is the honest answer.
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
  a confirm on any slot that is not the running one is refused with `IMAGE_CONFIRMATION_DENIED` unless the build sets
  `CONFIG_MCUMGR_GRP_IMG_ALLOW_CONFIRM_NON_ACTIVE_SLOT`
  ([`protocol-notes.md`](protocol-notes.md) §7), so that flow cannot be built.
* `UpdateMode::UploadOnly` — stops after `VerifyingUpload`; the application
  decides when to activate.

### Application-facing events

`FirmwareUpdater` never touches a connection. It communicates intent through a
single event stream. Each event is one alternative of the `UpdateEvent`
variant, so a handler reads only the fields its kind has, and a `std::visit`
that forgets a kind does not compile:

| Alternative | Meaning | Application must |
| ----------- | ------- | ---------------- |
| `UploadProgress{transferred, total}` | the device confirmed an advance | update UI |
| `ConfirmationRequired` | the new image is running, unconfirmed | validate it, then `confirm()` — or `cancel()` and let it revert |
| `UpdateStateChanged{from, to}` | any transition | update UI |
| `DisconnectExpected` | reset accepted; the link is about to drop | stop treating a drop as an error |
| `ReconnectRequired{hint}` | reconnect now | re-establish the link, `rebind_transport()`, then `resume_after_reconnect()` |
| `UpdateFinished{Result<UpdateReport>}` | terminal; nothing follows | release resources |

### Failure and recovery per state

| State | Failure | Recovery |
| ----- | ------- | -------- |
| `QueryingParams` | `ENOTSUP` / timeout | **not fatal** — fall back to defaults (PN §9 A8) |
| `InspectingImages` | any error | fatal; nothing has been changed on the device |
| `Uploading` | timeout | chunk retry (design §6) |
| `Uploading` | disconnect | suspend; `ReconnectRequired`; resume via `sha` (PN §6 rule 6) |
| `Uploading` | server `off == 0` | restart from the first packet, bounded by `max_restarts` |
| `VerifyingUpload` | target hash absent from any slot | fatal `ImageMismatch` — the device did not store what we sent |
| `MarkingForTest` | `IMAGE_ALREADY_PENDING`, **or a group-less `EBADSTATE`** | re-read state **once**; the planner then sees our own image already marked and steps straight to `Resetting`, or re-marks it if the refusal was not ours after all. The second shape is not a second rule: a v1 server that translates group codes sends `NO_FREE_SLOT`, `CURRENT_VERSION_IS_NEWER` and `IMAGE_ALREADY_PENDING` all as a flat `EBADSTATE` (PN §9 A16, A24), so without it the recovery cannot fire at all on the commonest configuration. Deliberately not extended to `EUNKNOWN`, which the same table gives to every flash failure |
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
image state, the number of bytes transferred, and, on failure, the state it
failed in plus the underlying `Error`. (The restart and retry counts live inside
the upload and are not plumbed out; the roadmap's backlog has the item.)

**`upload_skipped` has two sources, and both matter.** The updater's own
pre-flight check skips a transfer when the slot table it just read already shows
the target hash. The *server* runs the same check independently, on any first
packet carrying a full `sha`, and can answer "complete" before any image data is
written (§6, rule 9a) — which happens whenever `skip_if_already_present` is off,
or when a reconnect makes the client resend a first packet. The report takes
both into account, the second through `UploadResult::already_present`, so
neither is reported as a transfer of the whole image.

## 9. Transport contract

Normative contract; full signatures in [`api.md`](api.md). Rationale in
[ADR-0005](decisions/ADR-0005-transport-abstraction.md).

| Question | Answer |
| -------- | ------ |
| What is one outbound unit? | **Exactly one complete SMP message** (8-byte header + `length` payload bytes). Fragmenting it is the transport's job. |
| Does `send()` block? | No. It returns once the message is accepted for transmission. |
| Backpressure? | `send()` may return `ErrorCode::TransportBusy`, which is a **request to retry**, not a link failure. The core still does not queue: with `max_in_flight = 1` there is at most one message outstanding, and the request it belongs to fails. What the bench falsified is the *inference* that a transport with a write in progress must therefore refuse — the device's answer can arrive before the local write's own completion runs, so the medium is free while a naive "a write is in progress" flag still says busy. Admitting a second message is the **transport's** decision and is transport-internal (§10); `TransportBusy` now means the medium is genuinely behind rather than merely mid-handover. |
| How is inbound data delivered? | `TransportListener::on_bytes(span)` with **arbitrary** chunk boundaries. The core reassembles (ADR-0006). |
| Ordering? | The transport **must** preserve byte order. GATT and UART both do. |
| Buffer lifetime? | Borrowed for the duration of the call, in both directions. A transport that defers a send must copy. |
| Concurrency? | Single-threaded: all calls in and out happen on the client context (architecture §5). |
| May `send()` answer inline? | **No.** It must not invoke any `TransportListener` method before it returns: that re-enters reassembly mid-write, which `MessageAssembler` refuses (§2). An in-process transport queues the answer and delivers it from the application's next turn. |
| Who outlives whom? | **The transport outlives every client bound to it.** `~SmpClient` and `rebind_transport()` both detach by calling `set_listener(nullptr)`, so a transport destroyed first leaves those calls dangling. |
| Cancellation? | The core never cancels an in-flight write. `close()` stops all callbacks before returning. |
| Failure reporting? | Recoverable/one-off ⇒ `on_transport_error(Error)`; link is gone ⇒ `on_disconnected(Error)`. After `on_disconnected` no further callbacks may be issued. |
| Size hint? | `max_message_size()` — the largest whole SMP message this transport can carry. `0` means "unknown"; the core then uses its configured default. |

### The adapter's marshalling obligation, and `smply::Dispatcher`

The last row of that table is the one that costs adapter authors time. The core
has exactly one client context and no lock (ADR-0004), so a driver that raises
its callbacks on its own thread — WinRT's thread pool, a serial reader thread —
must hand the bytes across before touching `TransportListener`. smply ships the
helper rather than leaving each adapter to invent it.

```cpp
smply::Dispatcher inbound{[&] { wake_the_pump(); }};

// driver thread
inbound.post([this, bytes = std::vector<std::byte>{data, data + size}] {
    listener_->on_bytes(smply::ConstBytes{bytes});   // now on the client context
});

// pump thread
inbound.drain();
client.poll(now);
```

Note the copy. Inbound buffers are borrowed for the duration of the transport
callback (the table above), so anything crossing a thread boundary has to own
its bytes.

`Dispatcher` lives in its own target, `smply::util`; `libsmply` does not link
it. Its contract is in [`api.md`](api.md), and three parts of it are load-bearing
for an adapter:

* **one `drain()` is one turn.** The queue is taken before anything runs, so
  what a closure posts waits for the next drain and cannot starve
  `SmpClient::poll()` — which is where every deadline lives.
* **`on_wake` runs on the posting thread**, inside `post()`, without the
  dispatcher's lock. Signal something; do not do work, and do not block on the
  client context. An adapter that took the client's lock there would have built
  a lock-order inversion.
* **`clear()` discards.** On teardown the queued closures name a link that is
  going away, and running them is worse than dropping them.

**Misuse is caught in debug builds.** `SMPLY_ASSERT_CLIENT_THREAD()` fires if a
`TransportListener` callback, or any `SmpClient` entry point, is reached from a
thread other than the one that constructed the client — which is exactly the
mistake an adapter makes when it forgets to marshal. It compiles out under
`NDEBUG`; see `src/detail/client_thread.hpp` for why it lives in `SmpClient`
alone.

## 10. WinRT BLE adapter design (`transports/winrt_ble/`)

Windows-only target `smply::winrt_ble`. Consumes `smply::smply`; no WinRT type
appears in any header under `include/smply/`, and the core builds with the
target absent (enforced by CI, [`quality-gates.md`](quality-gates.md)).

### Mapping

| smply concept | WinRT operation |
| ------------- | --------------- |
| open | `WinRtBleTransport::connect()`: `BluetoothLEDevice::FromBluetoothAddressAsync` → `GetGattServicesForUuidAsync(SMP_SERVICE)` (PN §8) → `GetCharacteristicsForUuidAsync(SMP_CHAR)`, the last two **retried while either collection comes back empty** (see below). The UUIDs come from `transports/common/smp_ble_uuid.hpp`, whose bytes and endian split are unit-tested on every platform |
| enable notifications | `WriteClientCharacteristicConfigurationDescriptorAsync(Notify)` + subscribe `ValueChanged` |
| `send(message)` | admitted by `transports/common/send_queue.hpp` (one writer, one message waiting, a third refused as `TransportBusy`), then split into `mtu − 3` fragments; each fragment `WriteValueWithResultAsync(buf, GattWriteOption::WriteWithoutResponse)` (PN §8) |
| `close()` | mark closing → stop accepting events → revoke tokens → **discard the waiting message** → wait up to five seconds for the fragment on the air → close session and device |
| fragment size | `GattSession::MaxPduSize − 3`, clamped to `[20, 512]` by `transports/common`'s `fragment_size()`; read once per `send()` (see below) |
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
* `close()` is the **only** safe shutdown path: mark the link closing → stop
  accepting events → revoke tokens → wait for the in-flight write to observe a
  cancellation flag → close the session and device → mark closed. It is
  synchronous and idempotent (`LinkState::begin_close()` is what makes the
  second call a no-op), and after it returns no callback can fire. The
  destructor calls it.

  **It does not drain or clear the dispatcher, and must not.** An earlier
  version of this section said "drain the dispatcher queue", which assumes the
  adapter owns it. It does not: the `Dispatcher` belongs to the application and
  may carry several transports' work at once — `examples/cli_dfu/main.cpp` runs
  every link it opens through one — so clearing it would discard another
  transport's callbacks, and draining it from inside `close()` would run
  arbitrary application closures at the worst possible moment. Instead each
  posted closure captures a strong reference to the adapter's state and asks
  `LinkState::may_deliver()` before touching the listener; a closure that
  outlives the link keeps its state alive, finds the link closed and returns
  having done nothing. The guarantee is unchanged, and it no longer depends on
  owning something the application owns.

* **The event handlers hold a weak reference** to that state. A strong one would
  be a cycle — the state owns the revoker, which owns the handler — and the
  state would never be destroyed.

* A handler running on a pool thread must not read `LinkState`, which belongs to
  the client context. It consults an atomic `accepting` flag instead, cleared
  *before* the revokers run, so correctness does not depend on whether
  `revoke()` waits for a handler that is already executing.
* `winrt::apartment_context`/`resume_background` are used inside the adapter
  only; no coroutine crosses the core boundary.
* Write-without-response has no flow control at the GATT level. The adapter
  paces fragments by awaiting each `GattCharacteristic::WriteValueWithResultAsync`
  before starting the next, in one detached coroutine per **writer run** —
  `send()` may not block (§9), so the writes outlive the call that started them.
  A writer run is not a message: the coroutine loops on
  `SendQueue::next_for_writer()` and exits only when the queue is empty
  (`transports/winrt_ble/winrt_ble_transport.cpp`), so one run may carry several
  messages. The difference between "per message" and "per run" is exactly the
  invariant below.

  Two consequences follow, and they are easy to state the wrong way round.
  `TransportBusy` means **two messages are already outbound** — one being
  written and one waiting (below). The core keeps one request in flight
  (ADR-0010), so it is a request to retry, not a broken link. A *write* the
  stack rejects is discovered after `send()` has already returned, so it cannot
  be its return value — it arrives later as `on_transport_error()`, or
  `on_disconnected()` when the status says the device is unreachable, posted
  through the same dispatcher as inbound bytes and never delivered inline.

* **Send admission: one writer, one waiting message**
  (`transports/common/send_queue.hpp`). A single "a write is in progress" flag
  is the obvious design, and the bench showed it is wrong (PN §9, A22). The flag can only
  be cleared by the write's own completion, and the **device's answer can
  arrive first**: the response travels device → radio → OS → a pool thread →
  the client, while the local write's continuation waits for a thread of its
  own. The core, having been answered, offers the next chunk and is refused
  although the previous message is complete in every sense that matters. A
  sequential run of the hardware suite died on it (PN §9, A22).

  So `SendQueue` separates "a writer is running" from "no further message may be
  accepted": one message may **wait** while another is written, and only a
  third is refused. Its invariant is that the writer's claim is taken by
  `offer()` and released *only* by `next_for_writer()` finding nothing, so a
  writer never exits with a message waiting and a second writer is never
  started. That last part is load-bearing rather than tidy: two writers would
  let two messages' fragments interleave, and reassembly rests entirely on them
  not doing so (ADR-0006, PN §8) — with no framing to resynchronise on, the
  damage would be a mis-framed message rather than an error anyone reports.
  Exactly one writer exists at any instant, it submits fragments strictly
  sequentially, it takes the next message only after the previous message's last
  `co_await` resumed, and GATT preserves submission order. ADR-0006's premise is
  therefore upheld, not bent, and `TransportBusy` stays reachable, so the
  contract in §9 needs no change.

  The bookkeeping lives in `transports/common/` rather than in the adapter
  because that directory is unit-tested, linted and coverage-measured on every
  platform while this one is checked by MSVC and a bench; the same move
  `ble_framing.hpp` and `link_state.hpp` made. What a unit test *cannot* check
  is the adapter's use of it — that every call is made under `send_mutex`, that
  the mutex is never held across a suspension point, and that a writer is
  started exactly on `Admission::StartWriter`.

  **What the bench then discharged, and what it did not.** Three sequential
  suites passed with `deferred_sends` non-zero on three to six cases per run and
  `refused_sends` zero throughout, so the handover path was exercised rather
  than avoided. The first and third properties are also *reviewable statically*
  and were reviewed: every `SendQueue` call in the adapter is made under
  `send_mutex`, none is made across a `co_await`, and the writer is started at
  the single `Admission::StartWriter` site in `send()`. The second — that the
  mutex is never held across a suspension point — is the one a bench run cannot
  prove, because holding it would manifest as `send()` blocking under a load
  pattern that may simply not have occurred; it rests on the code having one
  lock scope that closes before the coroutine starts. Treat it as reviewed, not
  as measured.

  The genuinely open residual is elsewhere: whether the *device's* reassembler
  discards a partial message on a timeout of its own, which is what makes the
  discard rule above sufficient rather than merely usually sufficient. That is
  unverified from this side of the link and is filed as such.

  **A failure discards the waiting message, deliberately.** A message abandoned
  mid-fragment leaves the device's length-driven reassembler holding a partial
  message; writing the next one would let its bytes be consumed as the abandoned
  tail — the same mis-framing as interleaving, from the other direction. The
  discarded message's request then simply times out, which is the well-covered
  retry path. For the same reason `close()` **discards rather than flushes**, so
  its five-second write grace still covers one message's remaining fragments and
  did not need widening.

  Removing this admission gate would contradict ADR-0006; moving the retry into
  the core would contradict ADR-0003 and ADR-0004 (§6). Neither is a change a
  reader should make without an ADR.

* **Service discovery is retried while it comes back empty** — a fix whose
  error path is proven and whose *benefit* is not, and it should be read that
  way. The behaviour it exists for did not reproduce in any of five full runs,
  and an instrumented build recorded discovery succeeding on the first attempt
  in 20 of 20 measured discoveries; forcing the condition proves the loop runs
  its six attempts and reports the right one of two messages, which is
  reachability, not benefit (PN §9, A22). It rests on one bench observation.
  After a *rapid*
  reconnect Windows answers with a service whose characteristic collection is
  **empty** for a second or two — its own service cache, even though every
  query here asks for `BluetoothCacheMode::Uncached` (PN §9, A22). Concluding
  "no SMP characteristic" from that fails a reconnect that would have succeeded
  a moment later, which on this platform is a reconnect a real DFU tool depends
  on. `connect()` therefore re-runs **service** discovery, not just the
  characteristic query: the stale object is the service, so re-asking it for
  characteristics would likely return the same empty list. Six attempts 400 ms
  apart, closing the stale service between them, bounds the extra wait at about
  two seconds. Only the *empty* outcomes are retried — a non-`Success` status
  means unreachable or denied, and retrying that just makes a doomed reconnect
  slower — and the two final verdicts stay distinct: no service at all, or a
  service with no SMP characteristic.

* **Every `connect()` failure tears the half-built link down** by calling the
  same `close()` sequence before returning. It used to drop the state instead,
  leaving the device, the service and a `GattSession` with
  `MaintainConnection(true)` open, and on the notification path handlers
  subscribed and never revoked. During a reconnect storm that is a plausible
  contributor to the staleness above. With no writer started the shutdown's
  condvar wait returns immediately.

* **`MaxPduSize` is read once per message, not cached.** Keeping a cached copy
  fresh would need a `MaxPduSizeChanged` subscription, and so another handler
  and another revoker in the shutdown sequence above, to avoid a stale fragment
  size. One property read per message is cheaper than that, and cannot go
  stale.

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

## 12. Serial (MCUmgr console) framing (`transports/serial/`)

*Numbered 12 and placed after §11 deliberately: ten files across the tree cite
"design.md section 11" for the robustness checklist above, and renumbering it
would churn all of them to move a heading.*

Header-only and portable, shipped by `smply::transport_common` alongside
`transports/common/`. The decision, and in particular why this module has a
receiver when `common/ble_framing.hpp` deliberately has none, is
[ADR-0017](decisions/ADR-0017-serial-framing-placement.md). The wire format is
[`protocol-notes.md`](protocol-notes.md) §8, read out of Zephyr's
`serial_util.c`.

**This is not a transport.** Nothing here opens a port. An application supplies
the file descriptor, the `termios`/`CreateFile` setup, the reader thread and
the `smply::Dispatcher` that marshals inbound bytes onto the client context
(§9), and implements `Transport` over them. What it does not have to supply is
the protocol.

### The three pieces

| Type | Direction | Holds |
| ---- | --------- | ----- |
| `SerialFramer` | outbound | Borrows one SMP message; writes one frame per call into a caller-owned buffer of `kMaxFrame` bytes. Nothing allocates. |
| `LineSplitter` | inbound | Turns arbitrary reads into whole lines. Owns one bounded buffer. |
| `SerialDeframer` | inbound | Turns lines into whole SMP packets. Owns one bounded buffer. |

Split three ways rather than offered as one pipeline because each is separately
testable with no callback, no template and no virtual — and because an adapter
that already has a line-oriented reader can use the deframer alone.

```cpp
// outbound, inside Transport::send()
std::array<std::byte, smply::transport::kMaxFrame> frame{};
smply::transport::SerialFramer framer{message};
while (!framer.done()) {
    write_all(fd_, frame.data(), framer.next_frame(frame));
}

// inbound, on the reader thread
smply::ConstBytes rest{buffer, n};
while (const auto line = splitter_.next_line(rest)) {
    if (deframer_.feed_line(*line) == SerialDeframer::Outcome::Packet) {
        const smply::ConstBytes packet = deframer_.packet();
        inbound_.post([this, copy = std::vector<std::byte>{packet.begin(), packet.end()}] {
            listener_->on_bytes(smply::ConstBytes{copy});
        });
    }
}
```

Note the copy, and note *where* it is. `packet()` is borrowed until the next
`feed_line()`, which is the same borrowed-buffer rule as everywhere else (§9),
and anything crossing a thread boundary has to own its bytes.

### The four outcomes

`feed_line()` answers one of four things, and the difference between the last
two is the interesting part.

| Outcome | Meaning | State |
| ------- | ------- | ----- |
| `NeedMore` | a frame was accepted | packet in progress |
| `Packet` | complete and CRC-verified | available from `packet()` until the next call |
| `Ignored` | not a frame at all | **partial packet kept** |
| `Error` | framing violated | partial packet discarded |

`Ignored` exists because a Zephyr console is shared. With
`CONFIG_SHELL_BACKEND_SERIAL` and `CONFIG_LOG_BACKEND_UART` the same stream
carries prompts, command echo and log lines, interleaved with frames at
arbitrary points — and that is the exact stream over which a third-party client
could not complete an upload on the bench (PN §9). The server's own receiver ignores
such a line without touching its context, and so does this. An ignored line is
never *decoded*, so nothing a device puts in one reaches the CRC, the length or
the buffer.

### Three bounds, and where each fires

Everything below comes off a wire a device controls, so each of the three has a
test named for it.

1. **A line longer than `kMaxFrame` is dropped, not buffered.** No frame can
   exceed it, so an over-long line is by definition somebody else's output —
   and without this a peer that never sends a newline grows `LineSplitter`'s
   buffer without limit.
2. **The declared length is checked against `max_packet` before the buffer is
   allowed past one frame's worth.** The device supplies a big-endian length in
   the first frame; a hostile one is refused while at most 93 bytes have been
   accumulated, so the peak footprint is bounded by the configured cap and never
   by what was asked for. A declared length of two or less — a CRC and no
   packet — is refused with it.
3. **A body longer than its own declared length is an error, not a trim.** The
   server refuses the same case, and for the same reason: extra bytes on the end
   mean the stream has lost sync, not that the packet has rubbish after it.

`buffered()`, `peak_buffered()` and `capacity()` are exposed so a test can
assert the second directly — the property worth guaranteeing is that the buffer
never *grew*, not that it was emptied afterwards. `fuzz_serial_deframe` asserts
all three at every step over an arbitrary stream cut at a fuzzer-chosen size.

### Frame boundaries match the reference exactly

`SerialFramer` reproduces `mcumgr_serial_tx_pkt()` byte for byte, including its
least obvious behaviour: rather than let the CRC straddle two frames it defers a
data byte to the next one, so a 183- or 184-byte packet yields a 123-byte
second frame where a naive 93-bytes-per-frame packing would yield 127.

A conforming receiver accepts either, so this is a deliberate choice to be
bug-compatible in the harmless direction: any device that tolerates Zephyr's own
transmitter tolerates smply's. `tests/unit/test_serial_framing.cpp` carries a
transcription of the C for exactly this, and requires byte-identical output for
every packet size from 1 to 300.

### Size limits

A serial `Transport::max_message_size()` has a real ceiling and it is **not**
93. The ceiling is `kMaxSerialPacket`, 65533, because the frame's length field
is two bytes and carries `packet.size() + 2` — and a maximal SMP message is
*above* it, since its own header `length` is 16-bit as well (8 + 65535 =
65543). `SerialFramer` refuses anything larger rather than truncating the
field, which would put a length on the wire contradicting the bytes after it,
and which no receiver could diagnose.

93 is the *frame* limit, and it feeds nothing above the transport: a whole SMP message
spans as many frames as it needs, exactly as a BLE message spans GATT packets.
What the adapter reports is whatever whole-message budget it is willing to
carry, and the upload chunk arithmetic in
[`protocol-notes.md`](protocol-notes.md) §8 takes the minimum of that, the
device's `buf_size` and the configured cap. Conflating the two would cap
uploads at a chunk size the protocol never asked for.

### Open

Whether a device reset drops a serial link is unsettled — a hardware UART stays
open across one, a USB CDC port disappears and returns. `FirmwareUpdater`'s
`AwaitingDisconnect` / `AwaitingReconnect` states and
`UpdatePlan::disconnect_grace` assume a link that drops. Roadmap **O7**; it
cannot be decided without a port adapter.
