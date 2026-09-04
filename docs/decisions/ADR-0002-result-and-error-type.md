# ADR-0002 — `Result<T>` plus a structured `Error`

**Status:** Accepted (2026-09-04)

## Context

The library must distinguish at least: API misuse, malformed SMP, unsupported
SMP version, CBOR errors, MCUmgr protocol errors, *group-specific* MCUmgr
errors, timeout, cancellation, transport error, connection loss, unexpected
response and DFU state errors. Crucially, MCUmgr's own errors come in two
incompatible shapes (SMP v1 flat `rc`, SMP v2 `err:{group,rc}` — see
[`../protocol-notes.md`](../protocol-notes.md) §3) and the numeric values are
only meaningful together with their group. Exceptions are undesirable: this is a
library that a UI application drives on its own thread, most "errors" here are
ordinary expected protocol outcomes, and the baseline is C++20 without
`std::expected`.

## Decision

Two pieces:

1. **`Result<T>` = an `expected<T, Error>`.** Where `__cpp_lib_expected` is
   available it *is* `std::expected`; otherwise it is `smply::detail::expected`,
   a ~200-line implementation of the subset smply uses (`has_value`,
   `operator*`, `operator->`, `error()`, `value_or`, converting construction,
   `unexpected<Error>`), with the same names and semantics. A single alias
   `SMPLY_EXPECTED` selects between them.

2. **`Error` is a small value type**, not a string and not an `int`:
   * `ErrorCode code` — the machine-readable category, a closed enum;
   * `std::optional<MgmtError> mgmt` — `{group, rc, group_scoped}`, preserving
     the device's own numbers **and** the group they must be interpreted
     against, so a v2 `img` `rc == 30` is never confused with a v1 `rc == 30`;
   * `std::string reason` — the device's optional `rsn` text, diagnostics only;
   * `const char* where` — a static call-site tag for logs, never compared.

   `to_string(Error)` exists for humans and is explicitly not part of any
   control flow.

## Alternatives considered

**`std::error_code`.** The idiomatic non-throwing choice, and it composes with
the standard library. It cannot carry the `(group, rc)` pair without inventing a
category per MCUmgr group and encoding both numbers into one `int`, which loses
the distinction between "SMP-level rc" and "group-scoped rc" that SMP v2
requires clients to handle. It also gives the caller no place for `rsn`.
Rejected as the *primary* type; an `std::error_code` interop shim is listed as a
future extension.

**Exceptions.** Ruled out: protocol errors are expected outcomes, exceptions
across a callback boundary complicate the DFU state machine, and many embedded-
adjacent consumers build with exceptions disabled.

**A third-party `expected` (`tl::expected`, Boost.Outcome).** `tl::expected` is
excellent and CC0, but adding a dependency to obtain a type we will delete on
the move to C++23 is not worth the supply-chain surface for ~200 lines.
Boost.Outcome is a large framework for a small problem, which contradicts the
dependency philosophy.

**A bespoke error framework** with source locations, chaining and category
registries. Explicitly rejected as over-engineering; `code + mgmt + reason +
where` has covered every diagnostic need identified during design.

## Consequences

* Every fallible operation returns `Result<T>` or delivers one to a callback;
  no error is ever reported by a magic sentinel value.
* Callers switch on `ErrorCode` for control flow and read `mgmt()` only when
  they need device specifics.
* Group error enums that grow with new Zephyr releases
  ([`../protocol-notes.md`](../protocol-notes.md) §9 A2) never break decoding:
  unknown `rc` values pass through numerically.
* One conditional compilation seam (`SMPLY_EXPECTED`) must be tested in both
  configurations; CI builds the fallback explicitly even on toolchains that have
  `std::expected`.
* When the project moves to C++23, delete `detail/expected.hpp` — no public API
  change.
