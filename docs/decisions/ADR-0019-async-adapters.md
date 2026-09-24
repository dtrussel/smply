# ADR-0019 — Coroutine and future adapters, in an installed `smply::asyncutil`

**Status:** Accepted (2026-09-24). Adds a target to the installed package
defined by ADR-0016; ADR-0016's rule for what may be installed is unchanged.

## Context

ADR-0003 chose callbacks and an application-driven pump. Its consequences named
futures and coroutines as thin wrappers anyone could add. `architecture.md`
went further and promised an optional `smply::asyncutil` target with a future
adapter and a coroutine adapter. That target never existed.

The callback style is correct, but it is expensive to read. An update written
as callbacks is a state machine the application writes by hand: read the
state, then in its callback mark for test, then in that callback reset, and so
on. A C++20 coroutine writes the same sequence as straight-line code, and the
compiler builds the state machine. The library's baseline is C++20
(ADR-0001), so every supported compiler has coroutines.

A future is the other common request. It is also the dangerous one:

* A smply callback runs only from `poll()` or `on_bytes()`, on the pump thread
  (ADR-0004).
* `std::future::get()` blocks.
* So a pump thread that blocks on a future waits for a callback that only it
  can run: a deadlock that no test with a background pump will ever show.

## Decision

**1. A header-only target, `smply::asyncutil`, installed and exported.**
* The headers live under `include/smply/async/`, in namespace `smply::async`.
* It links `smply::smply` and `smply::util`.
* It is a separate target for the same reason `smply::util` is: `libsmply`
  never links it, and `check_public_headers.py` rejects any core header or
  `src/` file that includes it.
* It is installed because it is written against the public API only, and is
  tested on every preset. That is ADR-0016's criterion for membership. It is
  not the `dfu_app` case, where code is shared by the examples and outside
  the promised surface.

**2. `smply/async/task.hpp`: coroutines, for code on the pump thread.**
* `async::await_result<T>(start)` is an awaitable that runs `start(callback)`
  and resumes the coroutine with the `Result<T>` the callback receives. Any
  smply operation that takes a `Callback<T>` works with it, including a
  `RequestHandle`-returning group command, an upload and a raw request.
* `async::Task<T>` is a minimal coroutine type:
  * it starts eagerly;
  * it can itself be awaited;
  * it exposes `done()` and `result()`.
* The coroutine resumes **inside** the completion callback, on the pump thread.
  That is exactly where a callback chain would continue, so the ADR-0004 rule
  holds with no new thread and no new queue.
* Destroying a suspended `Task` does not cancel the operation it awaits. The
  operation still completes, and its result is dropped. The awaitable holds its
  state in a `shared_ptr` so that this is safe rather than a dangling resume.

**3. `smply/async/future.hpp`: futures, for code on another thread.**
* `async::post_for_future<T>(dispatcher, start)` posts `start` through a
  `smply::Dispatcher`, so the operation begins on the pump thread when that
  thread next drains the dispatcher. It returns a `std::future<Result<T>>`
  fulfilled by the callback.
* **Calling `get()` on it from the pump thread deadlocks**, and the header says
  so first. There is no reliable way to detect it, so the rule is documented,
  not checked.
* A dispatcher cleared or destroyed before the work runs breaks the promise.
  `get()` then throws `std::future_error`, rather than blocking forever.

**4. `FirmwareUpdater` gets no adapter.** It is an event stream that needs the
application's participation (reconnect, confirm), not one callback with one
result. Its individual steps can already be awaited through the groups.

## Alternatives considered

**Keep it an idea, and delete the promise from `architecture.md`.** This is
honest, but it leaves every application to write the same awaitable. The first
awaitable most people write resumes a coroutine that has already been
destroyed.

**Put `Task` and the awaitable in the core.** Adding coroutines to `libsmply`
would change nothing the core does, and would add a coroutine type to the
public surface of every consumer. It stays opt-in, like `Dispatcher`.

**Use an existing coroutine library** (cppcoro, libunifex, `std::execution`).
Each is either a new dependency (ADR-0011) or not yet in the standard library
of every supported compiler. The one type this needs is about a hundred lines.

**Resume through `SmpClient::defer()` instead of inside the callback.** This
would need the awaitable to know the client, and it would add a poll cycle of
latency to every step. It would buy nothing: continuing inside the callback is
what a callback chain already does.

## Consequences

* The installed package has four targets: `smply::smply`, `smply::util`,
  `smply::transport_common` and `smply::asyncutil`. The consumption smoke test
  uses the new one, so the export is checked in all three consumption modes.
* **One more compatibility promise** under ADR-0016's versioning policy.
* `architecture.md` §4 and §12 stop describing the adapters as missing, and
  §5's separate-target rule covers `smply::asyncutil` as well as `smply::util`.
