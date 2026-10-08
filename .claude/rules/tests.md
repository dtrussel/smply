---
paths:
  - "tests/**"
---

# Tests

The strategy, the test doubles (`ManualClock`, `FakeTransport`,
`ServerSimulator`, the image builders) and the determinism rules are in
`docs/testing.md`; §3 lists the coverage each component must have. Tests use
Catch2 v3 (ADR-0012) and are registered with `ctest`; fuzz targets are not.

* Unit and component tests never read the real clock; drive time with
  `ManualClock`.
* A test that is meant to fail matches its expected output
  (`PASS_REGULAR_EXPRESSION`) rather than accepting any failure.
* Response goldens are built in both CBOR encodings
  (`tests/support/cbor_shapes.hpp`).
* `tests/hil/` needs the hardware bench and is never a PR gate: read
  `tests/hil/README.md` before changing it.
