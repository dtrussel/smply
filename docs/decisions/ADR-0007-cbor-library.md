# ADR-0007 — QCBOR behind a narrow façade

**Status:** Accepted (2026-09-04)

## Context

All MCUmgr payloads for groups below 64 are CBOR. smply needs to *encode* a
handful of small maps and *decode* responses that arrive from an untrusted
device. Writing a general-purpose CBOR implementation is explicitly not wanted.
The important properties are bounded parsing, predictable allocation, honest
error reporting on malformed input, a permissive licence, and a small footprint.

## Decision

**QCBOR**, wrapped by `smply::cbor::Reader` / `Writer` in `src/cbor/`, with
`src/cbor/backend_qcbor.*` as the only file that names it. No QCBOR type appears
in any public header (enforced by `tools/check_public_headers.py`).

## Candidates compared

| | **QCBOR** | TinyCBOR | zcbor | libcbor | nlohmann::json (CBOR) |
| - | - | - | - | - | - |
| Licence | BSD-3-Clause | MIT | Apache-2.0 | MIT | MIT |
| Language | C (C++-friendly) | C | C | C | C++ |
| Allocation | **none** | none | none | **malloc per node** | heavy, per-node |
| Bounded parsing | **yes, by design** | yes | yes | partial | no |
| Decode-by-map-key | **yes (`GetXxxInMapSZ`)** | manual walk | codegen or manual | tree walk | tree |
| Error handling | **sticky error, checked once** | per-call | per-call | per-call | exceptions |
| Hostile-input posture | explicit design goal; extensive fuzzing upstream | good | good | tree building on untrusted input | poor |
| Footprint | small | very small | small | medium | large |
| Maintenance | active | active, low churn | active (Nordic) | active | very active |

**Why QCBOR wins for this job.** The spiffy-decode API (`QCBORDecode_EnterMap`,
`QCBORDecode_GetUInt64InMapSZ`, …) maps one-to-one onto MCUmgr's "read these
named keys out of a map, some optional" shape, so the façade is thin and the
group code reads like the specification. Its sticky-error model means a
malformed response produces one error at the end rather than a cascade of
half-decoded values — exactly right for untrusted input. It allocates nothing,
so a device cannot induce host allocation. The licence is proprietary-friendly.

**Why not the others.** TinyCBOR is the close runner-up — smaller, MIT, equally
allocation-free — but its cursor-walking API forces the "find this key" logic
into smply, which is the error-prone part we want the library to own; it is the
designated fallback if QCBOR ever became unavailable. zcbor is appealing because
the *server* uses it, but its ergonomics centre on generated code from CDDL,
which is disproportionate for six small maps, and its manual API is clumsier
than QCBOR's from C++. libcbor builds an allocating tree from attacker-supplied
data — disqualifying on T3 in [`../security.md`](../security.md).
nlohmann::json is a large dependency with exception-based errors and per-node
allocation for a problem this small; it would contradict the dependency
philosophy outright.

## Decision detail: the façade

`Reader` returns `std::optional` for individual keys (absent is *normal* in
MCUmgr — [`../protocol-notes.md`](../protocol-notes.md) §6) and keeps decode
failures in a sticky `status()`. `Writer` writes into a caller-owned
fixed-capacity buffer and surfaces its sticky error from `finish()`. Arrays are
iterated through a callback so no container is allocated on the device's behalf.
Nesting depth is bounded by configuration.

## Consequences

* One runtime dependency for the whole library.
* The public API exposes typed structs (`ImageState`, `UploadResult`), never a
  CBOR map — a stated requirement.
* Replacing the backend means rewriting one file; the façade's own test suite
  doubles as the conformance suite for any replacement.
* Views returned by `Reader` point into the response buffer and are valid only
  during the response callback; group code copies what it retains. This is
  documented at every use site and checked by ASan.
* QCBOR is pinned by tag and commit and appears in
  [`../dependencies.md`](../dependencies.md) and the SBOM.
