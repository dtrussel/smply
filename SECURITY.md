<!-- SPDX-License-Identifier: Apache-2.0 -->

# Security policy

## What smply is, and is not, an authority on

smply is a **host-side firmware update client**. It moves bytes to a device and
checks that they arrived intact. It performs **no cryptographic verification of
firmware authenticity** and must never be presented as doing so:

* authenticity is created by your **signing pipeline**, which holds the private
  key, and enforced by **MCUboot** on the device, which holds the public key;
* smply computes SHA-256 only for the MCUmgr `sha` field — transfer integrity
  and upload resume — and compares the device's `match` answer against it;
* if MCUboot signature verification is disabled on your device, nothing smply
  does can compensate.

BLE link encryption is not a substitute for image signing. The full threat
model, with mitigations per threat, is in
[`docs/security.md`](docs/security.md).

Everything received from a device is treated as untrusted input. Every length,
offset, array size, string and nesting depth is bounded before it is used to
size, index or allocate anything; the bounds are gathered in
[`include/smply/limits.hpp`](include/smply/limits.hpp) and tabulated in
[`docs/architecture.md`](docs/architecture.md) §9. Seven libFuzzer targets run
over the decoders that read bytes they did not write.

## Supported versions

smply is pre-1.0. **Only the latest release is supported**: fixes land on the
default branch and go out in the next release rather than being backported.
[ADR-0016](docs/decisions/ADR-0016-installed-package-and-versioning.md) sets out
what a version number promises, and
[`CHANGELOG.md`](CHANGELOG.md) records what changed.

| Version | Supported |
| ------- | --------- |
| 0.1.x   | ✅ |
| < 0.1   | ❌ (no such release) |

## Reporting a vulnerability

**Please do not open a public issue for a security defect.**

Report it through GitHub's private vulnerability reporting on this repository —
the **Security** tab, then **Report a vulnerability**. That opens a private
advisory visible only to the maintainers.

Please include, as far as you can:

* which version or commit you tested, and the platform and transport;
* what an attacker controls — most plausibly a **malicious or malfunctioning
  device**, since that is the untrusted side of every interface smply has;
* a reproduction. A byte sequence is ideal: a crashing input for one of the
  fuzz targets in [`tests/fuzz/`](tests/fuzz/) is the most directly actionable
  form a report can take.

What to expect: an acknowledgement within **7 days**, an assessment within
**30 days**, and credit in the advisory and the changelog unless you would
rather not be named. If a report turns out to describe something already
documented as a non-guarantee (see below), we will say so plainly and explain
why rather than quietly closing it.

## Out of scope

These are documented properties rather than defects, and are listed in
[`docs/security.md`](docs/security.md) §3. A report about one of them is
welcome as an ordinary issue, but it is not a vulnerability:

* smply not verifying image signatures — that is MCUboot's job, by design;
* a device that lies about its own image state. smply bounds what such a device
  can *do to the host*; it cannot tell you the device is honest;
* device authentication, pairing policy and key storage, which belong to the
  application and the platform;
* a malicious *application* misusing the library. smply trusts its caller.

Bounded resource use under hostile input **is** in scope: if a device can make
smply allocate on a size it supplied, loop without terminating, read out of
bounds, or grow a buffer past
[`limits.hpp`](include/smply/limits.hpp), that is a defect and we want to hear
about it.
