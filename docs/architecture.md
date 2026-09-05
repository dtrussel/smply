# Architecture

Status: **authoritative**. Update this file in the same change that alters the
structure or behaviour it describes ([ADR-0013](decisions/ADR-0013-living-documentation.md)).

Companions: [`design.md`](design.md) (detailed mechanics),
[`protocol-notes.md`](protocol-notes.md) (wire truth), [`api.md`](api.md)
(headers), [`decisions/`](decisions/) (rationale).

---

## 1. System context

```
┌──────────────────────────────────────────────────────────────────┐
│ Windows 11 native application (WinUI/Win32, C++)                 │
│  • owns the UI thread and the event loop                         │
│  • owns BLE scanning, pairing, connection policy, reconnection   │
│  • owns the firmware file on disk                                │
└───────────┬─────────────────────────────────────┬────────────────┘
            │ uses                                │ uses
┌───────────▼──────────────┐        ┌─────────────▼────────────────┐
│ smply (portable core)    │◄───────┤ smply::winrt_ble (example)   │
│ no OS / no threads / no  │Transport│ C++/WinRT GATT adapter       │
│ clock / no sockets       │ contract│ Windows-only, separate target│
└───────────┬──────────────┘        └─────────────┬────────────────┘
            │ SMP messages                        │ GATT
            └─────────────────► ◄─────────────────┘
                    ┌───────────────────────┐
                    │ Zephyr device         │
                    │  MCUmgr SMP server    │
                    │  MCUboot bootloader   │
                    └───────────────────────┘
```

smply is a **library**, not a service. It owns protocol state only.

## 2. Architectural goals

| # | Goal | How it is enforced |
| - | ---- | ------------------ |
| G1 | The core is genuinely platform independent | A CI job builds `smply::smply` + all unit tests on Linux/GCC, Linux/Clang and Windows/MSVC. A header-hygiene gate greps public headers for forbidden includes. |
| G2 | Everything is testable without hardware | Sans-IO core: transport, clock and randomness are injected. `FakeTransport` + `ManualClock` cover every protocol path. |
| G3 | Protocol layers are separable | SMP framing has no knowledge of groups; groups have no knowledge of DFU; nothing below `dfu/` knows what a firmware file is. |
| G4 | Hostile device input cannot hurt the host | Every length is validated against a configured bound before allocation; fuzzers run over the parser and reassembler. |
| G5 | New MCUmgr groups and transports are additive | A group is a leaf module depending only on `SmpClient` + the CBOR façade. A transport implements one interface. |
| G6 | A fresh agent session can continue from the repo alone | Living docs + status-tracked roadmap + ADRs, enforced by a Definition-of-Done gate. |

Explicit non-goals: server-side SMP, MCUmgr groups other than 0 and 1,
image signing/verification, BLE connection management, an async framework.

## 3. Components and dependency direction

Dependencies point **downward only**. There are no upward or lateral
dependencies between peers, and no cycles.

