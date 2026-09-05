# Protocol notes

Authoritative, verified protocol facts for smply. **This file is the single
source of truth for wire behaviour.** Do not re-derive protocol details from
third-party clients; add findings here instead.

Verified on **2026-09-04** against Zephyr `main` and MCUboot `main`; sections 6
and 7 re-verified against the image-group implementation on **2026-09-05**.

---

## 1. Specification inventory

| # | Document / source | Scope | Retrieved from |
| - | ----------------- | ----- | -------------- |
| S1 | `doc/services/device_mgmt/smp_protocol.rst` | SMP framing, header, group IDs, minimal response, SMP v1/v2 error shape | `zephyrproject-rtos/zephyr@main` |
| S2 | `doc/services/device_mgmt/smp_transport.rst` | BLE service/characteristic UUIDs, BLE fragmentation rule, UART/console and raw-UART framing | same |
| S3 | `doc/services/device_mgmt/smp_groups/smp_group_0.rst` | OS management group (echo, reset, mcumgr params, bootloader info) | same |
| S4 | `doc/services/device_mgmt/smp_groups/smp_group_1.rst` | Image management group (state, upload, erase, slot info) | same |
| S5 | `include/zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h` | `mcumgr_op_t`, `mcumgr_group_t`, `mcumgr_err_t`, `MGMT_HDR_SIZE` | same |
| S6 | `include/zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h` | `IMG_MGMT_ID_*`, `img_mgmt_err_code_t` | same |
| S7 | `include/zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt.h` | `OS_MGMT_ID_*`, `os_mgmt_err_code_t` | same |
| S8 | `include/zephyr/mgmt/mcumgr/smp/smp.h` | `smp_mcumgr_version_t` | same |
| S9 | `subsys/mgmt/mcumgr/transport/include/mgmt/mcumgr/transport/smp_internal.h` | `struct smp_hdr` bitfield order | same |
| S10 | `subsys/mgmt/mcumgr/grp/img_mgmt/src/img_mgmt.c` | Server-side upload handler and response construction; also the erase and slot-info handlers, and `img_mgmt_translate_error_code()` | same (image handlers verified 2026-09-05) |
| S11 | `subsys/mgmt/mcumgr/grp/img_mgmt/src/zephyr_img_mgmt.c` | `img_mgmt_upload_inspect()` — offset/resume/validation rules | same |
| S13 | `subsys/mgmt/mcumgr/grp/os_mgmt/src/os_mgmt.c` | OS-group server handlers; echo, reset and mcumgr-params decoding and the handler registration table | same (verified 2026-09-05, and against tags `v3.5.0` and `v3.7.0`) |
| S12 | `docs/design.md` | MCUboot image format, TLVs, slots, swap types, trailer | `mcu-tools/mcuboot@main` |
| S14 | `subsys/mgmt/mcumgr/grp/img_mgmt/src/img_mgmt_state.c` | Image-state read and write handlers; the flag encoding and the set-state decode | `zephyrproject-rtos/zephyr@main` (verified 2026-09-05) |
| S15 | `subsys/mgmt/mcumgr/grp/img_mgmt/src/img_mgmt_util.c` | `img_mgmt_ver_str()` -- the version string a device actually reports | same |
| S16 | `subsys/mgmt/mcumgr/grp/img_mgmt/include/mgmt/mcumgr/grp/img_mgmt/img_mgmt_priv.h` | `IMAGE_SHA_LEN` and `IMAGE_TLV_SHA` | same |
| S17 | `subsys/mgmt/mcumgr/smp/src/smp.c` | `smp_add_cmd_err()`, `smp_build_err_rsp()` and the v1 error translation path | same |
| S18 | `boot/bootutil/include/bootutil/image.h` | `struct image_header`, `struct image_tlv{,_info}`, `IMAGE_MAGIC`, `IMAGE_F_*`, the TLV type numbers | `mcu-tools/mcuboot@main` (verified 2026-09-05) |
| S19 | `boot/bootutil/src/tlv.c` | `bootutil_tlv_iter_begin()`/`_next()` -- the authoritative TLV area layout and bounds checks | same |
| S20 | `boot/bootutil/src/image_validate.c` | `allowed_unprot_tlvs` -- which TLVs may live in the unprotected area | same |

Reference-only (behavioural comparison, **not** a source of protocol truth, and
never a source of copied code): `zephyrproject-rtos/mcumgr-client` (Go),
`nrfconnect/Android-nRF-Connect-Device-Manager`, `apache/mynewt-mcumgr`.

### Layer attribution

Every fact below is tagged with the layer it belongs to, so that it lands in the
right component:

* **[SMP]** — transport-independent framing/correlation. Lives in `src/smp/`.
* **[MGMT]** — management-group semantics. Lives in `src/groups/`.
* **[XPORT]** — transport-specific. Lives in `transports/`, never in the core.
* **[BOOT]** — MCUboot firmware/update semantics. Lives in `src/dfu/` + `src/image/`.

