# ADR-0006 — SMP message reassembly lives in the core

**Status:** Accepted (2026-09-04)

## Context

A BLE notification carries at most `ATT_MTU − 3` bytes, so SMP responses arrive
in fragments with no framing of their own — *"the SMP header in the first
fragment contains sufficient information for reassembly"*
([`../protocol-notes.md`](../protocol-notes.md) §8). Somebody must turn that
byte stream back into messages: either every transport, or the core once.

## Decision

**The core reassembles.** `MessageAssembler` (`src/smp/assembler.*`) consumes
arbitrary byte chunks from `TransportListener::on_bytes()` and emits complete
`(header, payload)` messages, using only the 8-byte header's `length` field.
Transports do no SMP-level work whatsoever.

The assembler enforces the defensive bounds (`max_smp_payload`,
`max_assembly_bytes`), is `reset()` on connect/disconnect, and is one of the
primary fuzz targets.

## Alternatives considered

**Each transport reassembles and delivers whole messages.** Superficially
tidier — `on_message(header, payload)` is a nicer callback than `on_bytes()`.
But it duplicates identical, security-sensitive parsing in every adapter;
each duplicate is a fresh chance to get a length check wrong on untrusted
input; each adapter would need its own bounds configuration; and the fuzzers
could only reach the copy inside whichever adapter they were built against. The
one legitimate argument for it — a transport that *already* has message
boundaries, like BLE-with-perfect-fragments — does not survive contact with
reality, because BLE fragments are not messages either. Rejected.

**Split it: transports with native framing deliver messages, stream transports
deliver bytes.** Two inbound paths, two sets of tests, and the "does this
transport preserve boundaries?" question leaks into the core anyway. Rejected as
complexity with no payoff; a boundary-preserving transport simply calls
`on_bytes()` once per message, which the assembler handles as the trivial case.

## Consequences

* Exactly one implementation of the most security-sensitive parsing in the
  library, fuzzed and bounds-checked in one place.
* Adapters get simpler: the WinRT adapter copies the notification buffer and
  forwards it — no state, no partial-message handling.
* Consistent limits and error reporting regardless of transport.
* The assembler must handle every fragmentation pattern, which the
  "arbitrary fragmentation invariant" test enforces
  ([`../testing.md`](../testing.md) §3).
* `reset()` must be called on every (re)connect. `SmpClient::rebind_transport()`
  does it, so adapters cannot forget.
