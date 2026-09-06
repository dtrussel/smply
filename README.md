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

**Phases P0–P13 complete — the portable library does what it exists to do, and
its untrusted-input surface is fuzzed.**
SMP framing and streaming reassembly, a bounded CBOR façade, request
correlation with timeouts and cancellation, the OS and image management groups,
MCUboot image parsing with SHA-256, the image upload state machine, and
`FirmwareUpdater`: the whole update, including the reset and the reconnect.
Seven libFuzzer targets over every decoder that reads bytes it did not write,
with the coverage thresholds and a fuzz smoke run now blocking.
590 tests, 12 CI jobs green plus a nightly soak.

What is not built yet: the `Dispatcher` helper, the WinRT BLE transport and the
example applications. See
[`docs/roadmap.md`](docs/roadmap.md) for the phase-by-phase plan and what is
next.

A whole update, with the application owning the pump and the connection:

```cpp
MyTransport            transport{/* ... */};
smply::SmpClient       client{transport};
smply::ImageManagement image{client};
smply::OsManagement    os{client};
smply::FirmwareUpdater updater{client, image, os};
smply::MemoryImageSource source{firmware_bytes};

updater.start(source, {}, [&](const smply::UpdateEvent& event) {
    // Two things the library deliberately will not do for you: reconnect after
    // the device resets, and decide the new image is good. See docs/api.md.
});

while (busy) {                       // the application owns the pump
    const auto now = std::chrono::steady_clock::now();
    client.poll(now);
    updater.poll(now);
}
```

## Building

Requires **CMake >= 3.25** and a C++20 compiler (GCC 11+, Clang 14+, MSVC
19.30+). C is also required: QCBOR, the CBOR backend, is a C library.

```sh
cmake --preset linux-clang          # or linux-gcc, windows-msvc
cmake --build --preset linux-clang
ctest --preset linux-clang
```

Dependencies (QCBOR, Catch2) are fetched automatically and pinned to exact
commits. Pass `-DSMPLY_USE_SYSTEM_QCBOR=ON` to use a system or vcpkg copy
instead.

Every CI configuration has a matching preset, so a CI failure reproduces
locally with one command. `cmake --list-presets` shows them all.

### Checks

```sh
tools/format.sh              # reformat; --check to verify only
tools/lint.sh                # clang-tidy (+ cppcheck when installed)
tools/coverage.sh            # coverage report; --enforce applies the thresholds
python3 tools/check_public_headers.py   # no platform/third-party types in public headers
python3 tools/check_deps.py             # dependencies declared and pinned by hash
python3 tools/check_docs.py             # documentation gate (ADR-0013)
tools/verify_gates.sh        # proves each gate rejects a deliberate violation
```

`tools/verify_gates.sh` works on a throwaway copy of the tree and never
modifies the working tree.

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
* [`docs/api.md`](docs/api.md) — the public C++ headers, each marked
  shipped or proposed.
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

**Apache-2.0** — see [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE). Permissive and
suitable for linking into proprietary applications, with an explicit patent
grant. Third-party licences are inventoried in
[`docs/dependencies.md`](docs/dependencies.md).