---

## 2. [SMP] Frame format

8-byte header (`MGMT_HDR_SIZE == 8`, S5), multi-byte fields **big-endian** (S1).

```
 byte 0   byte 1   byte 2   byte 3   byte 4   byte 5   byte 6   byte 7
+--------+--------+--------+--------+--------+--------+--------+--------+
|RRRVVOOO| flags  |    data length  |     group id    |  seq   |  cmd   |
+--------+--------+--------+--------+--------+--------+--------+--------+
```

Byte 0 bit layout, MSB→LSB (S1 diagram, confirmed by `struct smp_hdr` in S9):

| bits | field     | notes |
| ---- | --------- | ----- |
| 7..5 | `res`     | reserved, **must be 0** |
| 4..3 | `version` | `0b00` = SMP v1 (legacy), `0b01` = SMP v2 (group-scoped errors). `0b10`/`0b11` reserved. |
| 2..0 | `op`      | `mcumgr_op_t` |

| field | offset | size | notes |
| ----- | ------ | ---- | ----- |
| `flags` | 1 | 1 | no flags defined; **must be 0** |
| `length` | 2 | 2 | BE, length of payload **excluding** the 8-byte header |
| `group` | 4 | 2 | BE, `mcumgr_group_t` |
| `seq` | 6 | 1 | 8-bit, wraps; response must echo the request's value |
| `command` | 7 | 1 | command ID within group |

**Operations** (S5, `mcumgr_op_t`): `0` read, `1` read-response, `2` write,
`3` write-response.

**Endianness caveat (S1, verbatim):** *"The original specification states that SMP
should support receiving both the Little-endian and Big-endian frames but in
reality the MCUmgr library is hardcoded to always treat Network side as
Big-endian."* → smply encodes and decodes **big-endian only**. No heuristic
endian detection.

**Payload encoding:** CBOR for all groups `< 64`. Groups `≥ 64` may define their
own encoding — out of scope.

**Message boundary determination:** total message size is `8 + length`. Since the
header is fixed-size and the length field is in the first fragment, a byte stream
can always be resynchronised on message boundaries without transport framing
(S1, S2). This is what makes reassembly a *core* responsibility (ADR-0006).

### Management group IDs (S1, S5)

`0` OS · `1` Image · `2` Stat · `3` Settings · `4` Log (unused) · `5` Crash
(unused) · `6` Split (unused) · `7` Run (unused) · `8` FS · `9` Shell ·
`10` Enum · `11` Transport · `63` Zephyr basic · `64+` user-defined.

smply implements groups **0** and **1** only; the rest are representable as
opaque group IDs.

---

## 3. [SMP] Error reporting: v1 vs v2

This is the single most important version dependency.

**SMP v1** (`version = 0b00`) — error is a *flat, signed* `rc` in the response map:

```
{ "rc": (int), "rsn": (str, optional) }
```

`rc` is `mcumgr_err_t` (S5). Absent or `0` ⇒ success.

**SMP v2** (`version = 0b01`) — error is *group-scoped*:

```
{ "err": { "group": (uint), "rc": (uint) } }
```

Here `group` is a `mcumgr_group_t` and `rc` is that group's own error enum
(e.g. `img_mgmt_err_code_t`, S6). **`rc` values from different groups are not
comparable.**

**Critical (S1, verbatim):** *"For SMP version 2, errors relating to SMP itself
that are not group specific will still be returned as `rc` errors, SMP version 2
clients must therefore be able to handle both types of errors."*

⇒ smply always decodes **both** shapes on every response, regardless of the
version it requested. See `Error`/`MgmtError` in [`api.md`](api.md).

**Success shape:** an empty CBOR map. `rc`/`err` only appear on error. A response
that carries neither is a success.

**Version negotiation:** none exists. There is no capability query for SMP
version support. A v1-only server receiving a v2 request replies
`MGMT_ERR_UNSUPPORTED_TOO_NEW` (S5) — as a *v1* `rc`.
**Decision:** smply defaults to **SMP v1** requests (universally supported) and
exposes an opt-in v2 mode. Rationale in [ADR-0010](decisions/ADR-0010-request-correlation.md).

### `mcumgr_err_t` (S5) — SMP-level / v1 errors

| val | name | val | name |
| --- | ---- | --- | ---- |
| 0 | `EOK` | 8 | `ENOTSUP` |
| 1 | `EUNKNOWN` | 9 | `ECORRUPT` |
| 2 | `ENOMEM` | 10 | `EBUSY` |
| 3 | `EINVAL` | 11 | `EACCESSDENIED` |
| 4 | `ETIMEOUT` | 12 | `UNSUPPORTED_TOO_OLD` |
| 5 | `ENOENT` | 13 | `UNSUPPORTED_TOO_NEW` |
| 6 | `EBADSTATE` | 14 | `BRIDGED_CONNECTION_UNAVAILABLE` |
| 7 | `EMSGSIZE` | 256 | `EPERUSER` (user range base) |