```
                     ┌───────────────────────────────┐
   application  ───► │ FirmwareUpdater      (dfu/)   │   DFU orchestration,
                     │  UpdatePlan, UpdateState      │   reset/reconnect protocol
                     └───────────────┬───────────────┘
                                     │
                ┌────────────────────┼────────────────────┐
                ▼                    ▼                    ▼
     ┌────────────────────┐ ┌─────────────────┐ ┌────────────────────┐
     │ ImageManagement    │ │ OsManagement    │ │ ImageFile          │
     │ (groups/image/)    │ │ (groups/os/)    │ │ (image/)           │
     │  + UploadSession   │ │                 │ │ MCUboot hdr + TLV, │
     └─────────┬──────────┘ └────────┬────────┘ │ SHA-256            │
               │                     │          └────────────────────┘
               └──────────┬──────────┘                (no deps)
                          ▼
              ┌───────────────────────────┐
              │ SmpClient      (smp/)     │  request lifecycle: seq alloc,
              │  PendingRequestTable      │  correlation, timeouts,
              │  MessageAssembler         │  cancellation, reassembly
              └───────────┬───────────────┘
                          │
          ┌───────────────┼─────────────────┬──────────────┐
          ▼               ▼                 ▼              ▼
   ┌────────────┐  ┌────────────┐   ┌─────────────┐ ┌────────────┐
   │ smp codec  │  │ cbor façade│   │ Transport   │ │ core types │
   │ (smp/)     │  │ (cbor/)    │   │ (interface) │ │ Result,    │
   │ Header,    │  │ Reader/    │   │             │ │ Error,     │
   │ encode/    │  │ Writer     │   │             │ │ Clock,     │
   │ decode     │  │  ▲ QCBOR   │   │             │ │ Buffer     │
   └────────────┘  └────────────┘   └──────┬──────┘ └────────────┘
                                           │ implemented by
                          ┌────────────────┼────────────────┐
                          ▼                ▼                ▼
                 ┌─────────────┐   ┌──────────────┐  ┌─────────────┐
                 │ FakeTransport│  │ WinRT BLE    │  │ future:     │
                 │ (tests)      │  │ (transports/)│  │ UART/UDP/USB│
                 └─────────────┘   └──────────────┘  └─────────────┘
```

### Responsibilities

| Component | Owns | Must not know about |
| --------- | ---- | ------------------- |
| **core types** (`include/smply/`) | `Result<T>`, `Error`, `ErrorCode`, `MgmtError`, `Group`, `Clock`, `Duration`, byte-buffer aliases, `limits` | anything protocol-specific beyond group identity |
| **smp codec** (`src/smp/codec.*`) | `Header` encode/decode, byte order, field validation | CBOR, groups, transports |
| **MessageAssembler** (`src/smp/assembler.*`) | turning an arbitrary byte stream into complete SMP messages; bounded buffering; resync | who sent the bytes |
| **cbor façade** (`src/cbor/`) | bounded encode/decode of the CBOR shapes smply uses; `MgmtError` extraction | SMP header, groups' meaning |
| **SmpClient** (`src/smp/client.*`) | sequence allocation, pending-request table, per-request deadlines, cancellation, retired-seq set, dispatch of decoded responses, transport binding | firmware, images, files |
| **OsManagement** (`src/groups/os/`) | reset, mcumgr params, echo | DFU policy |
| **ImageManagement** (`src/groups/image/`) | image state get/set, upload request/response encoding, `UploadSession` state machine, erase, slot info | files on disk, reconnection |
| **ImageFile** (`src/image/`) | MCUboot header parse, TLV scan for `IMAGE_TLV_SHA256`, streaming SHA-256 of the file, chunk supply | SMP, CBOR, transports |
| **FirmwareUpdater** (`src/dfu/`) | the update state machine, reset/disconnect/reconnect protocol with the application, progress reporting | GATT, WinRT, threads, sockets |
| **Transport** (interface) | delivering one whole SMP message outbound; delivering inbound bytes; reporting link errors | SMP semantics, sequence numbers |

### Public vs internal

* **Public**: everything under `include/smply/`. Stable, documented, no
  third-party type in any signature (no QCBOR, no WinRT, no Catch2).
* **Internal**: everything under `src/`. Free to change. Tests may include
  internal headers via a `smply::smply_internal` interface target that adds
  `src/` to the include path — production consumers cannot.
* **Adapters**: `transports/` and `examples/` are separate CMake targets,
  never linked into the core.

### Extension points

1. **New transport** — implement `Transport`; nothing else changes.
2. **New MCUmgr group** — a new leaf under `src/groups/`, depending only on
   `SmpClient` + the CBOR façade. `Group` is an open enum carrying a `uint16_t`.
