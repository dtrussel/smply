# smply — instructions for coding agents

**Before doing anything, read [`docs/handoff.md`](docs/handoff.md)** — its
session protocol and its **§ Standing caveats**, which is the distilled version
of everything earlier sessions learned the hard way. This file is only the
short version.

## The rules that matter

1. **The repository is the authoritative project state.** Never rely on
   conversation history. If something is worth knowing next session, write it
   down.
2. **Work the next incomplete phase in [`docs/roadmap.md`](docs/roadmap.md)**,
   and stay inside its scope. Work found outside it goes into the roadmap's
   "Discovered follow-up work".
3. **Documentation is part of the product.** A change that makes
   `architecture.md`, `design.md` or `api.md` inaccurate is not complete until
   they are updated *in the same change*
   ([ADR-0013](docs/decisions/ADR-0013-living-documentation.md)).
4. **Never silently deviate from an ADR.** To change a decision: write a new ADR
   that supersedes the old one, update the docs and the roadmap, *then*
   implement. See [`docs/handoff.md`](docs/handoff.md).
5. **Protocol behaviour is traced to primary sources.** Zephyr and MCUboot
   documentation and source only — third-party clients are for behavioural
   comparison, never for copying code or inferring the protocol. New findings go
   in [`docs/protocol-notes.md`](docs/protocol-notes.md).
6. **Treat everything from the device as untrusted.** Bound every length before
   using it; never allocate on a device-supplied size.
7. **Keep the core platform-independent.** No WinRT, no Windows, no BLE, no
   threads and no clock in `include/smply/` or `src/`.
8. **Finish with the end-of-session checklist** in
   [`docs/handoff.md`](docs/handoff.md), including a session-log entry.

## Layout

`include/smply/` public headers · `src/` implementation · `transports/` platform
adapters · `tests/` unit, component, fuzz, HIL · `docs/` living documentation
and ADRs. Full description: [`docs/architecture.md`](docs/architecture.md) §10.

## Conventions

C++20, no compiler extensions. Every file starts with
`// SPDX-License-Identifier: Apache-2.0` (`#` for CMake, Python and shell).
Warnings are errors for smply's own targets.
No owning raw pointers, no C-style casts, no `reinterpret_cast` over device
data. Exhaustive `switch` over internal enums with no `default`. Public entry
points validate arguments and return `InvalidArgument` rather than asserting.
See [`docs/design.md`](docs/design.md) §11.

## Before you finish

`tools/format.sh --check`, `tools/lint.sh`, the three `tools/check_*.py` gates
and `ctest` must all pass. `tools/verify_gates.sh` proves the gates themselves
still work — run it if you touch anything under `tools/` or `cmake/`.

Three ways this has gone wrong before, all cheap to avoid:

* **Neither `cppcheck` nor `gcovr` is installed here, and both fail soft** —
  `lint.sh` skips cppcheck silently, `coverage.sh` falls back to plain `gcov`
  and reports a number that is not comparable. So a clean local run can still
  fail CI. `apt-get install -y cppcheck && pip install gcovr` first.
* **A failed build leaves the old test binary in place**, so `ctest` then
  reports the *previous* suite passing. Check the build's exit status
  separately — never read "N tests passed" as proof anything was rebuilt.
* **Build every preset, not just one.** GCC and Clang reject different things,
  in both directions, and Clang's ASan finds dangling callback captures that
  GCC's does not report at all.
