# Security considerations

Scope: a **host-side firmware update client**. It runs on a desktop or a
build machine with the user's privileges and talks to a possibly-untrustworthy
embedded peer, over BLE or -- since P20's framing -- a serial console.

## 1. Trust boundaries

```
┌──────────────────────┐   signs    ┌──────────────────────┐   verifies  ┌──────────────┐
│ build/signing        │──────────► │ smply (host)         │────────────►│ MCUboot      │
│ pipeline             │  image +   │ moves bytes,         │  image over │ (device)     │
│ holds the PRIVATE KEY│  TLV hash  │ checks TRANSFER      │  SMP over a │ holds the    │
│                      │  + sig     │ integrity only       │  link       │ PUBLIC KEY   │
└──────────────────────┘            └──────────────────────┘             └──────────────┘
        AUTHENTICITY                     INTEGRITY ONLY                    AUTHENTICITY
        originates here                  no authority                      enforced here
```

**The single most important rule: smply is not an authority on image
authenticity.** It never validates a signature and must never be presented as
doing so. Authenticity is created by the signing pipeline and enforced by
MCUboot. If MCUboot signature verification is disabled on the device, no
behaviour of smply can compensate.

Everything crossing the device boundary — every response byte, length, offset,
array size, string, hash and flag — is **untrusted input**.

## 2. Threats and mitigations

