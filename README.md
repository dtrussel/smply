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

**Version 0.1.0.** The library does what it exists to do:
* SMP framing and streaming reassembly;
* a bounded CBOR façade;
* request correlation with timeouts and cancellation;
* the OS and image management groups;
* MCUboot image parsing with SHA-256;
* the image upload state machine;
* `FirmwareUpdater`, which drives the whole update: upload, reset, reconnect,
  trial boot and confirmation.

Around the core library:
* **For adapter authors:** `smply::Dispatcher` (a thread-marshalling helper
  the core does not link), portable BLE helpers, and MCUmgr's serial console
  framing.
* **Examples:** `examples/cli_dfu/` runs a whole update against a stub device
  on every push. `smply::winrt_ble` with `examples/winrt_ble_dfu/` installs
  firmware over Bluetooth LE on Windows.
* **Verification:** every decoder that reads untrusted bytes is fuzzed. An
  installed package is consumed on every push by three separate out-of-tree
  projects, by `find_package`, `add_subdirectory` and `FetchContent`.

**What CI proves, and what a bench proved.** CI builds and tests everything on
Linux and Windows, but a CI runner has no radio. So for the WinRT adapter, CI
proves only that it builds. The adapter and `winrt_ble_dfu` have completed
updates in both directions against a NUCLEO-WB55RG running Zephyr's `smp_svr`,
from a hardware bench run by hand (`tests/hil/`). Those runs found three
defects that no simulated test could:
* a real device's CBOR is indefinite-length;
* its final upload chunk takes more than 5 s to answer;
* the report misread the retransmission that hid.

([`docs/protocol-notes.md`](docs/protocol-notes.md) §9.) The serial framing is
tested against a transcription of Zephyr's source, but has not yet been used
against a device.

**Deliberately not done: the 1.0 declaration.** Promising compatibility is a
decision somebody makes, not a side-effect of the packaging being finished
([ADR-0016](docs/decisions/ADR-0016-installed-package-and-versioning.md)).
Current work and open questions are in [`docs/roadmap.md`](docs/roadmap.md).

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
portable BLE framing, send admission and serial framing an adapter needs). `add_subdirectory`
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
| picking up work on smply           | [`docs/handoff.md`](docs/handoff.md) → [`docs/roadmap.md`](docs/roadmap.md) |
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
* [`docs/roadmap.md`](docs/roadmap.md) — work in progress, open questions and
  the backlog.
* [`docs/handoff.md`](docs/handoff.md) — how to work on smply, and the standing
  caveats.
* [`docs/decisions/`](docs/decisions/) — Architecture Decision Records.
* [`CHANGELOG.md`](CHANGELOG.md) — what changed, and the versioning policy.
* [`SECURITY.md`](SECURITY.md) — how to report a vulnerability, and what smply
  is and is not an authority on.

## Licence

**Apache-2.0** — see [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE). Permissive and
suitable for linking into proprietary applications, with an explicit patent
grant. Third-party licences are inventoried in
[`docs/dependencies.md`](docs/dependencies.md).