### `img_mgmt_err_code_t` (S6) — group 1, SMP v2

`0` OK · `1` UNKNOWN · `2` FLASH_CONFIG_QUERY_FAIL · `3` NO_IMAGE ·
`4` NO_TLVS · `5` INVALID_TLV · `6` TLV_MULTIPLE_HASHES_FOUND ·
`7` TLV_INVALID_SIZE · `8` HASH_NOT_FOUND · `9` NO_FREE_SLOT ·
`10` FLASH_OPEN_FAILED · `11` FLASH_READ_FAILED · `12` FLASH_WRITE_FAILED ·
`13` FLASH_ERASE_FAILED · `14` INVALID_SLOT · `15` NO_FREE_MEMORY ·
`16` FLASH_CONTEXT_ALREADY_SET · `17` FLASH_CONTEXT_NOT_SET ·
`18` FLASH_AREA_DEVICE_NULL · `19` INVALID_PAGE_OFFSET · `20` INVALID_OFFSET ·
`21` INVALID_LENGTH · `22` INVALID_IMAGE_HEADER ·
`23` INVALID_IMAGE_HEADER_MAGIC · `24` INVALID_HASH ·
`25` INVALID_FLASH_ADDRESS · `26` VERSION_GET_FAILED ·
`27` CURRENT_VERSION_IS_NEWER · `28` IMAGE_ALREADY_PENDING ·
`29` INVALID_IMAGE_VECTOR_TABLE · `30` INVALID_IMAGE_TOO_LARGE ·
`31` INVALID_IMAGE_DATA_OVERRUN · `32` IMAGE_CONFIRMATION_DENIED ·
`33` IMAGE_SETTING_TEST_TO_ACTIVE_DENIED · `34` ACTIVE_SLOT_NOT_KNOWN

> ⚠ **Version dependency.** This enum is append-only but has grown across Zephyr
> releases. smply must map unknown values to a numeric-preserving "unknown
> group error" rather than failing to decode.

### `os_mgmt_err_code_t` (S7) — group 0, SMP v2

`0` OK · `1` UNKNOWN · `2` INVALID_FORMAT · `3` QUERY_YIELDS_NO_ANSWER ·
`4` RTC_NOT_SET · `5` RTC_COMMAND_FAILED · `6` QUERY_RESPONSE_VALUE_NOT_VALID ·
`7` HEAP_STATS_FETCH_FAILED

---

## 4. [SMP] Sequence numbers

* 8-bit, incremented by one per request (S1).
* The response `seq` **must** equal the request `seq` (S1).
* No uniqueness guarantee beyond 256 outstanding requests; wrap-around is
  expected and normal.
* **Not specified:** what a server does with a duplicate `seq`, or how a client
  should treat a response whose `seq` matches but whose `group`/`command`/`op`
  does not. smply treats a `(seq, group, command, op)` mismatch as an
  `UnexpectedResponse` and does **not** complete the pending request
  (see [ADR-0010](decisions/ADR-0010-request-correlation.md)).
* Late responses to timed-out or cancelled requests are unavoidable. smply
  keeps a bounded "retired sequence" set so late arrivals are discarded silently
  instead of being mis-attributed to a *later* request that reused the number.

---

## 5. [MGMT] OS management group (group 0)

Commands (S3, S7): `0` echo · `1` console echo control (unimplemented in Zephyr)
· `2` taskstat · `3` mpstat · `4` datetime · `5` **reset** · `6` **mcumgr params**
· `7` OS/app info · `8` bootloader info.

### Echo — op `0` (read) **or** `2` (write), group `0`, cmd `0` (S3, S13)

Request: `{ "d": (str) }`. Response: `{ "r": (str) }`, echoing `d` back
verbatim. The response op is `1` for a read request and `3` for a write.

The handler table registers echo under *both* the read and the write slot
(`[OS_MGMT_ID_ECHO] = { os_mgmt_echo, os_mgmt_echo }`, S13), which is why either
op is legal. Reset, by contrast, is registered write-only
(`{ NULL, os_mgmt_reset }`) and mcumgr params read-only
(`{ os_mgmt_mcumgr_params, NULL }`), so using the wrong op on those yields
`MGMT_ERR_ENOTSUP` rather than an answer.

The server places no length limit of its own on `d`; the bound is the SMP buffer
(`buf_size`, §8). A string that does not fit yields `MGMT_ERR_EMSGSIZE` from the
response encoder.

### System reset — op `2` (write), group `0`, cmd `5` (S3)

Request: empty CBOR map, or

```
{ "force": (int, opt), "boot_mode": (uint, opt) }
```

Response: op `3`, empty map on success.

Verified semantics:

