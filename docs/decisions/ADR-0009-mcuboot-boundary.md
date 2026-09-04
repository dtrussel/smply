# ADR-0009 — MCUboot responsibility boundary

**Status:** Accepted (2026-09-04)

## Context

The uploaded artefact is an MCUboot-signed image with a 32-byte header, a body
and a TLV trailer ([`../protocol-notes.md`](../protocol-notes.md) §7). The
question is how much of that structure smply should understand. Doing too much
duplicates MCUboot and creates a second, weaker implementation of security-
relevant logic; doing too little means the tool cheerfully uploads a wrong file
and only fails minutes later on the device.

There is also a trap: MCUmgr uses **two different SHA-256 values** — the upload
`sha` (of the whole file) and image-state `hash` (`IMAGE_TLV_SHA256`, over
header + body + protected TLVs). Confusing them is the classic client bug.

## Decision

smply treats the image as **essentially opaque, with three narrow exceptions**.

**Does:**

1. **Parse the 32-byte header** (field by field from a byte span — never a
   struct cast): validate `ih_magic == 0x96F3B83D`, read `ih_hdr_size`,
   `ih_img_size`, `ih_flags` and `ih_ver`. Cheap, and it converts "upload fails
   after 200 KB with `INVALID_IMAGE_HEADER_MAGIC`" into "this is not an MCUboot
   image" before a single byte goes out. The server performs exactly this check
   itself ([`../protocol-notes.md`](../protocol-notes.md) §6 rule 3), so we are
   pre-empting a known rejection, not inventing a policy.
2. **Compute SHA-256 of the whole file**, streaming, for the MCUmgr `sha` field.
   This is *required* by the protocol for upload resumption and for the
   device-side `match` check. Not optional.
3. **Optionally scan the TLV area for `IMAGE_TLV_SHA256`**, so the uploaded file
   can be correlated with a device slot entry without taking the device's word
   for which image it is holding.

**Does not:**

* verify signatures (the device holds the public key and is the only party whose
  verdict matters);
* decrypt or handle encrypted images end-to-end;
* evaluate dependency TLVs, security counters or boot records;
* reimplement swap, revert or trailer logic;
* modify the image in any way.

**Version reporting** comes from the parsed header, not from a filename.

## Alternatives considered

**Fully opaque — upload bytes, nothing else.** Simplest and most obviously
correct in terms of responsibility. Rejected on two counts: SHA-256 is
protocol-mandatory anyway, so the "no image knowledge" purity is already broken;
and without the magic check the tool's single most common user error (picking
the unsigned `zephyr.bin` instead of `zephyr.signed.bin`) surfaces as an obscure
device error after a long transfer.

**Full parse and validation, including signature verification.** Would let the
tool refuse a bad image before touching the device. Rejected: it means embedding
a crypto library and a copy of MCUboot's verification rules that will drift from
the device's, and it invites the dangerous misreading that a host-side "valid"
verdict means anything. Verification belongs where the key is.

**Compute the TLV hash always, and require it to match the device.** Attractive
for strictness, but the TLV hash is absent or different for encrypted images and
for some MCUboot configurations, so a hard requirement would break legitimate
setups. Made optional and used as corroboration, not as a gate.

## Consequences

* `src/image/` depends on nothing else in the library and is trivially testable
  with golden headers and NIST SHA-256 vectors.
* SHA-256 is a ~150-line vendored public-domain implementation rather than a
  dependency on OpenSSL/BCrypt — see [`../dependencies.md`](../dependencies.md).
  Portable, auditable, and it keeps the runtime footprint at one third-party
  library.
* The trust boundary is stated in [`../security.md`](../security.md) §1 and must
  be repeated in user-facing documentation: **a successful smply update is not
  an authenticity statement.**
* Encrypted images are a known limitation
  ([`../protocol-notes.md`](../protocol-notes.md) §9 A13): upload works, TLV
  correlation is skipped, and the plan is flagged.
* TLV scanning is hardened (bounded `it_tlv_tot`, strictly-positive advance,
  iteration cap) and fuzzed, because it parses attacker-supplied file content.