3. **New DFU policy** — `UpdatePlan` is data; the state machine is driven by it.
4. **Alternate CBOR backend** — replace `src/cbor/backend_qcbor.*`; the
   `cbor::Reader`/`Writer` façade is the seam.
5. **Alternate async style** — the callback core is the substrate; a
   futures/coroutine wrapper is a thin, optional header.

### Test seams

`Transport`, `Clock`, the `cbor` façade, `UploadSession` (pure function of
`(state, response) → action`), and `UpdateStateMachine` (pure
`(state, event) → (state, effects)`) are all individually substitutable and
individually testable. See [`testing.md`](testing.md).

## 4. Asynchronous model

**Sans-IO + explicit completion callbacks + an application-driven pump.**
Decision and alternatives: [ADR-0003](decisions/ADR-0003-async-model.md).

* Every operation takes a completion callback and returns a
  `RequestHandle` (cancellation token).
* The core performs no I/O and starts no threads. It writes to the transport
  when the application calls into it, and it makes progress on timeouts only
  when the application calls `SmpClient::poll(now)`.
* Time is a parameter, never a global. `Clock` is injectable; tests use
  `ManualClock`.
* Callbacks are invoked **from** `poll()` or from `on_bytes()` — i.e. always on
  the application's own pump thread, never re-entrantly from inside the call
  that started the operation.

Supported by construction: timeouts, cancellation, transport disconnection,
late responses, unexpected sequence IDs, bounded retries, and progress
reporting.

Optional, non-core convenience headers (opt-in, `smply::asyncutil` target):
`future_adapter.hpp` (callback → `std::future`) and `coro_adapter.hpp`
(callback → C++20 awaitable). Neither is used by the core.

## 5. Threading model

Decision: [ADR-0004](decisions/ADR-0004-threading-model.md).

* There is exactly one **client context**: the thread that calls `poll()`.
* `SmpClient`, the group clients and `FirmwareUpdater` are **not thread-safe**.
  All public calls on them, and all callbacks out of them, happen on the client
  context.