* *"The device should issue response before resetting so that the SMP client
  could receive information that the command has been accepted."* ⇒ a reset
  response is **acceptance, not completion**. The link drops afterwards.
* A registered `CONFIG_MCUMGR_GRP_OS_RESET_HOOK` callback may **reject** the
  reset with an error.
* If a reset attempt returns `MGMT_ERR_EBUSY`, the client may retry with
  `"force" > 0`.
* **`"force"` is a CBOR boolean on the wire, not an integer** — see A15. The
  documentation (S3) says `(int)` and "force reset if value > 0"; the server
  (S13) decodes it with `zcbor_bool_decode`, at `main`, `v3.7.0` and `v3.5.0`
  alike. smply sends a boolean.
* The request body is parsed **only** when `CONFIG_MCUMGR_GRP_OS_RESET_HOOK` is
  enabled (S13): the whole decode block sits inside that guard. Without it,
  `"force"` is not read at all and every reset behaves as unforced.
* `"boot_mode"` requires `CONFIG_MCUMGR_GRP_OS_RESET_BOOT_MODE`. smply does not
  use it in the default DFU flow.

> ⚠ **Implementation-defined:** the delay between the response and the actual
> reset. smply must not assume the response implies the device is already down;
> it waits for a transport disconnect **or** a configurable grace timeout.
> Losing the response entirely (device resets first) is also possible — see
> §9 recovery.

### MCUmgr parameters — op `0` (read), group `0`, cmd `6` (S3)

Response: `{ "buf_size": (uint), "buf_count": (uint) }`.

`buf_size` is *"Single SMP buffer size, this includes SMP header and CBOR
payload"*. This is the **whole-SMP-message** budget of the server and is the
authoritative input for upload chunk sizing. `buf_count` is the number of such
buffers (relevant to how many requests may be in flight).

> ⚠ Optional command. Older/minimal servers return `MGMT_ERR_ENOTSUP`. smply
> must fall back to a conservative default (see §8).

---

## 6. [MGMT] Image management group (group 1)

Commands (S4, S6): `0` state of images · `1` image upload · `2`–`4` reserved,
unsupported in Zephyr · `5` image erase · `6` slot info.

### Slots and images (S4)

An *image* is a pair of *slots*: slot `0` = primary (running), slot `1` =
secondary (upload target). Zephyr supports at most two images today
(image 1 → `image-0`/`image-1`, image 2 → `image-2`/`image-3`).

### Get image state — op `0`, group `1`, cmd `0`

Request: empty map. Response:

```
{
  "images": [ {
      "image":     (uint, opt)   // absent iff device supports only one image
      "slot":      (uint)        // 0 = primary, 1 = secondary
      "version":   (str)         // imgtool version string
      "hash":      (bstr, opt*)  // MCUboot IMAGE_TLV_SHA; 32 or 64 bytes
      "bootable":  (bool, opt)   // absent == false, but usually sent
      "pending":   (bool, opt)
      "confirmed": (bool, opt)
      "active":    (bool, opt)
      "permanent": (bool, opt)
  } ... ],
  "splitStatus": (int, opt)      // unused by Zephyr
}
```

Verified semantics:

* **Absent boolean == `false` -- but a false flag is usually *sent*.** S4 says
  the flags are omitted when false. In the implementation (S14) that holds only
  under `CONFIG_MCUMGR_GRP_IMG_FRUGAL_LIST`; the default build encodes every
  flag explicitly, `splitStatus` included. So a client must treat *both* an
  absent field and a present `false` as false, and must never read "the key is
  there" as "the flag is set".
* **`"slot"` and `"version"` are the only non-optional fields**, and the server
  always writes both (S14).
* **`"version"` may be the literal `"<???>"`.** S14 substitutes it when
  `img_mgmt_ver_str()` fails, so a version string is not guaranteed to parse.
  Keep the raw string; parse separately and tolerate failure.
* **The version string is dotted, not `+`-separated.** `img_mgmt_ver_str()`
  (S15) formats `"major.minor.revision"` and appends `".build"` only when the
  build number is non-zero. imgtool's `"1.2.3+4"` is an *input* spelling, not
  what a device reports. Longest possible string: `"255.255.65535.4294967295"`,
  24 bytes with the terminator.
* **A slot whose `img_mgmt_read_info()` fails is skipped silently** (S14), which
  is the mechanism behind the "only valid images" rule below.
* **`"hash"` is not the hash of the file.** S4, verbatim: *"SHA256 hash of the
  image header and body. Note that this will not be the same as the SHA256 of
  the whole file, it is the field in the MCUboot TLV section…"* i.e.
  `IMAGE_TLV_SHA256` (S12). **This is a different value from the `"sha"` field
  used in upload** (§7). Confusing the two is the classic MCUmgr client bug.
* *"A response will only contain information for valid images, if an image can
  not be identified as valid it is simply skipped."* ⇒ an empty/short `images`
  array is normal after erasing a slot, and is **not** an error.
