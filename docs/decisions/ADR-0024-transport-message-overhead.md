# ADR-0024 — A transport reports what the device's buffer spends beyond the message

**Status:** Accepted (2026-10-07). Amends [ADR-0005](ADR-0005-transport-abstraction.md):
adds one member to `Transport`. ADR-0005's other decisions stand.

## Context

The device reports one receive-buffer size, `buf_size` (`mcumgr params`, the
netbuf size `CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE`; protocol-notes §8, S33).
smply sizes an upload's messages so that each fits it:

```
budget = min(buf_size, transport.max_message_size())   // upload_session.cpp
chunk  = min(budget − SMP header − request overhead, kUploadChunkMax)
```

That is right over BLE, where the netbuf holds the SMP message alone. It is
four bytes too large over serial. The serial transport decodes each frame into
the netbuf *before* stripping its framing, so the netbuf holds the 2-byte
length prefix and the 2-byte CRC as well as the message (protocol-notes §8,
S27, S34; §9 A25). A message between `buf_size − 4` and `buf_size` bytes is
dropped by the device with no response, and the request times out.

The serial adapter cannot fix this alone. It knows its framing costs four
bytes, but it never sees `buf_size`: that arrives in an OS-group response the
core decodes, or from the caller. The adapter's own default cap, 256 bytes,
keeps a default-configured device safe (netbuf 384 → 380 usable). A device
whose `buf_size` is at or below the adapter's cap is not safe
(roadmap backlog: "before claiming serial support for devices with a small
`buf_size`"). Serial now runs whole updates on a real device (the BL54L15
bench), so the claim is close.

## Decision

1. **`Transport` gains one member**:

   ```cpp
   /// Bytes the device's receive buffer spends on each message beyond the
   /// SMP message itself: framing it buffers before removing. 0 by default,
   /// which is right when the device's buffer holds the SMP message alone.
   [[nodiscard]] virtual std::size_t message_overhead() const noexcept { return 0; }
   ```

   It is a fact about the medium's framing as the *device* receives it, not
   about the host's buffers or the wire: base64 expansion, frame markers and
   line breaks do not count, because the device does not keep them.

2. **The core subtracts it from the device's buffer, and only there.**
   `compute_chunk_size()` takes `min(buf_size − overhead, max_message_size())`.
   The adapter's `max_message_size()` is already a limit on the SMP message,
   so it is not reduced again. When `buf_size` is unknown, the conservative
   default (`kDefaultSmpMessageBudget`, which stands in for a device's buffer)
   is reduced the same way. A budget that the overhead leaves too small fails
   with `MessageTooLarge`, as an undersized `buf_size` does today.

3. **`SmpClient::transport_message_overhead()`** forwards it, beside
   `transport_max_message_size()`, so it follows `rebind_transport()`.

4. **`SerialPortTransport` returns 4.** The BLE adapter, the loopback and the
   test fake keep 0 (the fake can be set).

5. **An explicit `UploadOptions::chunk_size` still bypasses the budget.** A
   caller that sizes its own chunks owns the arithmetic, as now.

6. **Defaulted, not pure virtual.** This is the first defaulted virtual in a
   public smply interface. A pure virtual would break every out-of-tree adapter
   at compile time for a number that is 0 for all of them except a serial one.
   A default of 0 keeps their behaviour exactly as it is today.

## Alternatives considered

**Fold it into `max_message_size()`.** The serial adapter would report
`cap − 4`. Wrong quantity: the adapter's cap is not the device's buffer, and the
adapter cannot see `buf_size`. A device with `buf_size` ≤ cap stays broken.
Rejected.

**A caller-supplied option** (`UploadOptions::transport_overhead`). It works,
but the four bytes are a property of the transport, not of the application.
Every serial application would have to know to set it, and forgetting fails
silently, with a timeout on one size of message. Rejected.

**Subtract a fixed 4 when the transport "is serial".** The core does not know,
and must not know, what its transport is (ADR-0005). Rejected.

**Pure virtual.** It makes every adapter state the number explicitly, which is
cleaner in principle. It breaks out-of-tree adapters for no behavioural gain,
since 0 is correct for any medium that delivers the message unframed. Rejected
under 0.x as well, because a defaulted member costs nothing.

## Consequences

* The installed, public `smply/transport.hpp` changes. That is a minor bump
  under 0.x ([ADR-0016](ADR-0016-installed-package-and-versioning.md)
  clause 5), source-compatible for existing adapters, and recorded in the
  CHANGELOG. `api.md` and `design.md` §9 (the normative contract) gain the
  member.
* ADR-0005's consequence "every transport implements four small methods" now
  reads "four, and may override a fifth".
* Upload sizing over serial is correct for any `buf_size`, which removes the
  A25 caveat from protocol-notes, `architecture.md`, `design.md` §13 and
  `api.md`.
* The three size limits of protocol-notes §8 stay three. The overhead is not a
  fourth limit: it is a correction applied to one of them.
* The evidence is a hardware case: a peer built with a small netbuf, a message
  sized to `buf_size` that the device drops (the negative control), and the
  default sizing that completes.