* `Transport::send()` is called on the client context.
* **`TransportListener::on_bytes()` / `on_error()` / `on_disconnected()` must
  also be invoked on the client context.** Marshalling from a driver thread
  (WinRT's thread pool, a serial reader thread) is the *adapter's*
  responsibility, because only the adapter knows its threading environment.
* smply ships `smply::Dispatcher` (a mutex + queue + wake callback) in a small
  utility target so adapters do not each reinvent the marshalling. The WinRT
  example uses it.
* No hidden threads anywhere in the core.

## 6. State ownership and lifetime

| State | Owner | Lifetime |
| ----- | ----- | -------- |
| reassembly buffer, pending requests, next seq, retired seqs | `SmpClient` | client |
| upload offset, session hash, chunk size, retry counters | `UploadSession`, owned by `ImageManagement` | one upload |
| DFU state, plan, target hash, progress | `FirmwareUpdater` | one update |
| firmware bytes | the **application** (`ImageSource`, e.g. a memory-mapped file) | ≥ the update |
| GATT session, MTU, device handle | the transport adapter | connection |

Rules:

* No owning raw pointers anywhere. `std::unique_ptr` for owned polymorphic
  objects, references/spans for borrowed ones.
* `SmpClient` **borrows** its `Transport` (reference/non-owning pointer);
  the application outlives it. Group clients borrow `SmpClient`;
  `FirmwareUpdater` borrows the group clients.
* Buffers passed to `Transport::send()` and to `on_bytes()` are **valid only for
  the duration of the call**. A transport that queues must copy.
* Destroying a `SmpClient` with pending requests completes each of them with
  `ErrorCode::Cancelled` before returning — no callback fires after destruction.
* All types are move-only where they own protocol state; no copying of
  in-flight state.

## 7. Error model

Decision: [ADR-0002](decisions/ADR-0002-result-and-error-type.md). Full types in
[`api.md`](api.md).

`Result<T> = expected<T, Error>` (`std::expected` when available, otherwise a
vendored minimal equivalent with the same subset API).

`Error` is a small value type: a machine-readable `ErrorCode`, an optional
`MgmtError { group, rc, smp_version }` preserving the device's own numbers, an
optional device-supplied `rsn` string, and a static `const char*` call site for
logs. **Strings are never the machine-readable representation.**

`ErrorCode` categories: `InvalidArgument`, `InvalidState`, `MalformedMessage`,
`UnsupportedSmpVersion`, `MessageTooLarge`, `CborDecode`, `CborEncode`,
`ProtocolError` (MCUmgr `rc`/`err`), `UnexpectedResponse`, `Timeout`,
`Cancelled`, `TransportError`, `TransportBusy`, `Disconnected`,
`ImageMismatch`, `UpdateFailed`, `Internal`.

Propagation: transport/codec errors surface as the `Result` of the affected
request; a link loss fails **all** pending requests with `Disconnected`; the
DFU state machine translates a failed step into a terminal `UpdateFailed`
carrying the underlying `Error` as its cause. Errors are never silently
swallowed and never converted to strings inside the library.

## 8. Security and trust boundaries

Detail: [`security.md`](security.md).

```
  build/signing pipeline  │  smply (host)          │  MCUboot (device)
  ────────────────────────┼────────────────────────┼───────────────────────
  produces a SIGNED image │  transports bytes;     │  VERIFIES the signature
  holds the private key   │  checks integrity only │  decides bootability
                          │  (SHA-256 of the file) │  performs swap/revert
```

* **Everything received from the device is untrusted.** Lengths, offsets, array
  sizes, string sizes and CBOR nesting are all bounded before allocation.
* **BLE encryption is not a substitute for image signing.** Link security
  protects confidentiality and pairing, not image authenticity. smply never
  presents "connected over an encrypted link" as an authenticity guarantee.
* smply performs **no** cryptographic verification of the image. It computes a
  SHA-256 for the MCUmgr `sha` field (transfer integrity + resume) only.
* Logging never includes payload bytes at default levels.

## 9. Configuration limits (defensive bounds)

Compile-time defaults in `include/smply/limits.hpp`, overridable per
`SmpClient` via `SmpClientConfig`:

| Limit | Default | Purpose |
| ----- | ------- | ------- |
| `max_smp_payload` | 8192 B | reject oversized `length` before buffering |
| `max_assembly_buffer` | 16 KiB | cap partial-message buffering |
| `max_cbor_nesting` | 16 | bound decoder recursion |
| `max_in_flight` | 1 | bound pending-request table |
| `max_retired_seqs` | 64 | bound late-response suppression |
| `default_timeout` | 5 s | per request |
| `first_chunk_timeout` | 30 s | implicit slot erase (protocol-notes §9 A7) |
| `erase_timeout` | 60 s | synchronous erase command |
| `upload_chunk_max` | 512 B | upper bound before `buf_size` negotiation |
| `max_image_size` | 16 MiB | sanity bound on a supplied firmware file |

## 10. Repository layout

Entries marked **(planned)** do not exist yet and name the phase that creates
them; everything else is present today. Keep this accurate — a layout that lists
files which were never written costs the next session a search.

```
smply/
├── CMakeLists.txt              options SMPLY_BUILD_{TESTS,FUZZERS,WINRT,EXAMPLES,HIL},
│                               SMPLY_USE_SYSTEM_QCBOR, SMPLY_FORCE_FALLBACK_EXPECTED
├── CMakePresets.json           named configs used verbatim by CI
├── cmake/                      warnings.cmake  sanitizers.cmake  dependencies.cmake
├── include/smply/              PUBLIC headers only — no third-party types
│   ├── bytes.hpp  clock.hpp  error.hpp  group.hpp  limits.hpp  result.hpp
│   ├── version.hpp.in          configured into the build dir as version.hpp
│   ├── detail/expected.hpp     C++20 fallback for std::expected (ADR-0002)
│   ├── transport.hpp           Transport + TransportListener; the contract is in the header
│   ├── smp_client.hpp          SmpClient, SmpClientConfig, RequestHandle, RawResponse
│   ├── smp/header.hpp          Operation, Version, Header, codec, response_to()
│   ├── groups/os.hpp           OsManagement, McumgrParameters, ResetOptions
│   ├── groups/image.hpp        ImageManagement, ImageState, ImageHash, ImageError;
│   │                           UploadOptions and UploadHandle are planned (P10)
│   ├── image_source.hpp        ImageSource, MemoryImageSource
│   ├── mcuboot_image.hpp       McubootImageInfo, parse_mcuboot_header, sha256,
│   │                           find_image_tlv_hash
│   ├── dfu/firmware_updater.hpp    (planned, P12)
│   └── util/dispatcher.hpp     (planned, P14) thread-marshalling helper for adapters
├── src/
│   ├── core.cpp                system_clock, group_name, to_string
│   ├── version.cpp
│   ├── smp/                    codec.cpp  assembler.{hpp,cpp}  client.cpp
│   ├── cbor/                   cbor.hpp  reader.cpp  writer.cpp  mgmt_error.{hpp,cpp}
│   │                           — reader/writer ARE the QCBOR backend; there is no
│   │                             separate backend file (P5 deviation)
│   ├── groups/os/              os_management.cpp
│   ├── groups/image/           image_management.cpp; upload_session.* planned (P10)
│   ├── image/                  image_source.cpp  mcuboot_header.cpp  tlv.cpp
│   │                           sha256.{hpp,cpp}  source_reader.hpp
│   ├── dfu/                    (planned, P12) update_state_machine.*  firmware_updater.cpp
│   └── util/                   (planned, P14) dispatcher.cpp
├── transports/winrt_ble/       (planned, P15) Windows-only target smply::winrt_ble
├── examples/                   (planned, P14/P16) cli_dfu/  winrt_ble_dfu/
├── tests/
│   ├── support/                fake_transport.*  manual_clock.hpp  message_builder.hpp
│   │                           — built as smply_test_support; ServerSimulator lands in P11
│   ├── unit/                   per-component
│   ├── component/              (planned, P11) full stack over FakeTransport + simulator
│   ├── fuzz/                   (planned, P13) libFuzzer targets + corpora
│   └── hil/                    (planned, P17) hardware interoperability, opt-in
├── tools/                      format.sh  lint.sh  coverage.sh  sources.sh
│                               check_public_headers.py  check_deps.py  check_docs.py
│                               verify_gates.sh  cppcheck-suppressions.txt
└── docs/                       this documentation set + decisions/
```

CMake is fully target-based: no `include_directories()`, no global
`add_compile_options()`. Warnings and sanitizers are applied via
`smply_internal_options` — an `INTERFACE` target linked `PRIVATE` by smply's own
targets, so consumers never inherit them. Install/export produces
`smply::smply` via `smplyConfig.cmake`.

## 11. Known limitations

* Groups other than OS and Image are not implemented.
* SMP v1 by default; v2 opt-in (protocol-notes §9 A1).
* One outstanding request; no pipelining (A10).
* Encrypted MCUboot images are not supported end-to-end (A13).
* Multi-image (image ≥ 1) upload is representable but only image 0 is exercised
  by the default `UpdatePlan` and by the HIL suite.
* No serial/UART transport in the initial scope.
* The core does not manage connections; reconnection is the application's job.

## 12. Planned future extensions

Ordered by expected value, none scheduled: UART/raw-UART transport ·
UDP transport · SMP v2 by default once the fleet supports it · request
pipelining driven by `buf_count` · Shell (group 9) and FS (group 8) ·
`std::error_code` interop for the error model · a C ABI shim.