* `"image"` may be absent on single-image devices; smply defaults it to `0`.
* `"hash"` is optional only in MCUboot serial-recovery configurations
  (`CONFIG_BOOT_SERIAL_IMG_GRP_HASH`); *"MCUmgr in applications must support
  sending hashes."*
* **`"hash"` is `IMAGE_SHA_LEN` bytes, which is 32 *or* 64.** S16 defines it as
  64 under `CONFIG_MCUBOOT_BOOTLOADER_USES_SHA512` (with `IMAGE_TLV_SHA` then
  being `IMAGE_TLV_SHA512`) and 32 otherwise. Only those two; there is no
  SHA-384 variant of the symbol, and Zephyr's `modules/Kconfig.mcuboot` offers
  only the SHA-512 option. A client that assumes 32 bytes cannot talk to a
  SHA-512 device at all, which is why smply carries the length with the value.

### Set image state — op `2`, group `1`, cmd `0`

Request:

```
{ "hash": (bstr, opt), "confirm": (bool) }
```

* `confirm == false`/absent + `hash` ⇒ mark that image **for test** (trial boot;
  reverts on next reset unless confirmed).
* `confirm == true` ⇒ **confirm**. `hash` is optional here: *"the currently
  running application will be assumed as target for confirmation."*
* Response has the same shape as get-state (the refreshed image list).
* **A test with no hash is refused**, with `IMG_MGMT_ERR_INVALID_HASH` (S14):
  the server has no way to tell which image is meant. A hash of any length other
  than `IMAGE_SHA_LEN` gets the same code, before any lookup happens.
* Unlike reset's `force` (§9, A15), the decode result **is** checked here: a
  wrong-typed field fails the command with `MGMT_ERR_EINVAL` rather than being
  ignored.
* The write handler is registered as `NULL` under
  `CONFIG_MCUBOOT_BOOTLOADER_MODE_DIRECT_XIP`, `..._RAM_LOAD` and
  `..._FIRMWARE_UPDATER` (S14), so set-state answers `ENOTSUP` on those builds.

> ⚠ Setting `confirm: true` on a *pending, not-yet-booted* image makes the swap
> permanent without a trial. That is a legitimate but less safe mode; smply
> exposes it as an explicit non-default `UpdateMode`.

### Image upload — request op `2`, group `1`, cmd `1`

```
{
  "image":   (uint, opt)   // image number; only when off == 0; default 0
  "len":     (uint, opt)   // total image size; REQUIRED when off == 0
  "off":     (uint)        // REQUIRED always
  "sha":     (bstr, opt)   // SHA-256 of the WHOLE FILE; only when off == 0
  "data":    (bstr)        // chunk payload
  "upgrade": (bool, opt)   // only when off == 0
}
```

Response (op `3`):

```
{ "off": (uint, opt), "match": (bool, opt) }
```

**Verified server behaviour** (S10, S11 — these are the rules smply's upload
state machine is built on):

1. **`"sha"` is the SHA-256 of the entire file being uploaded**, not the MCUboot
   TLV hash. It tags the upload session and enables resume + final verification.
   Omitting it disables both.
2. **The first chunk must contain at least `sizeof(struct image_header)` = 32
   bytes**, or the server returns `INVALID_IMAGE_HEADER` (S11). Hard constraint
   on minimum chunk size.
3. **The server validates the MCUboot magic `0x96F3B83D` in the first chunk**
   and returns `INVALID_IMAGE_HEADER_MAGIC` otherwise (S11). Uploading a raw
   (unsigned/unwrapped) binary always fails.
4. **`"len"` must be present when `off == 0`**, else `INVALID_LENGTH` (S11).
5. **Offset mismatch is not an error.** S10/S11, verbatim: *"Request specifies
   incorrect offset. Respond with a success code and the correct offset."*
   The server drops the data and returns a **success** response carrying the
   `"off"` it expects. ⇒ **The server-returned `off` is authoritative. Never
   compute `next_off = off + sent`.**
6. **Resume**: when `off == 0` and the supplied `"sha"` matches the in-progress
   session's hash, the server returns success with the current offset and does
   not restart (S11).
7. **Restart from zero can be requested by the server at any time.** S4,
   verbatim: *"It is possible that a server will respond to an upload with `off`
   of 0 … a client must re-send all the required and optional fields that it
   sent in the original first packet so that the upload state can be re-created
   by the server. If the original fields are not included, the upload will be
   unable to continue."* ⇒ **whenever the response `off` is 0, the next request
   must be a full first-packet** (`len`, `sha`, `image`, `upgrade`).
8. `off + len(data) > len` ⇒ `INVALID_IMAGE_DATA_OVERRUN` (S11).
9. `"match"` appears **only in the response to the final chunk** and only when
   `CONFIG_IMG_ENABLE_IMAGE_CHECK` is enabled (S10). `match == false` means the
   flashed bytes do not hash to the supplied `"sha"` ⇒ the upload must be
   treated as failed.
