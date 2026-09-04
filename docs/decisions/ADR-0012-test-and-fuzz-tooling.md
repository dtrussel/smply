# ADR-0012 — Catch2 v3 for tests, libFuzzer for fuzzing

**Status:** Accepted (2026-09-04)

## Context

The test suite is the primary artefact that keeps this library correct: the
protocol logic is dense, the failure modes are numerous, and there is no
hardware in the loop for most of it. The framework needs good parameterised and
table-driven support (the upload response table, the fragmentation invariant),
clean CTest integration, and it must not become a dependency burden.
Separately, SMP and CBOR parse untrusted binary input and need real fuzzing.

## Decision

**Catch2 v3** for unit and component tests.

* BSL-1.0 (permissive), fetched via `FetchContent`, pinned.
* `TEST_CASE` + `SECTION` fits the "same setup, many divergent paths" shape of
  protocol tests; `GENERATE` covers the fragmentation invariant over many
  fragment sizes without hand-written loops.
* `catch_discover_tests()` registers each case with CTest individually, so CI
  reports precise failures and can shard.
* Compiled (not header-only) in v3, keeping test build times sane.

**libFuzzer** (`-fsanitize=fuzzer,address,undefined`, Clang) for fuzzing.

* In-process, coverage-guided, no external runner, corpora as plain directories
  committed to the repository.
* The same targets run as a 20 000-iteration smoke gate on every PR and as a
  30-minute soak nightly ([`../quality-gates.md`](../quality-gates.md) §8).

## Alternatives considered

**GoogleTest.** The other mainstream choice: mature, excellent mocking, BSD-3.
Rejected narrowly — `SECTION`-style branching expresses protocol scenarios more
directly than fixtures, and smply needs almost no mocking (the test doubles are
hand-written and small by design, which is better than generated mocks for a
sans-IO library). GoogleTest would be a fine substitute if a consumer's
environment demanded it; nothing in the tests is deeply Catch2-specific beyond
the macros.

**doctest.** Lighter and faster to compile. Rejected: fewer facilities for
data-driven tests, and a smaller ecosystem for CI integration — the compile-time
saving is not worth it at this project's size.

**Boost.Test.** Would drag in Boost, contradicting the dependency philosophy.
Rejected.

**AFL++ instead of libFuzzer.** Stronger for deep, long-running campaigns and
better at file-format grammars. Rejected as the primary tool because libFuzzer's
in-process model integrates with the existing Clang+ASan CI job with no extra
infrastructure, and the parsers here are small. AFL++ remains a reasonable
addition if the nightly soak stops finding anything and deeper exploration is
wanted.

**Property-based testing (RapidCheck).** Genuinely attractive for the codec
round-trip and the fragmentation invariant. Deferred rather than rejected:
Catch2's `GENERATE` plus seeded randomised inputs covers the same ground for
now without another dependency. Revisit if the hand-rolled generators start to
feel like a framework.

## Consequences

* Two build-time-only dependencies, neither shipped.
* Test binaries are Catch2 executables; `ctest --output-on-failure` is the
  single entry point in every CI job.
* Fuzz targets are ordinary `LLVMFuzzerTestOneInput` functions in
  `tests/fuzz/`, buildable only under Clang — the fuzz option is guarded so a
  GCC or MSVC configure does not fail.
* Randomised tests must print their seed on failure and be reproducible from it
  ([`../testing.md`](../testing.md) §7).
* Any crash found is committed to the corpus with its fix, so the corpus becomes
  the regression suite for parser bugs.