| # | Threat | Mitigation |
| - | ------ | ---------- |
| T1 | **Malicious/corrupt responses** — a peer (or a MITM on an unencrypted link) sends crafted SMP/CBOR to compromise the host. | Bounded parsers with no allocation driven by device-supplied sizes; no `reinterpret_cast` over device data; ASan/UBSan in CI; dedicated fuzzers over the header, reassembler and every response decoder ([`testing.md`](testing.md) §5). |
| T2 | **Malicious length field** — `length` claims 64 KiB, or fragments never complete. | `length > max_smp_payload` ⇒ error before buffering; partial buffer capped by `max_assembly_bytes`; both are configured, not device-derived. |
| T3 | **Memory exhaustion / DoS on the host** — huge `images` arrays, deep CBOR nesting, endless partial messages. | Element-count caps on every array, `kMaxCborNesting`, capped assembly buffer, capped pending-request table. Worst case is a bounded error, never unbounded growth. Every one of these bounds now has a test that names the constant (`tests/unit/test_limits.cpp`), because a bound documented and not enforced looks exactly like one that is. |
| T4 | **Integer overflow** on offsets/lengths. | All offset arithmetic in `uint64_t` with explicit `off + len` overflow and range checks; `-Wconversion`/`-Wsign-conversion` as errors; UBSan `signed-integer-overflow`. |
| T5 | **Stale / replayed SMP responses** — a replayed response is attributed to a later request that reused the 8-bit `seq`. | Correlation on `(seq, group, command, op)`; a bounded **retired-sequence set** discards late responses for completed/cancelled/timed-out requests; the sequence allocator skips both pending and retired values. |
| T6 | **Unexpected device identity** — the wrong device answers on the SMP characteristic. | Out of smply's scope by design (connection policy is the application's). smply provides the material to check: image-state hashes and versions are surfaced verbatim so the application can refuse an unexpected device before starting. Documented as an application responsibility. |
| T7 | **Uploading the wrong image** to the wrong device/slot. | Pre-flight: MCUboot magic and header parsed from the file; version reported; `UpdatePlan` targets an explicit image number; the post-upload verify step requires the device to report the expected TLV hash before anything is marked for boot. Choosing *which* firmware is correct for a device remains the application's decision. |
| T8 | **Rollback / downgrade** — an attacker persuades the tool to install an older signed image. | smply does not decide policy. It exposes `upgrade_only` (server-enforced version check) and reports both versions so the application can refuse. Genuine anti-rollback is MCUboot's security counter (`IMAGE_TLV_SEC_CNT`), on the device. |
| T9 | **Silent corruption in transit.** | The `sha` field (SHA-256 of the whole file) plus the device's `match` response detect it; a `match == false` fails the update. MCUboot's own verification is the backstop. |
| T10 | **Bricking via a bad image.** | Default `UpdateMode::TestThenConfirm` uses MCUboot's trial-boot/revert mechanism: an image that never boots, or that boots but is never confirmed, is reverted on the next reset. `ConfirmImmediately` removes this net and is opt-in with that stated in its documentation. |
| T11 | **BLE link security mistaken for authenticity.** | Stated explicitly here, in [`architecture.md`](architecture.md) §8 and in the WinRT adapter's documentation. The adapter never reports pairing/encryption state as a security property of the *image*. Encryption protects the transfer; it says nothing about who signed the firmware. |
| T12 | **Sensitive data in logs.** | **smply has no logging**, which is the strongest form this mitigation can take: there is no log statement in `include/` or `src/` to leak anything, no log level to misconfigure, and no sink the library writes to. The application logs, and what it may be handed is bounded here — a device-supplied `rsn` string is capped at `limits::kMaxReasonLength`, and `error.hpp` states at the declaration that it is attacker-controlled text an application must escape before displaying it. P18's audit corrected this row: it previously described log levels, truncation and default verbosity for a logging subsystem that does not exist. |
| T13 | **Denial of service against the device** — a runaway client hammering the SMP server. | One outstanding request by default; bounded chunk retries and bounded upload restarts; no automatic reconnect loop (reconnection is the application's, and therefore rate-limitable, decision). |
| T14 | **Supply-chain risk in dependencies.** | Minimal footprint (one runtime dependency), exact tag+hash pinning, an SPDX 2.3 SBOM generated from those pins by `tools/sbom.py` and published by CI, and an advisory weekly OSV-Scanner run over it ([`quality-gates.md`](quality-gates.md) §9). The SBOM and the scanner were claimed here from P0 and built in P18. Read §9 before relying on a clean scan: it runs and produces a report, but its *scheduled* firing is unproven and nothing establishes that OSV has advisory coverage for two C libraries consumed from git — an empty report from a database with nothing to say looks exactly like one from a database that checked. |
| T15 | **A hostile serial frame stream.** A console link's framing is its own parser — markers, base64, a device-supplied big-endian length and a CRC — and all of it is decoded *before* a single SMP byte exists, so it sits outside every bound T2 and T3 describe. | Three bounds in `transports/serial/`, each with a test named for it ([`design.md`](design.md) §12): a line longer than a frame can be is **dropped, not buffered**, so a peer that never sends a newline cannot grow the line buffer; the declared length is checked against a configured `max_packet` **before the packet buffer is allowed past one frame's worth**, so the peak footprint is bounded by the cap and never by what the device asked for; and a body longer than its own declared length is an error rather than a trim. Base64 decoding is strict — no character outside the alphabet, no length that is not a whole quartet, no padding anywhere but the tail — because a lenient decoder turns a corrupt frame into a plausible one. `fuzz_serial_deframe` asserts all three bounds at every step over an arbitrary stream. |
| T16 | **A shared console is not a private channel.** On a device with `CONFIG_SHELL_BACKEND_SERIAL` the same stream carries a shell, its echo and a log backend, and the deframer is deliberately tolerant of lines that are not frames — so anything with write access to the port can interleave text of its choosing, including well-formed frames. | **Nothing changes in the trust model, which is the point**: §1 already treats every byte crossing the device boundary as untrusted, so an injected frame is exactly as trusted as a genuine one — which is to say not at all, and bounded by T15. What tolerance does *not* do is widen the attack surface: an ignored line is never decoded, so no byte of it reaches the base64 decoder, the length or the CRC. Who may open the port is the operating system's business and the application's, as T6 says of device identity. A serial link also offers no confidentiality at all, where BLE at least may be encrypted — and per T11 that was never a property of the *image* either. |

### What the P13 audit found

Auditing every bound in [`architecture.md`](architecture.md) §9 for an
enforcement site *and* a test turned up one real defect, which is recorded here
because it was a silent-failure bug rather than a crash.

**`kMaxCborNesting` was 16 while QCBOR's own `QCBOR_MAX_ARRAY_NESTING` is 15**,
so smply's documented bound was never the one in force. That would be harmless
if the two failures looked alike, and they do not: smply's own cap records a
sticky error, while QCBOR's is reported through `Reader::enter_map(key)`, whose
failure path is **deliberately not sticky** so that it can double as a probe for
an optional nested map. The result was that a document nested deeper than QCBOR
allows made the reader quietly stop descending with `status()` still clean — a
hostile response turning into *silently missing fields* rather than a decode
failure, for a caller following the documented "check `status()` at the end"
rule.

The fix is one constant: 14, not 15. Reaching a cap needs a document one level
deeper than the cap, so an equal value still lets QCBOR refuse first; only a
strictly lower one routes the refusal through smply's `record()`. The reasoning
is written next to the constant so it cannot be "tidied" back up.

Everything else in §9 was enforced where an untrusted number drives it. One case
is deliberately *not* bounded and is now documented as such: `sha256()` streams
an `ImageSource` the **application** supplies, in fixed-size chunks, so a large
one costs time rather than memory and there is no untrusted number to defend
against. `kMaxImageSize` binds where it matters — on the MCUboot header's own
declared size, and on the source offered to an upload.

**The fuzzers found nothing**, over 416 million executions across the seven
targets in the phase's local soak, under ASan and UBSan throughout. That is a
weaker statement than it looks — it is the *absence* of evidence over one
afternoon — which is why the arrangement that outlives it matters more: every
committed corpus input is replayed on every push
([`quality-gates.md`](quality-gates.md) §8), and the search continues nightly.
Each target asserts a property beyond "no crash", so a bound that quietly stops
holding fails the target rather than passing it silently — the same failure mode
the nesting defect above had.

## 3. Explicit non-guarantees

smply does **not** provide: image authenticity or confidentiality; protection
against a compromised device that lies about its image state; device
authentication or pairing policy; secure key storage; protection against a
malicious application using the library; anti-rollback enforcement.

## 4. Reporting

The disclosure policy is [`SECURITY.md`](../SECURITY.md) at the repository
root: private vulnerability reporting through GitHub's Security tab, an
acknowledgement within 7 days, and an explicit out-of-scope list that matches
§3 above. This document is the threat model; that one is how to tell us.