10. `"off"` is only present in successful responses; on error it may be absent
    (S4).
11. `"upgrade": true` makes the server reject a non-newer version
    (`CURRENT_VERSION_IS_NEWER`). Comparison is major.minor.revision unless
    `CONFIG_MCUMGR_GRP_IMG_VERSION_CMP_USE_BUILD_NUMBER` is set — **version
    dependent**; smply does not set `upgrade` by default.
12. Erase of the target slot may happen implicitly on the first chunk and can be
    slow ⇒ the **first chunk needs a longer timeout** than subsequent ones.

### Image erase — op `2`, group `1`, cmd `5`

Request `{ "slot": (uint, opt) }`. **Synchronous and potentially very slow**
(S4) ⇒ needs its own long timeout.

* The default is not the constant `1`: the server computes *the slot opposite
  the active one of the active image* (S10), which is slot 1 in the
  ordinary case but need not be. Omitting the key is therefore better than
  sending `1`.
* A slot holding an image already marked for the next boot is refused with
  `IMG_MGMT_ERR_NO_FREE_SLOT`, which a v1 client sees as `MGMT_ERR_EBADSTATE`
  (see A16) -- that translation is where S4's `EBADSTATE` note comes from.
* Success is the **empty map**, or `{"rc": 0}` when the server was built with
  `CONFIG_MCUMGR_SMP_LEGACY_RC_BEHAVIOUR`. Zero is success in both shapes.

### Slot info — op `0`, group `1`, cmd `6`

```
{ "images": [ { "image": (uint),
                "slots": [ { "slot": (uint), "size": (uint),
                             "upload_image_id": (uint, opt) } ],
                "max_image_size": (uint, opt) } ] }
```

Requires `CONFIG_MCUMGR_GRP_IMG_SLOT_INFO`; `upload_image_id` requires
`CONFIG_MCUMGR_GRP_IMG_DIRECT_UPLOAD`; `max_image_size` requires
`CONFIG_MCUMGR_GRP_IMG_TOO_LARGE_*`. **All optional** ⇒ treat as best-effort
pre-flight information only.

**Not in S4: a slot entry may carry its own `"rc"`.** When `flash_area_open()`
fails for a slot, the server writes `{"slot": N, "rc": (int)}` for it -- no
`"size"`, no `"upload_image_id"` (S10). The value is a Zephyr
`errno`, and it is *nested inside the slot map*, so it can never be confused
with the message-level `rc` a client reads for the command's own result. A
client that requires `"size"` will reject an otherwise perfectly good response
from a device with one unreadable flash area.

`upload_image_id` also means two different things: with
`CONFIG_MCUMGR_GRP_IMG_DIRECT_UPLOAD` it is the global slot index plus one and
is emitted for every slot; without it, it is the *image* number and is emitted
only for slots that are not the active one (S10).

---

## 7. [BOOT] MCUboot image and update semantics (S12, S18-S20)

### Image header — 32 bytes, **little-endian** (S18)

| off | size | field |
| --- | ---- | ----- |
| 0 | 4 | `ih_magic` = `0x96F3B83D` (`IMAGE_MAGIC`) |
| 4 | 4 | `ih_load_addr` |
| 8 | 2 | `ih_hdr_size` |
| 10 | 2 | `ih_protect_tlv_size` |
| 12 | 4 | `ih_img_size` (excludes header) |
| 16 | 4 | `ih_flags` |
| 20 | 1+1+2+4 | `ih_ver` = major, minor, revision(u16), build(u32) |
| 28 | 4 | `_pad1` |

`IMAGE_MAGIC_V1 = 0x96F3B83C` is the first image format and is not usable here;
recognising it separately turns "not an MCUboot image" into "an image from a
toolchain that is too old".

Flags of interest: `IMAGE_F_ENCRYPTED_AES128 = 0x04`,
`IMAGE_F_ENCRYPTED_AES256 = 0x08`. There is also a compression family
(`IMAGE_F_COMPRESSED_LZMA1 = 0x200`, `LZMA2 = 0x400`, `ARM_THUMB_FLT = 0x800`)
with its own `IMAGE_TLV_DECOMP_SHA` — out of scope, and a reason not to reject
unknown flag bits.

### TLV areas — the layout, from the scanner rather than the diagram (S19)

The trailer is **two contiguous areas**, and the rules below come from
`bootutil_tlv_iter_begin()`/`_next()`, which is what actually enforces them.
`struct image_tlv_info { u16 magic; u16 total; }` and
`struct image_tlv { u16 type; u16 len; }`, four bytes each, little-endian.

