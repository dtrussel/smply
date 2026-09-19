# smply

A small, transport-agnostic C++ client library for the Zephyr **MCUmgr / SMP**
protocol, focused on **MCUboot firmware update (DFU)**.

* **Portable core** — no Windows, WinRT, Qt, BLE or GUI dependency. Builds and is
  fully testable without any radio hardware. CI builds and runs the whole suite
  on **Linux and Windows**; nothing in the core is platform-specific, but macOS
  has no CI job and so is untried rather than supported.
* **Sans-IO** — the core never opens a socket, never starts a thread and never
  looks at a real clock. The application supplies the transport, the clock and
  the thread that drives it.
* **Layered** — SMP framing, CBOR, MCUmgr management groups and DFU
  orchestration are separate, individually testable layers.
* **Example adapter** — a Windows 11 C++/WinRT BLE GATT transport ships as a
  *separate* target and never leaks into the core's public headers.

## Status

**Phases P0–P17 complete — the library is done, it has met a real device, and
it is packaged: it does what it exists to do, its untrusted-input surface is
fuzzed, adapter authors have both the marshalling helper the threading model
assumes and the BLE helpers a correct adapter needs, and there is a runnable
example that performs a whole update.**
SMP framing and streaming reassembly, a bounded CBOR façade, request
correlation with timeouts and cancellation, the OS and image management groups,
MCUboot image parsing with SHA-256, the image upload state machine, and
`FirmwareUpdater`: the whole update, including the reset and the reconnect.
Seven libFuzzer targets over every decoder that reads bytes it did not write,
with the coverage thresholds and a fuzz smoke run now blocking. And
`smply::Dispatcher`: the thread-marshalling helper every transport adapter
needs, shipped as a separate target the core does not link. And
`examples/cli_dfu/`, which drives a whole update — upload, reset, reconnect,
trial boot, confirmation — against a stub device on another thread, and runs on
every push.
And `smply::winrt_ble`, the reference Bluetooth LE adapter, with
`examples/winrt_ble_dfu/` — a console tool that installs firmware over BLE.
663 tests, 16 CI jobs green plus a nightly fuzz soak and a weekly dependency
scan, and an installed package that
three separate out-of-tree projects consume on every push — by `find_package`,
by `add_subdirectory` and by `FetchContent`. Fourteen more cases run on
hardware, on a bench, by hand.

**The Windows side has now run against a real device** (roadmap P17a): the
WinRT adapter and `winrt_ble_dfu` completed updates in both directions against a
NUCLEO-WB55RG running Zephyr's `smp_svr`, with the device's state confirmed over
a UART path smply does not touch. CI still cannot exercise a radio — a GitHub
runner has none — so what CI proves about that code is that it builds; what the
bench proved is in [`docs/roadmap.md`](docs/roadmap.md) and the two READMEs,
[`transports/winrt_ble/README.md`](transports/winrt_ble/README.md) and
[`examples/winrt_ble_dfu/README.md`](examples/winrt_ble_dfu/README.md). The
first hardware run found three defects, none in the adapter: a real device's
CBOR is indefinite-length, its final upload chunk takes longer than 5 s to
answer, and the report misread the retransmission that hid.

**The hardware suite runs unattended** (P17b): fourteen cases over the public
API in thirteen groups, twelve of which need nobody present, a supervisor that
reflashes the board between groups and reports pass / fail / **unavailable**
per case, and a cross-check that installs the same
image with smply and with a third-party client and compares what the device
reports afterwards through a UART path neither of them touches (P17c). None of
it is in the pull-request gate, and no self-hosted runner is registered yet, so
it runs from the bench by hand.

What is deliberately not done: **the 1.0 declaration**. P18 built and proved
the package, wrote the versioning policy and left the version at `0.1.0`, so
that promising compatibility stays a decision somebody makes rather than a
side-effect of the packaging being finished
([ADR-0016](docs/decisions/ADR-0016-installed-package-and-versioning.md)). See
[`docs/roadmap.md`](docs/roadmap.md) for the phase-by-phase history.

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

### Running the example

```sh
build/linux-clang/examples/cli_dfu/cli_dfu
```

With no arguments it invents a device and an image to install, and performs the
whole update against a stub device running on a second thread — upload, mark for
test, reset, reconnect, trial boot, confirm. `--image PATH` installs a real
firmware file instead; `--mode` picks one of the three `UpdateMode`s.

`examples/cli_dfu/main.cpp` is the file to read: the other four are the stub
device it drives.

### Consuming it

```cmake
find_package(smply REQUIRED)
target_link_libraries(my_app PRIVATE smply::smply)
```

The package installs three targets: `smply::smply`, `smply::util` (the
thread-marshalling helper an adapter needs) and `smply::transport_common` (the
portable BLE framing and send admission a BLE adapter needs). `add_subdirectory`
and `FetchContent` work too, and `tools/check_install.sh` builds and runs a
consumer all three ways on every push.
[ADR-0016](docs/decisions/ADR-0016-installed-package-and-versioning.md) says
which targets ship and why the others do not.

### Checks

```sh
tools/format.sh              # reformat; --check to verify only
tools/lint.sh                # clang-tidy (+ cppcheck when installed)
tools/coverage.sh            # coverage report; --enforce applies the thresholds
python3 tools/check_public_headers.py   # no platform/third-party types in public headers
python3 tools/check_deps.py             # dependencies declared and pinned by hash
python3 tools/check_docs.py             # documentation gate (ADR-0013)
python3 tools/sbom.py --check           # every dependency has a licence entry
tools/check_install.sh       # consume the package out of tree, all three ways
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
| wanting working code               | [`examples/cli_dfu/main.cpp`](examples/cli_dfu/main.cpp) |
| wondering *why* something is so    | [`docs/decisions/`](docs/decisions/)       |

## Documentation index

* [`docs/architecture.md`](docs/architecture.md) — components, dependency
  direction, threading, async model, error model, trust boundaries.
* [`docs/design.md`](docs/design.md) — detailed design: SMP codec, reassembly,
  request tracking, upload state machine, DFU state machine.
* [`docs/protocol-notes.md`](docs/protocol-notes.md) — authoritative spec
  inventory, verified wire details, ambiguities and version dependencies.
* [`docs/api.md`](docs/api.md) — the public C++ headers, every one of them
  shipped.
* [`docs/testing.md`](docs/testing.md) — unit, component, fuzz and HIL strategy.
* [`docs/quality-gates.md`](docs/quality-gates.md) — CI matrix, warnings,
  static analysis, coverage, sanitizers, Definition of Done.
* [`docs/security.md`](docs/security.md) — threat model and trust boundaries.
* [`docs/dependencies.md`](docs/dependencies.md) — dependency inventory and
  licensing.
* [`docs/roadmap.md`](docs/roadmap.md) — phased, status-tracked execution plan.
* [`docs/handoff.md`](docs/handoff.md) — agent session handoff protocol and log.
* [`docs/decisions/`](docs/decisions/) — Architecture Decision Records.
* [`CHANGELOG.md`](CHANGELOG.md) — what changed, and the versioning policy.
* [`SECURITY.md`](SECURITY.md) — how to report a vulnerability, and what smply
  is and is not an authority on.

## Licence

**Apache-2.0** — see [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE). Permissive and
suitable for linking into proprietary applications, with an explicit patent
grant. Third-party licences are inventoried in
[`docs/dependencies.md`](docs/dependencies.md).
