# Architecture Decision Records

One file per decision. Format: Status · Context · Decision · Alternatives
considered · Consequences.

`Status` is one of `Proposed`, `Accepted`, `Superseded by ADR-NNNN`,
`Deprecated`. **Never edit an accepted decision in place to change it** — write
a new ADR that supersedes it and update the old one's status
([`../quality-gates.md`](../quality-gates.md) §11, and the process in
[ADR-0013](ADR-0013-living-documentation.md)).

| ADR | Title | Status |
| --- | ----- | ------ |
| [0001](ADR-0001-cpp-standard.md) | C++20 as the language baseline | Accepted |
| [0002](ADR-0002-result-and-error-type.md) | `Result<T>` + structured `Error` | Accepted |
| [0003](ADR-0003-async-model.md) | Sans-IO callbacks with an application-driven pump | Accepted |
| [0004](ADR-0004-threading-model.md) | Single client context, no internal threads | Accepted |
| [0005](ADR-0005-transport-abstraction.md) | Abstract `Transport`: whole message out, byte stream in | Accepted |
| [0006](ADR-0006-reassembly-location.md) | SMP reassembly lives in the core | Accepted |
| [0007](ADR-0007-cbor-library.md) | QCBOR behind a narrow façade | Accepted |
| [0008](ADR-0008-upload-state-ownership.md) | Upload state as a pure function owned by `ImageManagement` | Accepted |
| [0009](ADR-0009-mcuboot-boundary.md) | MCUboot responsibility boundary | Accepted |
| [0010](ADR-0010-request-correlation.md) | Correlation, SMP version default, one request in flight | Accepted |
| [0011](ADR-0011-build-and-dependencies.md) | Target-based CMake, FetchContent, pinning | Accepted |
| [0012](ADR-0012-test-and-fuzz-tooling.md) | Catch2 v3 and libFuzzer | Accepted |
| [0013](ADR-0013-living-documentation.md) | Documentation is part of the product | Accepted |