```
base = ih_hdr_size + ih_img_size
  [ image_tlv_info  magic = 0x6908 (PROT), total = ih_protect_tlv_size ]  optional
  [ entries ...                                                        ]
  [ image_tlv_info  magic = 0x6907 (INFO), total                       ]
  [ entries ...                                                        ]
prot_end = base + ih_protect_tlv_size
tlv_end  = base + ih_protect_tlv_size + <unprotected total>
```

* **`it_tlv_tot` includes the four-byte `image_tlv_info` header of its own
  area**, and `ih_protect_tlv_size` must equal the protected area's `it_tlv_tot`
  **exactly** — MCUboot refuses the image otherwise. A client that reads either
  as a payload size lands four bytes short on every signed image.
* **A protected magic with `ih_protect_tlv_size == 0`, or a non-zero
  `ih_protect_tlv_size` with no protected magic, is an error.** The two must
  agree.
* **The areas are walked as one run**, not separately: iteration starts at
  `base + 4` — inside the protected area when there is one — and when the cursor
  reaches `prot_end` it steps over the *unprotected* area's own four-byte header
  and carries on to `tlv_end`.
* **Every advance is `4 + it_len`, so it is ≥ 4 by construction.** A TLV scanner
  cannot spin on a zero-length entry: the entry header itself is always
  consumed. An iteration cap is still worth having, but it bounds *work*, not
  termination — do not document it as a loop guard.
* `it_len` is attacker-controlled: MCUboot checks the entry header fits, then
  that `it_len <= end - off - 4`, before using it for anything including the
  advance.

**The hash TLVs live in the unprotected area** (S20, `allowed_unprot_tlvs`):
`IMAGE_TLV_SHA256 = 0x10` (32 bytes), `IMAGE_TLV_SHA384 = 0x11` (48),
`IMAGE_TLV_SHA512 = 0x12` (64). Whichever is present is the value image-state
reports as `"hash"` (§6). It cannot be in the protected area, since it covers
the protected TLVs.

### Two different hashes — the key distinction

| Where | What it is | Length | Who computes it |
| ----- | ---------- | ------ | --------------- |
| upload `"sha"` | SHA-256 of the **entire file** as uploaded | always 32 | smply |
| image-state `"hash"` | `IMAGE_TLV_SHA`: SHA of **header + body** (+ protected TLVs) | `IMAGE_SHA_LEN`: 32, or 64 for a SHA-512 bootloader (§6, S16) | imgtool at signing time; read out of the file's TLVs or reported by the device |

The differing lengths are the second reason to keep them apart: smply gives them
different types (`Hash` and `ImageHash`), so passing one where the other belongs
does not compile rather than being caught in review.

### Swap types (S12)

`NONE` (1) · `TEST` (2) · `PERM` (3) · `REVERT` (4) · `FAIL` · `PANIC`.

Relevant DFU consequence: after `set-state(test)` + reset, MCUboot swaps and
boots the new image with `confirmed == false`. If the device resets again
**without** a confirm, MCUboot performs a `REVERT` back to the old image. This
is the rollback safety net that makes test-then-confirm the correct default.

### What smply must *not* do

Signature verification, encryption, dependency-TLV evaluation and swap
management are MCUboot's job. See
[ADR-0009](decisions/ADR-0009-mcuboot-boundary.md).

---

## 8. [XPORT] Transport specifics

### Bluetooth LE (S2)

* **Service UUID** `8D53DC1D-1DB7-4CD3-868B-8A527460AA84`
* **Characteristic UUID** `DA2E7828-FBCE-4E01-AE9E-261174997C48`
* Requests: **GATT Write Without Response**. Responses: **GATT Notification**.
* *"If an SMP request or response is too large to fit in a single GATT command,
  the sender fragments it across several packets. No additional framing is
  introduced… Since GATT guarantees ordered delivery of packets, the SMP header
  in the first fragment contains sufficient information for reassembly."*

⇒ BLE fragment size is `ATT_MTU - 3`. This is a **transport** concern and is
completely independent of the MCUmgr upload chunk size.

### UART / console (S2)

Base64 body, `0x06 0x09` initial marker, `0x04 0x14` continuation marker, `0x0A`
terminator, 2-byte BE total length prefix, CRC16 (poly `0x1021`, init `0`) over
the raw body. Zephyr imposes a 127-byte frame limit (124 payload).
Raw UART (`CONFIG_MCUMGR_TRANSPORT_RAW_UART`) sends binary SMP with no framing.

Not implemented in the initial scope; documented so the transport contract is
demonstrably general enough (see [ADR-0005](decisions/ADR-0005-transport-abstraction.md)).

### The three size limits — keep them distinct

| Limit | Owner | Source |
| ----- | ----- | ------ |
| **Transport fragment size** | transport | BLE `ATT_MTU-3`; UART frame limit |
| **Whole-SMP-message size** | server | OS `buf_size` (§5), else conservative default |
| **Upload chunk size** (`data` bstr length) | upload state machine | derived: `min(msg_budget) − header − CBOR overhead` |

