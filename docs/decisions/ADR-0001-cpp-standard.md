# ADR-0001 — C++20 as the language baseline

**Status:** Accepted (2026-09-04)

## Context

smply is consumed by a Windows 11 native C++ application (MSVC v143 / Visual
Studio 2022) and must also build with GCC and Clang for testing, sanitizers and
fuzzing. The core is protocol code: buffer views, byte manipulation, small state
machines. Two things drive the choice — what materially improves *safety* here,
and what a Windows application team can actually adopt.

## Decision

**C++20**, with no compiler extensions (`CXX_EXTENSIONS OFF`,
`CXX_STANDARD_REQUIRED ON`), and MSVC built with `/permissive- /Zc:__cplusplus`.

C++20 features smply relies on:

* `std::span` — the entire transport and codec surface is span-based. In C++17
  this would mean either raw `(ptr, len)` pairs or a vendored span; both are
  worse.
* Designated initializers — makes the many small protocol config structs
  readable and hard to fill in the wrong order.
* `<concepts>`/`requires` — used sparingly, for constraining the small number of
  generic helpers.
* `consteval`/`constexpr` improvements — protocol constant tables and header
  encoding are compile-time checkable.
* `<bit>` (`std::bit_cast`, `std::endian`) — byte-order handling without
  `reinterpret_cast` or type punning.
* `operator<=>` — value types (`Header`, `Error`, `ImageVersion`) get correct
  comparisons for free, which tests lean on heavily.

## Alternatives considered

**C++17.** Widest compatibility, and the only real argument for it is if the
consuming application were pinned below VS2019 16.11. It costs `std::span`
(the single most load-bearing feature here) and `<bit>`, and would push us
toward raw pointer/length pairs in exactly the code where a length mistake is a
security bug. Rejected — but note the migration cost is not large should a
consumer constraint appear: span and bit_cast have well-known C++17 shims.

**C++23.** Attractive for exactly one thing, `std::expected`, plus
`std::byteswap` and `std::print`. As of the target toolchains, C++23 library
support is uneven across GCC 13 / Clang 17 / MSVC v143, and requiring it would
constrain the consuming application for a feature we can obtain another way
(see [ADR-0002](ADR-0002-result-and-error-type.md)). Choosing it now would be
novelty, not need. Rejected.

## Consequences

* Minimum toolchains: MSVC 19.30 (VS2022 17.0), GCC 11, Clang 14. CI pins
  GCC 13 / Clang 17 / MSVC v143.
* `std::expected` is unavailable; ADR-0002 defines the substitute and the
  forward path.
* Coroutines exist in the language but the core does **not** use them
  ([ADR-0003](ADR-0003-async-model.md)); an optional adapter header may.
* Revisit when the consuming application moves to C++23 — at that point
  `SMPLY_EXPECTED` collapses onto `std::expected` with no API change.
