# ADR-0026 — An `Error` carries the OS diagnostic behind a transport failure

**Status:** Accepted (2026-10-11). Amends [ADR-0002](ADR-0002-result-and-error-type.md):
adds one diagnostic field to `Error`. ADR-0002's other decisions stand.

## Context

ADR-0002 gave `Error` four parts: `code`, `mgmt`, `reason` and `where`, and
rejected anything larger because those four "covered every diagnostic need
identified during design". They cover every need of the *core*. They do not
cover an adapter's.

Every transport adapter fails because an operating system call failed, and the
OS says why with a number: `errno` from a POSIX call, `GetLastError()` from a
Win32 one, an `HRESULT` from WinRT. The adapter maps that number onto an
`ErrorCode`, which is right for control flow, and then has nowhere to put the
number. `where()` is a static literal and `reason()` is the device's `rsn`. So
`EACCES` and `EBUSY` both arrive as `TransportError` with the same tag, and a
WinRT failure on the bench says "the Bluetooth stack raised an error" with the
`HRESULT` that would say which error already gone. The person debugging a
bench run, or a user's report, is the one who pays.

## Decision

1. **`Error` gains one optional diagnostic: the OS error, as a
   `std::error_code`.**

   ```cpp
   [[nodiscard]] const std::error_code& os_error() const noexcept; // empty: none
   Error&  with_os_error(std::error_code) & noexcept;
   Error&& with_os_error(std::error_code) && noexcept;
   ```

   The adapter chooses the category: `std::generic_category()` for `errno`,
   `std::system_category()` for `GetLastError()`, and a category of its own for
   a numbering the standard library does not know. The WinRT adapter defines
   `hresult_category()` for `HRESULT`.

2. **Only a transport adapter sets it.** The core makes no OS calls, so it
   never sets the field and never reads it. An `Error` that reaches the core
   from a transport, through `on_disconnected` or `send()`, is passed on with
   the field intact.

3. **It is a diagnostic, not part of the error's identity.** `operator==`
   ignores it, as it ignores `where`. Two `Disconnected` errors are the same
   failure to control flow whichever `errno` lay behind them.

4. **`to_string(const Error&)` renders it** as
   ` [os: <category>:<value> <message>]`. `to_string(ErrorCode)` is unchanged
   and still does not allocate.

## Alternatives considered

**Widen `reason()`'s contract.** `reason()` is the device's `rsn`:
attacker-controlled text that must be capped and escaped before it is shown
(`security.md`, T12). Putting local OS text into the same field means a caller
can no longer tell which of the two it is reading, and the escaping rule
becomes "sometimes". Rejected.

**smply's own `{domain, code}` struct.** Simpler to read, but every consumer
would then need its own table to turn a number into text, and the core would
have to name the domains, which means naming the platforms in
`include/smply/`. `std::error_code` already is that pair, with `message()`, and
the standard library already defines the two categories most adapters need.
Rejected.

**A detail string.** An adapter could format the OS message into text. It
would allocate on every failure path, lose the number a program could act on,
and be a second free-text field beside `reason()`. Rejected.

**Leave it out of `Error` and let each adapter expose its last OS error.** A
`last_os_error()` on the adapter is read from a different place than the
failure it explains, after the fact, and races the adapter's own I/O thread.
Rejected.

## Consequences

* The installed, public `smply/error.hpp` changes. It is additive and
  source-compatible, which under 0.x is a minor bump
  ([ADR-0016](ADR-0016-installed-package-and-versioning.md)), recorded in the
  CHANGELOG. `Error` grows by one `std::error_code`.
* `error.hpp` includes `<system_error>`. That is a standard header, so the
  public-header gate is unaffected, and the core still names no platform.
* The serial port adapter attaches `errno` (POSIX) or `GetLastError()` (Win32)
  to every failure that came from a system call, and the WinRT adapter attaches
  the `HRESULT` of every `hresult_error` it catches. A failure that has no OS
  number, such as a hang-up seen through `poll()` or a GATT status, carries an
  empty one.
* `api.md`, `design.md` §9 (failure reporting in the transport contract) and
  the `Error` class comment describe the field and who sets it.
* `std::error_code`'s category is a pointer to a static object. An `Error`
  carrying one stays a cheap, copyable value.
