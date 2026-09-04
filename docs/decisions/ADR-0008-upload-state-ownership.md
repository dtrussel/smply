# ADR-0008 — Upload state as a pure function, owned by `ImageManagement`

**Status:** Accepted (2026-09-04)

## Context

Image upload is the most complex operation in the library. It must survive
server-corrected offsets, a server-requested restart from zero, a server that
rewinds or repeats, timeouts, retransmission, disconnection and resumption, all
while reporting progress and honouring cancellation — and the server-returned
offset is authoritative, never `previous + sent`
([`../protocol-notes.md`](../protocol-notes.md) §6). If this logic is spread
across the DFU orchestrator and the request callbacks, it becomes untestable and
subtly wrong.

## Decision

Two separations.

**1. The decision logic is a pure function.** `upload_session.*` exposes

```cpp
Step plan_next  (const UploadState&, const UploadConfig&);
Step on_response(UploadState&, const UploadResponse&, const UploadConfig&);
```

with `Step = {Action (SendChunk|Complete|Fail|Restart), UploadRequest, Error}`.
No client, no transport, no clock, no I/O. Every rule in the response table in
[`../design.md`](../design.md) §6 is therefore a table-driven unit test.

**2. The state lives in `ImageManagement`, not in `FirmwareUpdater`.**
An upload is a *group-level* operation that is meaningful on its own (the
`UploadOnly` mode, and any caller who just wants to push an image).
`FirmwareUpdater` drives it and owns the surrounding policy — when to upload,
what to do afterwards — but does not own the offset, the session hash or the
retry counters.

The firmware bytes themselves are owned by the **application**, behind
`ImageSource`; the library never buffers a whole image.

## Alternatives considered

**State inside `FirmwareUpdater`.** Would make `ImageManagement::upload()`
impossible to use standalone and would entangle chunk-level retry policy with
reset/reconnect policy — two failure domains with different recovery rules.
Rejected.

**State inside `SmpClient`.** Would push image semantics into the protocol
layer, breaking the layering that the whole architecture rests on. Rejected
immediately.

**A stateful `UploadSession` object with methods that also send.** The obvious
object-oriented shape, and it is what the *public* `UploadHandle` looks like.
But making the decision logic a method that performs I/O means every test needs
a client and a transport. Keeping the decisions pure and the I/O in a thin
driver costs one indirection and buys exhaustive testability. Chosen: the pure
function *is* the implementation; `UploadHandle` is its public face.

**Deriving the next offset arithmetically** (`off += sent`) with the server's
`off` used only as a sanity check. This is the classic MCUmgr client bug: the
server may legitimately return a *different* offset, including a larger one or
zero, and a client that assumes arithmetic silently corrupts the flashed image
or loops forever. Rejected as protocol-incorrect; the response table treats
`rsp.off` as the only source of truth.

## Consequences

* The densest unit-test suite in the project is a pure table
  ([`../testing.md`](../testing.md) §3) with a ≥ 90 % branch-coverage gate.
* Resumption after a reconnect is expressible as "re-send the first packet with
  the same `sha` and adopt whatever offset comes back" — one code path shared
  with the restart case.
* Progress is derived from `confirmed_off` only, so it can never move backwards
  spuriously or overstate what the device has stored.
* `ImageManagement` gains the one piece of mutable state in the group layer;
  everything else there stays stateless. Documented in
  [`../architecture.md`](../architecture.md) §6.
