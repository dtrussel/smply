# ADR-0004 — Single client context, no internal threads

**Status:** Accepted (2026-09-04)

## Context

BLE stacks deliver notifications on their own threads; WinRT raises
`ValueChanged` and `ConnectionStatusChanged` on thread-pool threads. A serial
transport would use a reader thread. Meanwhile a GUI application wants results
on a thread it controls. Somebody has to marshal, and where that happens
determines how complex the library's internals are.

## Decision

**There is exactly one client context: the thread that calls `poll()`.**

* `SmpClient`, `OsManagement`, `ImageManagement` and `FirmwareUpdater` are **not
  thread-safe** and must only be used from that thread.
* All callbacks out of the library are invoked on that thread.
* `Transport::send()` is called on that thread.
* **`TransportListener::on_bytes()` / `on_transport_error()` /
  `on_disconnected()` must also be invoked on that thread.** Marshalling from a
  driver thread is the **adapter's** responsibility.
* The core contains no mutex, no atomic protocol state, no condition variable
  and no thread.
* To make the adapter's job trivial, smply ships `smply::Dispatcher` — a small
  thread-safe queue of closures with an optional wake callback — in a separate
  utility target. The WinRT adapter uses it; the core does not depend on it.

## Alternatives considered

**Internal locking (thread-safe core).** Lets a transport call in from any
thread. Costs a lock on every protocol operation, makes callback re-entrancy
rules subtle (does the lock hold across the user's callback?), makes deadlocks
possible in user code, and makes tests non-deterministic. It also solves a
problem the adapter is better placed to solve, since only the adapter knows
which thread the application wants. Rejected.

**Core-owned dispatch thread.** The library would own a thread and marshal
everything itself. Contradicts [ADR-0003](ADR-0003-async-model.md), adds a
lifetime hazard (shutdown ordering), and hides concurrency from the application
that must ultimately reason about it. Rejected.

**"Caller provides synchronization", unspecified.** The path of least
documentation and the most field bugs. Rejected: threading guarantees must be
explicit, per the requirements.

## Consequences

* The core has, structurally, no data races — TSan is therefore not part of the
  standard gate ([`../quality-gates.md`](../quality-gates.md) §7); it is applied
  to `Dispatcher` specifically.
* Adapter authors carry one clearly documented obligation, with a ready-made
  helper. The transport contract in [`../design.md`](../design.md) §9 states it
  normatively.
* Misuse (calling from two threads) is undefined behaviour rather than a
  detected error. Mitigation: debug builds assert on the owning thread id via a
  `SMPLY_ASSERT_CLIENT_THREAD()` check compiled out in release.
* Callback re-entrancy remains possible (a callback starting a new request) and
  is handled explicitly in the pending-request table
  ([`../design.md`](../design.md) §4), not by locking.
