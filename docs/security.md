# Security considerations

Scope: a **host-side firmware update client**. It runs on a Windows desktop with
the user's privileges and talks to a possibly-untrustworthy embedded peer.

## 1. Trust boundaries

```
┌──────────────────────┐   signs    ┌──────────────────────┐   verifies  ┌──────────────┐
│ build/signing        │──────────► │ smply (host)         │────────────►│ MCUboot      │
│ pipeline             │  image +   │ moves bytes,         │  image over │ (device)     │
│ holds the PRIVATE KEY│  TLV hash  │ checks TRANSFER      │  SMP/BLE    │ holds the    │
│                      │  + sig     │ integrity only       │             │ PUBLIC KEY   │
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
| T12 | **Sensitive data in logs.** | Log levels carry no payload bytes by default; hashes are truncated to 8 hex characters; device-supplied `rsn` strings are length-capped and escaped before logging (they are attacker-controlled text). No log statement in the library formats a raw buffer at default verbosity. |
| T13 | **Denial of service against the device** — a runaway client hammering the SMP server. | One outstanding request by default; bounded chunk retries and bounded upload restarts; no automatic reconnect loop (reconnection is the application's, and therefore rate-limitable, decision). |
| T14 | **Supply-chain risk in dependencies.** | Minimal footprint (one runtime dependency), exact tag+hash pinning, weekly OSV scanning, SBOM per release ([`quality-gates.md`](quality-gates.md) §9). |

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

Security-relevant defects are handled through the repository's normal issue
process until a `SECURITY.md` disclosure policy is added (tracked as a P0
follow-up).