`chunk = smp_message_budget − 8 (SMP header) − first_packet_cbor_overhead`,
where `smp_message_budget = min(server buf_size, transport max_message_size,
configured cap)`. The overhead must be computed for the **first** packet (which
carries `len`, `sha`, `image`) and applied to all chunks so the first packet
never overflows. Default budget when `buf_size` is unavailable: **256 bytes**
(the common Zephyr `CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE` default is 384; 256 is
safe). Minimum viable chunk is **32 bytes** (§6 rule 2).

---

## 9. Ambiguities, gaps and version dependencies

Recorded so future sessions do not rediscover them.

| # | Issue | smply's position |
| - | ----- | ---------------- |
| A1 | No SMP version negotiation exists. | Default to v1 requests; decode both error shapes always; v2 is opt-in. |
| A2 | Group error enums are append-only and grow per Zephyr release. | Never reject unknown `rc`; carry the numeric value through. |
| A3 | Reset response ↔ actual reboot delay is unspecified; the response may be lost. | Treat "reset accepted **or** transport disconnected" as the success condition, with a grace timeout. |
| A4 | Behaviour on duplicate/unexpected `seq` is unspecified. | Match on `(seq, group, command)`; mismatch ⇒ `UnexpectedResponse`, request stays pending until timeout. |
| A5 | Whether a server may send unsolicited notifications on the SMP characteristic. | Not used by groups 0/1. Unmatched responses are dropped and counted, never fatal. |
| A6 | `"match"` presence depends on `CONFIG_IMG_ENABLE_IMAGE_CHECK`. | Absence is not an error; presence with `false` **is**. |
| A7 | Implicit slot erase on first chunk has unbounded duration. | Separate, longer `first_chunk_timeout` (default 30 s vs 5 s). |
| A8 | `slot info` / `mcumgr params` are optional commands. | `ENOTSUP` is a normal outcome; fall back to defaults, never fail the update. |
| A9 | Single-image devices omit `"image"` in image-state entries. | Default to image `0`. |
| A10 | Whether the device accepts multiple in-flight requests. | Default **one** outstanding request. `buf_count` may later raise it; not in initial scope. |
| A11 | `upgrade` version comparison includes the build number only with a Kconfig option. | Do not set `upgrade` by default; expose it as an explicit option. |
| A12 | Erase (cmd 5) is synchronous and may take tens of seconds. | Dedicated long timeout; not part of the default DFU flow. |
| A13 | After a swap, whether the *new* image reports the same `"hash"` as the uploaded file's `IMAGE_TLV_SHA256`. | Verified true for non-encrypted images; for encrypted images it is **not** guaranteed. Encrypted-image DFU is out of scope — record as a known limitation. |
| A14 | MCUmgr allows several requests per packet in principle (S8 comment); Zephyr's SMP-over-BLE does not use this. | smply sends exactly one request per SMP message and expects one response per message. |
| A16 | **An image-group error code often does not survive the trip to a v1 client.** `smp_add_cmd_err()` (S17) always writes `err: {group, rc}` into the payload; but when the server has `CONFIG_MCUMGR_SMP_SUPPORT_ORIGINAL_PROTOCOL` and the request was SMP v1, `smp_handle_single_req()` translates the group code through `img_mgmt_translate_error_code()` (S10) and `smp_on_err()` then **discards the whole partial payload** and rebuilds it as flat `{"rc": …}`. The translation is lossy and many-to-one: `NO_FREE_SLOT`, `CURRENT_VERSION_IS_NEWER` and `IMAGE_ALREADY_PENDING` all become `EBADSTATE`; `HASH_NOT_FOUND`, `INVALID_TLV`, every flash failure and `INVALID_IMAGE_*` all become `EUNKNOWN`. With that Kconfig **off**, the same v1 client receives the `err` map untouched. | Decode both shapes on every response regardless of the version requested, which smply already does. Expose the group code when there is one (`image_error()`), and document that its absence is normal rather than a malformed reply. A caller that must distinguish `HASH_NOT_FOUND` from a generic failure needs SMP v2 -- another input to O2. |
| A15 | **The documentation and the implementation disagree on the type of reset's `"force"`.** S3 specifies `(int)` with "force reset if value > 0"; S13 decodes it with `zcbor_bool_decode`, which accepts only CBOR `true`/`false` and rejects any integer. Worse, the server *discards* the decode result (`(void)zcbor_map_decode_bulk(...)`, with a comment saying a core command should continue with defaults), so an integer `"force"` is **silently ignored** and the reset proceeds unforced — no error tells the client its intent was dropped. | Encode `"force"` as a CBOR **boolean**, matching the implementation, which is what a device actually enforces. Omit the key entirely when not forcing, so the unforced request is the empty map the specification shows. Same in `v3.5.0` and `v3.7.0`, so this is not a recent regression. |
