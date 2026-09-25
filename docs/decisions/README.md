# Architecture Decision Records

One file per decision. Format: Status · Context · Decision · Alternatives
considered · Consequences.

`Status` is one of `Proposed`, `Accepted`, `Superseded by ADR-NNNN`,
`Deprecated`. **Never edit an accepted decision in place to change it** — write
a new ADR that supersedes it and update the old one's status
([`../quality-gates.md`](../quality-gates.md) §11, and the process in
[ADR-0013](ADR-0013-living-documentation.md)).

Some ADRs carry a note appended to the `Status` line. That is the one edit an
accepted decision may receive. It is used where **the decision still holds but
something learned since changes how to read it**: a validated assumption, a
defect in a chosen dependency, a question later answered by measurement, or a
part superseded by a later ADR. A note never changes what was decided. If it
had to, the change would be a new ADR.

ADR bodies record the context of their day, including development-phase IDs
(`P<n>`) that the living documents no longer use
([ADR-0018](ADR-0018-maintenance-process.md)). `git log` resolves them.

| ADR | Title | Status |
| --- | ----- | ------ |
| [0001](ADR-0001-cpp-standard.md) | C++20 as the language baseline | Accepted |
| [0002](ADR-0002-result-and-error-type.md) | `Result<T>` + structured `Error` | Accepted |
| [0003](ADR-0003-async-model.md) | Sans-IO callbacks with an application-driven pump | Accepted |
| [0004](ADR-0004-threading-model.md) | Single client context, no internal threads | Accepted |
| [0005](ADR-0005-transport-abstraction.md) | Abstract `Transport`: whole message out, byte stream in | Accepted, qualified on hardware |
| [0006](ADR-0006-reassembly-location.md) | SMP reassembly lives in the core | Accepted |
| [0007](ADR-0007-cbor-library.md) | QCBOR behind a narrow façade | Accepted; QCBOR defect noted |
| [0008](ADR-0008-upload-state-ownership.md) | Upload state as a pure function owned by `ImageManagement` | Accepted |
| [0009](ADR-0009-mcuboot-boundary.md) | MCUboot responsibility boundary | Accepted |
| [0010](ADR-0010-request-correlation.md) | Correlation, SMP version default, one request in flight | Accepted; O2 resolved |
| [0011](ADR-0011-build-and-dependencies.md) | Target-based CMake, FetchContent, pinning | Accepted |
| [0012](ADR-0012-test-and-fuzz-tooling.md) | Catch2 v3 and libFuzzer | Accepted |
| [0013](ADR-0013-living-documentation.md) | Documentation is part of the product | Accepted; rule 4 superseded by 0018 |
| [0014](ADR-0014-confirmation-is-the-applications-call.md) | Confirmation is the application's call | Accepted |
| [0015](ADR-0015-hardware-evidence.md) | Hardware evidence and isolated bench tooling | Accepted |
| [0016](ADR-0016-installed-package-and-versioning.md) | What the installed package contains, and what a version promises | Accepted; target list extended by 0019 |
| [0017](ADR-0017-serial-framing-placement.md) | MCUmgr serial framing: portable, in `transports/serial/`, with a receiver | Accepted; decision 1 qualified by 0020 |
| [0018](ADR-0018-maintenance-process.md) | A maintenance process: a backlog, not a phase log | Accepted |
| [0019](ADR-0019-async-adapters.md) | Coroutine and future adapters, in an installed `smply::asyncutil` | Accepted |
| [0020](ADR-0020-serial-port-reference-adapter.md) | A reference serial port adapter, in `transports/serial_port/`, not installed | Accepted |
| [0021](ADR-0021-multi-image-update.md) | Several images on one device in one update, with per-image commit ownership | Accepted |
