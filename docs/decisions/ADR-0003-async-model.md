# ADR-0003 — Sans-IO callbacks with an application-driven pump

**Status:** Accepted (2026-09-04)

## Context

MCUmgr operations are inherently asynchronous, and an image upload is a long
sequence of them. The library must support request timeouts, cancellation,
transport disconnection, late responses, retries and progress reporting — while
running inside a Windows GUI application, inside a console tool, and inside unit
tests, without depending on WinRT coroutines, a Qt event loop, Windows handles,
`asio`, or any particular executor.

## Decision

**The core is sans-IO.** It performs no I/O, starts no threads and reads no
clock of its own. Concretely:

* Every operation takes a completion callback (`std::function`) and returns a
  `RequestHandle` usable for cancellation.
* Time enters through an injected `Clock`; deadlines are only evaluated when the
  application calls `SmpClient::poll(now)` / `FirmwareUpdater::poll(now)`.
* `next_deadline()` lets the application block precisely in its own event loop
  instead of polling in a spin.
* Callbacks are invoked from `poll()` or from `on_bytes()` — i.e. always on the
  application's pump thread, and never re-entrantly from inside the call that
  started the operation.
* Long-running work (an upload) is expressed as a state machine advanced by
  responses, not as a blocking loop.

Optional, opt-in adapter headers convert this substrate to other styles for
callers who want them (`std::future`, C++20 awaitable). They are separate
targets and the core never uses them.

## Alternatives considered

**Futures/promises as the primary API.** `std::future` has no continuations,
forces either a blocking `get()` (unusable on a UI thread) or a polling loop,
and makes progress reporting and cancellation awkward. It is a poor primary
model but a fine *adapter*, which is where it ended up.

**C++20 coroutines in the core.** Ergonomic for the sequential DFU flow, but
they require choosing an awaitable/executor model, which is precisely the
dependency we are trying not to impose; they complicate cancellation and
lifetime (a coroutine frame outliving its transport is a real hazard); and they
make deterministic single-stepping in tests harder. The DFU flow is a state
machine either way — writing it explicitly costs little and buys testability.
Deliberately available as an *adapter*.

**Caller-provided executor** (a `post(fn)` interface the core calls into). More
flexible, and a common design. It adds a concept the caller must implement,
introduces implicit re-entrancy questions, and — with a single-threaded contract
([ADR-0004](ADR-0004-threading-model.md)) — buys nothing the pump does not
already give us. Rejected as unnecessary generality.

**Internal worker thread with a blocking API.** Simplest for a naive caller,
worst for everything else: hidden threads, hidden synchronisation, untestable
timing, and it forces the library to own the event loop. Rejected outright.

**Pure polling/state-machine API with no callbacks** (`step()` returning
events). Maximally testable, but pushes a large event-demultiplexing burden onto
every caller and reads badly at the call site. The chosen design is a hybrid:
callbacks for results, an explicit pump for time — the polling benefit without
the ergonomics cost.

## Consequences

* **Deterministic tests.** With `ManualClock` and `FakeTransport`, every
  timeout, retry, late response and cancellation path is reproducible with no
  sleeps and no threads. This is the main payoff.
* The application must run a pump. This is 5 lines
  ([`../api.md`](../api.md)) and it is what a GUI application is already doing.
* Callbacks must not block; documented on every public callback type.
* Nothing in the core can "wake up on its own" — a design constraint that is
  also the reason no hidden concurrency bug can exist there.
* Progress and DFU events flow through one event callback rather than many
  narrow ones, keeping the state machine's output surface small.
