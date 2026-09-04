# smply

A small, transport-agnostic C++ client library for the Zephyr **MCUmgr / SMP**
protocol, focused on **MCUboot firmware update (DFU)**.

* **Portable core** — no Windows, WinRT, Qt, BLE or GUI dependency. Builds and is
  fully testable on Linux, macOS and Windows without any radio hardware.
* **Sans-IO** — the core never opens a socket, never starts a thread and never
  looks at a real clock. The application supplies the transport, the clock and
  the thread that drives it.
* **Layered** — SMP framing, CBOR, MCUmgr management groups and DFU
  orchestration are separate, individually testable layers.
* **Example adapter** — a Windows 11 C++/WinRT BLE GATT transport ships as a
  *separate* target and never leaks into the core's public headers.

## Status

**Planning / design complete. No implementation yet.**

The repository currently contains the architecture, protocol analysis, public
API proposal and a phased implementation roadmap. Implementation starts at
roadmap phase `P0`.

## Start here

| If you are…                        | Read                                       |
| ---------------------------------- | ------------------------------------------ |
| a coding agent picking up the work | [`docs/handoff.md`](docs/handoff.md) → [`docs/roadmap.md`](docs/roadmap.md) |
| reviewing the design               | [`docs/architecture.md`](docs/architecture.md) |
| implementing a protocol detail     | [`docs/protocol-notes.md`](docs/protocol-notes.md) |
| looking for the API                | [`docs/api.md`](docs/api.md)               |
| wondering *why* something is so    | [`docs/decisions/`](docs/decisions/)       |

## Documentation index

* [`docs/architecture.md`](docs/architecture.md) — components, dependency
  direction, threading, async model, error model, trust boundaries.
* [`docs/design.md`](docs/design.md) — detailed design: SMP codec, reassembly,
  request tracking, upload state machine, DFU state machine.
* [`docs/protocol-notes.md`](docs/protocol-notes.md) — authoritative spec
  inventory, verified wire details, ambiguities and version dependencies.
* [`docs/api.md`](docs/api.md) — proposed public C++ headers.
* [`docs/testing.md`](docs/testing.md) — unit, component, fuzz and HIL strategy.
* [`docs/quality-gates.md`](docs/quality-gates.md) — CI matrix, warnings,
  static analysis, coverage, sanitizers, Definition of Done.
* [`docs/security.md`](docs/security.md) — threat model and trust boundaries.
* [`docs/dependencies.md`](docs/dependencies.md) — dependency inventory and
  licensing.
* [`docs/roadmap.md`](docs/roadmap.md) — phased, status-tracked execution plan.
* [`docs/handoff.md`](docs/handoff.md) — agent session handoff protocol and log.
* [`docs/decisions/`](docs/decisions/) — Architecture Decision Records.

## Licence

To be decided in P0 (intent: permissive, proprietary-friendly — see
[`docs/dependencies.md`](docs/dependencies.md)).
