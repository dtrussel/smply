<!-- SPDX-License-Identifier: Apache-2.0 -->

# `smply::winrt_ble` — the Bluetooth LE transport for Windows

A `smply::Transport` over C++/WinRT's GATT API: SMP requests go out as GATT
write-without-response, responses arrive as notifications, and the whole
arrangement is specified in [`docs/design.md`](../../docs/design.md) §10 and
[`docs/protocol-notes.md`](../../docs/protocol-notes.md) §8.

## Read this first: what has and has not been verified

**This adapter has never talked to a device.** It was written on Linux, where
not one line of it can be compiled, and the `windows-winrt` CI job compiles it
and runs a link-and-call smoke test on a runner that has no Bluetooth radio. So:

| Claim | Status |
| ----- | ------ |
| It compiles at `/W4 /WX` and links | **verified** by CI |
| The SMP UUIDs are the ones in the specification | **verified** — the bytes and the endian split are unit-tested on every platform (`tests/unit/test_ble_framing.cpp`) |
| Fragment sizing and the fragment walk | **verified** — `transports/common/`, 100 % covered |
| The `close()` bookkeeping | **verified** as a state machine; its *WinRT* half is not |
| Discovery, notifications, writes, disconnect, MTU | **unverified.** No radio has ever seen this code |

Behavioural coverage arrives with the hardware interoperability suite (P17).
Treat a green CI badge as "it builds".

## Using it

```cpp
winrt::init_apartment(winrt::apartment_type::multi_threaded);   // see below

smply::Dispatcher inbound{[&] { wake_the_pump(); }};

auto link = smply::transport::WinRtBleTransport::connect(address, inbound);
if (!link.has_value()) { /* link.error() says why */ }

smply::SmpClient client{**link};        // the transport must outlive the client
// ... then the ordinary pump: inbound.drain(); client.poll(now);
```

`examples/cli_dfu/main.cpp` is the pump loop in full, against a stub device; the
only difference here is which transport is constructed.

## Four obligations, all of them easy to get wrong

1. **A multi-threaded apartment.** `connect()` blocks on WinRT asynchronous
   operations. On a single-threaded apartment that deadlocks — silently, with no
   diagnostic. Call `winrt::init_apartment(winrt::apartment_type::multi_threaded)`
   on the thread that connects.
2. **Drain the dispatcher.** Inbound bytes reach the core only through
   `Dispatcher::drain()` on the client context (ADR-0004). Nothing arrives
   without it, and the update simply times out.
3. **The transport outlives the client.** `~SmpClient` and `rebind_transport()`
   both detach through `set_listener(nullptr)`, so a transport destroyed first
   leaves those calls dangling (`smply/transport.hpp`).
4. **The dispatcher outlives the transport**, and is *not* cleared by it — see
   below.

## Two design points worth knowing

**`close()` does not touch the dispatcher.** `design.md` §10 originally
described the shutdown as "revoke → quiesce → **drain the queue** → mark
closed", which assumes the adapter owns the queue. It does not: the dispatcher
belongs to the application, and an application may run several links through one
(as `cli_dfu` does), so clearing it would discard another transport's work and
draining it would run arbitrary application closures from inside `close()`.
Instead every posted closure captures a strong reference to the adapter's state
and asks `LinkState::may_deliver()` before touching the listener. A closure that
outlives the link keeps its state alive, finds the link closed, and does
nothing. The guarantee — *no callback after `close()` returns* — is unchanged.

**The MTU is read once per message, not cached.** `GattSession::MaxPduSize` is
read at the top of each `send()`. Caching it would need a `MaxPduSizeChanged`
subscription, and therefore another event handler and another revoker in the
shutdown sequence, to avoid a stale value — for one property read per message.

## Building

```
cmake --preset windows-winrt      # -DSMPLY_BUILD_WINRT=ON
cmake --build --preset windows-winrt
ctest --preset windows-winrt
```

The option refuses a non-Windows host at configure time, and the core must go on
building with it off — which is what the `core-without-winrt` job exists to
prove.

## What is not here

Scanning, pairing and connection policy: this adapter is handed an address. A
dropped link stays dropped, so reconnecting after a device reboot means a *new*
transport and `SmpClient::rebind_transport()`. The console application that does
all of that is P16.

## Static analysis

clang-tidy and cppcheck do **not** see this directory (`tools/lint.sh`): both are
driven from a Linux build, where these translation units do not exist and the
projection headers cannot be parsed. clang-format still covers it, and MSVC's
`/W4 /WX` is the analysis that stands in. This is the only code in the
repository outside clang-tidy's reach; see
[`docs/quality-gates.md`](../../docs/quality-gates.md) §3.
