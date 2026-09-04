# ADR-0005 — Abstract `Transport`: whole message out, byte stream in

**Status:** Accepted (2026-09-04)

## Context

The core must be transport-agnostic and must never own GATT services, MTU
negotiation, characteristics, WinRT objects or connection management. A starting
sketch was considered:

```cpp
using SendCallback = std::function<Result(std::span<const std::byte>)>;
class SmpClient {
    explicit SmpClient(SendCallback send);
    void receive(std::span<const std::byte> bytes);
};
```

This is close to right, and its asymmetry is the tell: sending is one function,
but a real transport also needs to report link loss, report a recoverable write
failure, advertise its size limit, and be shut down deterministically. Those
four things have nowhere to live in a bare callback.

## Decision

A pair of abstract interfaces (full text in [`../api.md`](../api.md)):

```cpp
class Transport {                          // implemented by adapters
    virtual Result<void> send(ConstBytes message) = 0;   // ONE whole SMP message
    virtual std::size_t  max_message_size() const noexcept = 0;
    virtual void         set_listener(TransportListener*) noexcept = 0;
    virtual void         close() noexcept = 0;
};
class TransportListener {                  // implemented by SmpClient
    virtual void on_bytes(ConstBytes) = 0;              // ARBITRARY chunks
    virtual void on_transport_error(Error) = 0;
    virtual void on_disconnected(Error) = 0;
};
```

The normative contract — one outbound unit, ordering, buffer lifetime,
concurrency, backpressure, cancellation, failure reporting, size hints — is
specified in [`../design.md`](../design.md) §9. Its two load-bearing clauses:

* **Outbound: `send()` receives exactly one complete SMP message.** Splitting it
  into GATT writes or UART frames is the transport's job. The core never sees a
  fragment.
* **Inbound: `on_bytes()` accepts arbitrary chunks.** The transport does no
  SMP-level work. The core reassembles from the header length
  ([ADR-0006](ADR-0006-reassembly-location.md)).

Backpressure is expressed as `ErrorCode::TransportBusy` from `send()`; with one
request in flight ([ADR-0010](ADR-0010-request-correlation.md)) the core never
needs an outbound queue. `max_message_size()` is the whole-SMP-message capacity
— explicitly **not** the MTU — and feeds upload chunk sizing together with the
device's `buf_size`.

## Alternatives considered

**Callbacks (`std::function`) for send + a `receive()` method** — the sketch
above. Simple and testable, but leaves no place for disconnect notification,
size hints or deterministic shutdown; those would become extra constructor
callbacks, at which point it is an interface with worse ownership clarity.
Rejected.

**Concepts/templates (`template <Transport T> class SmpClient`).** Zero virtual
dispatch and compile-time checking. Costs: the client type becomes generic, so
it cannot be stored behind a stable ABI, cannot be swapped at runtime (fatal —
`rebind_transport()` after a reconnect is a core requirement), and pushes the
whole implementation into headers, which slows every build and complicates the
fuzzers. Virtual dispatch happens once per SMP message; it is irrelevant next to
a BLE round trip. Rejected.

**A `std::function`-based struct of callbacks** (send + error + disconnect).
Equivalent power to the interface, with fuzzier ownership and no natural place
for `close()`. Rejected on clarity, not capability.

**Transport delivers complete SMP messages** (transport does reassembly). See
[ADR-0006](ADR-0006-reassembly-location.md) — rejected there.

## Consequences

* Every transport implements four small methods; `FakeTransport` is ~80 lines
  and gives tests total control over fragmentation and faults.
* `SmpClient` holds a non-owning reference to its transport and is non-movable
  (its address is registered as the listener).
* Transport fragmentation, the server's SMP buffer size, and the MCUmgr upload
  chunk size stay three distinct, separately owned concepts
  ([`../protocol-notes.md`](../protocol-notes.md) §8).
* A future UART transport does its own base64/CRC framing and still satisfies
  this contract unchanged, which was the design's acceptance test.
* Adapters must copy any buffer they do not consume synchronously, in both
  directions. Stated in the contract and checked by ASan in the fake-transport
  tests.
