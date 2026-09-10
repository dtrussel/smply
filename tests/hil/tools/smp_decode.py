# SPDX-License-Identifier: Apache-2.0
"""Decode SMP traffic out of an HCI capture, and compare two clients' sequences.

This is the Tier B half of the cross-check: what each client actually put on the
air, rendered so that two clients' *operation sequences* can be compared while
their sequence numbers, fragmentation and timing are allowed to differ
(docs/testing.md section 6).

**The SMP decoding here is ours.** The header layout comes from
docs/protocol-notes.md section 2 and is the same layout `src/smp/codec.cpp`
encodes; the "no additional framing on BLE, reassemble from the header's length"
rule is section 8, quoted from the specification. The CBOR reader is written from
RFC 8949 because the reference server emits **indefinite-length** maps and arrays
(section 9, A18). Nothing in this file is inferred from a third-party client, and
no third-party client's code is used to interpret bytes -- ADR-0015 decision 3.
`tshark` is used only to undo *ATT* framing, which it does as a general-purpose
protocol analyser and which is not an SMP inference.

What it will not do: decide anything from a capture it cannot account for. Every
stream reports `bytes_in`, `bytes_consumed` and `trailing_bytes`, and they must
balance. A decoder that silently discards what it cannot parse makes two
sequences look identical, which is the one failure mode that would let this whole
phase report a false agreement.

  python tests/hil/tools/smp_decode.py --self-test
  python tests/hil/tools/smp_decode.py --capture smply.pcapng --out smply.ops.json
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

TSHARK = Path(r"C:\Program Files\Wireshark\tshark.exe")

# ATT opcodes. Requests go out as Write Without Response and responses arrive as
# notifications (protocol-notes.md section 8), so the ATT opcode *is* the
# direction. Never take it from an H4 direction bit or an ACL role field: those
# describe the HCI transport and the link, not who wrote the characteristic.
ATT_WRITE_COMMAND = 0x52
ATT_HANDLE_VALUE_NOTIFICATION = 0x1B

HEADER_SIZE = 8
MAX_PLAUSIBLE_LENGTH = 4096

GROUP_NAMES = {0: "os", 1: "image", 2: "stat", 3: "settings", 8: "fs", 9: "shell",
               10: "enumeration"}
COMMAND_NAMES = {
    (0, 0): "echo", (0, 5): "reset", (0, 6): "params",
    (1, 0): "state", (1, 1): "upload", (1, 5): "erase", (1, 6): "slot-info",
}
OP_NAMES = {0: "read", 1: "read-rsp", 2: "write", 3: "write-rsp"}


# --- CBOR ---------------------------------------------------------------------
#
# Enough of RFC 8949 for groups 0 and 1, definite and indefinite. Deliberately
# not `cbor2`, which is present in the bench venv: a decoder we own is one fewer
# thing to argue about when a divergence is being traced, and it is unit-testable
# off the bench.

class CborError(ValueError):
    pass


def _read_head(data: bytes, off: int) -> tuple[int, int, int]:
    """Returns (major, argument, next offset). `argument` is -1 for indefinite."""
    if off >= len(data):
        raise CborError("truncated head")
    initial = data[off]
    major, info = initial >> 5, initial & 0x1F
    off += 1
    if info < 24:
        return major, info, off
    if info == 24:
        if off >= len(data):
            raise CborError("truncated 1-byte argument")
        return major, data[off], off + 1
    if info in (25, 26, 27):
        width = 2 << (info - 25)
        if off + width > len(data):
            raise CborError(f"truncated {width}-byte argument")
        return major, int.from_bytes(data[off:off + width], "big"), off + width
    if info == 31:
        return major, -1, off
    raise CborError(f"reserved additional information {info}")


def _read_value(data: bytes, off: int):
    major, arg, off = _read_head(data, off)

    if major == 0:
        return arg, off
    if major == 1:
        return -1 - arg, off
    if major in (2, 3):
        if arg == -1:
            # Indefinite-length string: a run of definite-length chunks.
            chunks = []
            while True:
                if off < len(data) and data[off] == 0xFF:
                    off += 1
                    break
                chunk, off = _read_value(data, off)
                chunks.append(chunk if major == 2 else chunk.encode())
            joined = b"".join(chunks)
            return joined if major == 2 else joined.decode("utf-8", "replace"), off
        if off + arg > len(data):
            raise CborError("truncated string")
        raw, off = data[off:off + arg], off + arg
        return (raw if major == 2 else raw.decode("utf-8", "replace")), off
    if major == 4:
        items = []
        if arg == -1:
            while True:
                if off >= len(data):
                    raise CborError("unterminated array")
                if data[off] == 0xFF:
                    off += 1
                    break
                item, off = _read_value(data, off)
                items.append(item)
        else:
            for _ in range(arg):
                item, off = _read_value(data, off)
                items.append(item)
        return items, off
    if major == 5:
        out = {}
        if arg == -1:
            while True:
                if off >= len(data):
                    raise CborError("unterminated map")
                if data[off] == 0xFF:
                    off += 1
                    break
                key, off = _read_value(data, off)
                value, off = _read_value(data, off)
                out[key] = value
        else:
            for _ in range(arg):
                key, off = _read_value(data, off)
                value, off = _read_value(data, off)
                out[key] = value
        return out, off
    if major == 7:
        if arg == 20:
            return False, off
        if arg == 21:
            return True, off
        if arg == 22:
            return None, off
        if arg == 23:
            return "undefined", off
        if arg == -1:
            raise CborError("unexpected break")
        # Floats are not used by these groups; report rather than invent a value.
        return f"simple({arg})", off
    raise CborError(f"unsupported major type {major}")


def cbor_load(data: bytes):
    """Decodes one CBOR item. Raises `CborError` rather than guessing."""
    value, off = _read_value(data, 0)
    return value, off


# --- the SMP header and reassembly --------------------------------------------

def parse_header(buf: bytes) -> dict | None:
    """Decodes an 8-byte SMP header, or returns None if it cannot be one.

    The plausibility rules are the ones `src/smp/codec.cpp` enforces on decode:
    the top three bits of byte 0 are reserved and must be clear, and the version
    field's `0b10`/`0b11` are reserved. `flags` has no defined bits, so a set
    flag is implausible *here* even though the core carries unknown flags
    through -- this function's job is to recognise a header inside an
    unlabelled byte stream, which is a stricter question than decoding one that
    is known to be a header.
    """
    if len(buf) < HEADER_SIZE:
        return None
    byte0 = buf[0]
    if byte0 & 0xE0:
        return None
    version, op = (byte0 >> 3) & 0x03, byte0 & 0x07
    if version > 1 or op > 3:
        return None
    if buf[1] != 0:
        return None
    length = int.from_bytes(buf[2:4], "big")
    if HEADER_SIZE + length > MAX_PLAUSIBLE_LENGTH:
        return None
    return {"op": op, "version": version, "flags": buf[1], "length": length,
            "group": int.from_bytes(buf[4:6], "big"), "seq": buf[6], "command": buf[7]}


class Reassembler:
    """Turns one direction of one characteristic into whole SMP messages.

    There is no transport framing: the header's `length` says where the message
    ends and the next begins (protocol-notes.md section 8). So this is a byte
    stream, and the only two things that can go wrong are worth reporting rather
    than hiding:

    * **A start that is not a header.** Expected when attaching to a capture
      already in progress, which is the normal case with a BTVS session someone
      else started. It scans forward to the first plausible header and sets
      `truncated_start`.
    * **A tail that is not a whole message.** The capture stopped mid-message.
      Counted in `trailing_bytes`.

    `bytes_in == bytes_consumed + trailing_bytes` must hold at the end. That
    identity is the reason to trust anything below it.
    """

    def __init__(self) -> None:
        self.buf = bytearray()
        self.messages: list[dict] = []
        self.bytes_in = 0
        self.bytes_consumed = 0
        self.truncated_start = False
        self.resyncs = 0
        self._fragments = 0
        self._started = False

    def feed(self, payload: bytes, *, t_ms: float = 0.0) -> None:
        self.bytes_in += len(payload)
        self.buf += payload
        self._fragments += 1
        self._drain(t_ms)

    def _drain(self, t_ms: float) -> None:
        while True:
            header = parse_header(self.buf)
            if header is None:
                if len(self.buf) < HEADER_SIZE:
                    return
                # Not a header, and long enough to know. Drop one byte and look
                # again: either we joined the stream mid-message, or a message
                # was lost and the stream needs resynchronising. Both are
                # recorded; neither is silently absorbed.
                if not self._started:
                    self.truncated_start = True
                else:
                    self.resyncs += 1
                del self.buf[0]
                self.bytes_consumed += 1
                continue
            total = HEADER_SIZE + header["length"]
            if len(self.buf) < total:
                return
            body = bytes(self.buf[HEADER_SIZE:total])
            del self.buf[:total]
            self.bytes_consumed += total
            self._started = True
            self.messages.append({**header, "body": body, "t_ms": round(t_ms, 1),
                                  "fragments": self._fragments})
            self._fragments = 0

    @property
    def trailing_bytes(self) -> int:
        return len(self.buf)

    def accounts(self) -> bool:
        return self.bytes_in == self.bytes_consumed + self.trailing_bytes

    def summary(self) -> dict:
        return {"messages": len(self.messages), "bytes_in": self.bytes_in,
                "bytes_consumed": self.bytes_consumed,
                "trailing_bytes": self.trailing_bytes,
                "truncated_start": self.truncated_start, "resyncs": self.resyncs,
                "accounts": self.accounts()}


# --- rendering ----------------------------------------------------------------

def _bucket(n: int) -> str:
    """A coarse size class. Chunk sizes legitimately differ between clients."""
    for edge in (32, 64, 128, 256, 512, 1024, 2048):
        if n <= edge:
            return f"<={edge}"
    return ">2048"


def render(message: dict) -> dict:
    """One comparable operation record.

    The `projection` is what Tier B compares: the small set of facts that must
    match between two clients doing the same thing. Everything else -- seq,
    fragment count, timing, exact chunk size -- is recorded beside it and
    explicitly not compared.
    """
    group, command = message["group"], message["command"]
    name = COMMAND_NAMES.get((group, command), f"cmd{command}")
    record = {
        "dir": "req" if message["op"] in (0, 2) else "rsp",
        "op": message["op"], "op_name": OP_NAMES.get(message["op"], "?"),
        "version": message["version"],
        "group": group, "group_name": GROUP_NAMES.get(group, str(group)),
        "command": command, "name": name,
        # Recorded, never compared -- docs/testing.md section 6 grants exactly
        # these three tolerances, plus chunk size.
        "seq": message["seq"], "fragments": message["fragments"],
        "t_ms": message["t_ms"], "body_len": len(message["body"]),
    }
    try:
        body, consumed = cbor_load(message["body"]) if message["body"] else ({}, 0)
        record["body_trailing"] = len(message["body"]) - consumed
    except CborError as error:
        record["body_error"] = str(error)
        record["projection"] = {"undecodable": True}
        return record
    if not isinstance(body, dict):
        record["body_error"] = f"body is {type(body).__name__}, not a map"
        record["projection"] = {"undecodable": True}
        return record

    record["keys"] = sorted(str(k) for k in body)
    projection: dict = {}
    if (group, command) == (1, 1):  # image upload
        off = body.get("off")
        projection = {"off_class": "zero" if off == 0 else "nonzero",
                      "has_sha": "sha" in body, "has_len": "len" in body,
                      "has_upgrade": "upgrade" in body}
        if "data" in body and isinstance(body["data"], (bytes, bytearray)):
            projection["data_len_bucket"] = _bucket(len(body["data"]))
        if "rc" in body:
            projection["rc"] = body["rc"]
        record["off"] = off
        record["len"] = body.get("len")
    elif (group, command) == (1, 0):  # image state
        if record["dir"] == "req":
            projection = {"confirm": bool(body.get("confirm", False)),
                          "has_hash": "hash" in body}
        else:
            images = body.get("images") or []
            projection = {"slot_count": len(images) if isinstance(images, list) else None,
                          "flags": [
                              {"slot": s.get("slot"), "active": bool(s.get("active", False)),
                               "pending": bool(s.get("pending", False)),
                               "confirmed": bool(s.get("confirmed", False)),
                               "permanent": bool(s.get("permanent", False))}
                              for s in images if isinstance(s, dict)]}
    elif (group, command) == (0, 5):  # reset
        projection = {"has_force": "force" in body}
    elif (group, command) == (0, 6):  # mcumgr params
        projection = {k: body.get(k) for k in ("buf_size", "buf_count") if k in body}
    elif (group, command) == (0, 0):  # echo
        projection = {"has_d": "d" in body}
    else:
        projection = {"keys": record["keys"]}

    # An error is part of the comparable projection whatever the command: the
    # two shapes are what A16 and O2 are about.
    if "rc" in body and (group, command) != (1, 1):
        projection["rc"] = body["rc"]
    if isinstance(body.get("err"), dict):
        projection["err"] = {"group": body["err"].get("group"), "rc": body["err"].get("rc")}
    record["projection"] = projection
    return record


def collapse_uploads(ops: list[dict]) -> list[dict]:
    """Folds a run of upload request/response pairs into one `upload-run`.

    A client's chunk size is its own business -- smply derives it from the
    device's `buf_size`, another client may use a fixed number -- so comparing
    the *count* of upload requests would diverge on every run and mean nothing.
    What is comparable is the shape of the run: that it started at zero, that
    the offsets advanced, that it ended at the image size, and how many times it
    restarted.
    """
    out: list[dict] = []
    i = 0
    while i < len(ops):
        if ops[i]["group"] == 1 and ops[i]["command"] == 1:
            j = i
            offsets, restarts, requests, responses = [], 0, 0, 0
            first = ops[i]
            final_off = None
            while j < len(ops) and ops[j]["group"] == 1 and ops[j]["command"] == 1:
                if ops[j]["dir"] == "req":
                    requests += 1
                    off = ops[j].get("off")
                    if isinstance(off, int):
                        if offsets and off == 0:
                            restarts += 1
                        offsets.append(off)
                else:
                    responses += 1
                    off = ops[j].get("off")
                    if isinstance(off, int):
                        final_off = off
                j += 1
            monotonic = all(b >= a for a, b in zip(offsets, offsets[1:])) if offsets else None
            out.append({
                "dir": "req", "group": 1, "group_name": "image", "command": 1,
                "name": "upload-run", "version": first["version"],
                "requests": requests, "responses": responses,   # recorded only
                "t_ms": first["t_ms"],
                "projection": {
                    "started_at_zero": offsets[0] == 0 if offsets else None,
                    "off_monotonic": monotonic,
                    "restarts": restarts,
                    "final_off": final_off,
                    "first_has_sha": first["projection"].get("has_sha"),
                    "first_has_len": first["projection"].get("has_len"),
                },
            })
            i = j
            continue
        out.append(ops[i])
        i += 1
    return out


# --- the capture --------------------------------------------------------------

def extract_att(capture: Path, tshark: Path = TSHARK):
    """Yields one record per ATT write-command or notification in the capture.

    tshark is asked only to undo ACL/L2CAP/ATT framing and hand back
    `btatt.value`. Streamed line by line: a capture of a 134 KiB upload at a
    20-byte MTU is thousands of packets, and there is no reason to hold it.
    """
    cmd = [str(tshark), "-r", str(capture), "-Y",
           f"btatt.opcode == {ATT_WRITE_COMMAND} || btatt.opcode == {ATT_HANDLE_VALUE_NOTIFICATION}",
           "-T", "fields", "-E", "separator=,", "-e", "frame.number",
           "-e", "frame.time_relative", "-e", "bthci_acl.chandle",
           "-e", "btatt.opcode", "-e", "btatt.handle", "-e", "btatt.value"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, encoding="utf-8", errors="replace", bufsize=1)
    for line in proc.stdout:
        parts = line.rstrip("\n").split(",")
        if len(parts) < 6:
            continue
        frame, t_rel, chandle, opcode, handle, value = parts[:6]
        # A field can repeat within one frame (several ATT PDUs); tshark then
        # comma-joins the occurrences, which the split above has already
        # separated. Take the first of each and let the value be the rest.
        try:
            opcode_i = int(opcode.split(",")[0], 0)
            handle_i = int(handle.split(",")[0], 0)
        except ValueError:
            continue
        raw = re.sub(r"[^0-9a-fA-F]", "", value)
        if len(raw) % 2:
            continue
        yield {"frame": int(frame) if frame.isdigit() else 0,
               "t_ms": (float(t_rel) * 1000.0) if t_rel else 0.0,
               "chandle": chandle.split(",")[0] or "?",
               "opcode": opcode_i, "handle": handle_i, "payload": bytes.fromhex(raw)}
    proc.wait()
    err = proc.stderr.read()
    if proc.returncode != 0:
        raise RuntimeError(f"tshark exit {proc.returncode}: {err.strip()[:300]}")


def decode_capture(capture: Path, *, tshark: Path = TSHARK,
                   smp_handle: int | None = None, all_streams: bool = False):
    """Finds the SMP characteristic in a capture and decodes both directions.

    **The handle is found by plausibility, not by discovery and not by volume.**
    Discovery may be absent from the capture entirely -- Windows caches GATT, so
    a reconnect need not re-read the attribute table on the air -- and "the
    busiest handle" would pick whichever of the user's own Bluetooth peripherals
    happened to be chattier. So every `(chandle, handle)` pair is reassembled
    and the one that yields the most whole, plausible SMP messages *and* at
    least one request in group 0 or 1 wins.
    """
    streams: dict[tuple[str, int, str], Reassembler] = {}
    order: list[tuple[str, int, str]] = []
    for rec in extract_att(capture, tshark):
        if smp_handle is not None and rec["handle"] != smp_handle:
            continue
        direction = "req" if rec["opcode"] == ATT_WRITE_COMMAND else "rsp"
        key = (rec["chandle"], rec["handle"], direction)
        if key not in streams:
            streams[key] = Reassembler()
            order.append(key)
        streams[key].feed(rec["payload"], t_ms=rec["t_ms"])

    candidates: dict[tuple[str, int], dict] = {}
    for (chandle, handle, direction), asm in streams.items():
        entry = candidates.setdefault((chandle, handle),
                                      {"messages": [], "streams": {}, "accounts": True})
        entry["messages"].extend(asm.messages)
        entry["streams"][direction] = asm.summary()
        entry["accounts"] = entry["accounts"] and asm.accounts()

    def score(item) -> int:
        msgs = item[1]["messages"]
        if not any(m["op"] in (0, 2) and m["group"] in (0, 1) for m in msgs):
            return -1
        return len(msgs)

    considered = [{"chandle": c, "handle": h, "messages": len(v["messages"])}
                  for (c, h), v in candidates.items()]

    def described(key, value) -> dict:
        chandle, handle = key
        messages = sorted(value["messages"], key=lambda m: m["t_ms"])
        ops = [render(m) for m in messages]
        return {
            "capture": str(capture),
            "smp_chandle": chandle, "smp_handle": handle,
            "handle_source": "override" if smp_handle is not None else "smp-plausibility",
            "handles_considered": considered,
            "streams": value["streams"], "accounts": value["accounts"],
            "messages": len(messages),
            "first_ms": messages[0]["t_ms"] if messages else None,
            "last_ms": messages[-1]["t_ms"] if messages else None,
            "ops": collapse_uploads(ops),
            "raw_ops": ops,
            "seq_echo_ok": _seq_echo_ok(messages),
            "clean_headers": all(m["flags"] == 0 for m in messages),
        }

    plausible = sorted((k for k, v in candidates.items() if score((k, v)) >= 0),
                       key=lambda k: min(m["t_ms"] for m in candidates[k]["messages"]))

    if not plausible:
        empty = {"capture": str(capture), "smp_handle": None,
                 "handle_source": "override" if smp_handle is not None else "none",
                 "handles_considered": considered, "ops": [], "messages": 0,
                 "detail": "no handle carried a plausible SMP request"}
        return [empty] if all_streams else empty

    # **Every** SMP-carrying stream, in the order they first appear, not just the
    # busiest. One trace can hold several: each GATT connection gets its own
    # connection handle, so two clients updating the same device one after the
    # other are two streams -- which is exactly the shape of a cross-check run
    # captured in a single trace. Picking one would silently discard an arm.
    described_all = [described(k, candidates[k]) for k in plausible]
    if all_streams:
        return described_all
    return max(described_all, key=lambda d: d["messages"])


def _seq_echo_ok(messages: list[dict]) -> bool | None:
    """Does every response echo an outstanding request's sequence number?

    An intrinsic, per-client check -- worth more than the cross-client
    comparison because it needs no second client and it is a protocol
    invariant (protocol-notes.md section 4), not a convention.
    """
    outstanding: dict[tuple[int, int, int], int] = {}
    seen = False
    for message in messages:
        key = (message["seq"], message["group"], message["command"])
        if message["op"] in (0, 2):
            outstanding[key] = outstanding.get(key, 0) + 1
        else:
            seen = True
            if not outstanding.get(key):
                return False
            outstanding[key] -= 1
    return seen or None


# --- comparison ---------------------------------------------------------------

REQUIRED = [
    ("upload-run", {}),
    ("state", {"confirm": False, "has_hash": True}),
    ("reset", {}),
    ("state", {"confirm": True}),
]


def _matches(op: dict, name: str, want: dict) -> bool:
    if op["name"] != name or op["dir"] != "req":
        return False
    projection = op.get("projection", {})
    return all(projection.get(k) == v for k, v in want.items())


def required_subsequence(ops: list[dict], required=None) -> dict:
    """Are the required operations present, in order, as a subsequence?

    A subsequence rather than the whole sequence, deliberately. smply's updater
    reads image state before it uploads and another client need not, so
    demanding equality would fail every run and would say nothing about
    interoperability. What matters is that both clients performed the same
    *required* steps in the same order; everything else is reported as `extra`
    and is informational.
    """
    required = REQUIRED if required is None else required
    index, matched, extra = 0, [], []
    for op in ops:
        if index < len(required) and _matches(op, *required[index]):
            matched.append({"name": required[index][0], "want": required[index][1]})
            index += 1
        elif op["dir"] == "req":
            extra.append(op["name"])
    missing = [{"name": n, "want": w} for n, w in required[index:]]
    return {"ok": not missing, "matched": matched, "missing": missing, "extra": extra}


def compare(left_name: str, left: list[dict], right_name: str,
            right: list[dict]) -> list[dict]:
    """Cross-client Tier B divergences: the required ops, pairwise."""
    divergences = []
    lreq = [op for op in left if op["dir"] == "req"]
    rreq = [op for op in right if op["dir"] == "req"]
    lnames = [op["name"] for op in lreq]
    rnames = [op["name"] for op in rreq]
    for name in sorted(set(n for n, _ in REQUIRED)):
        if (name in lnames) != (name in rnames):
            divergences.append({"tier": "B", "class": "cross-client", "path": f"op:{name}",
                                "clients": [left_name, right_name],
                                "left": name in lnames, "right": name in rnames})
    for name in sorted(set(n for n, _ in REQUIRED)):
        lops = [op for op in lreq if op["name"] == name]
        rops = [op for op in rreq if op["name"] == name]
        for i, (lop, rop) in enumerate(zip(lops, rops)):
            keys = set(lop.get("projection", {})) | set(rop.get("projection", {}))
            for key in sorted(keys):
                lv, rv = lop["projection"].get(key), rop["projection"].get(key)
                if lv != rv:
                    divergences.append({"tier": "B", "class": "cross-client",
                                        "path": f"op:{name}[{i}].{key}",
                                        "clients": [left_name, right_name],
                                        "left": lv, "right": rv})
    return divergences


# --- self-test ----------------------------------------------------------------
#
# The goldens are real bytes from the NUCLEO-WB55RG peer, recorded during P17a
# bring-up and kept in build/hil-evidence/manual/. Using the device's own bytes
# rather than hand-built ones is the whole point: A18 (indefinite-length CBOR)
# was invisible to every hand-built golden in P5-P8.

GOLDEN_PARAMS = bytes.fromhex(
    "0900001900000006"
    "bf686275665f73697a651909ab696275665f636f756e7404ff")
GOLDEN_STATE = bytes.fromhex(
    "0900008600010100"
    "bf66696d616765739fbf64736c6f74006776657273696f6e65312e302e3064686173685820"
    "fc451c569e221ea27bbd85d0e1c1c0edbaa969aafa531e03f774ed40a921566c68626f6f74"
    "61626c65f56770656e64696e67f469636f6e6669726d6564f566616374697665f569706572"
    "6d616e656e74f4ffff6b73706c697453746174757300ff")
# Slot info, and the reason it is here: it ends in five consecutive breaks, which
# is the shape that defeats QCBOR's ExitMap (A18). Its header also carries
# version bits 0 where the two above carry 1 -- the same device, answering two
# clients that asked in different versions. That is A23's question, and this file
# only records what it sees.
GOLDEN_SLOTINFO = bytes.fromhex(
    "0100005100010706"
    "bf66696d616765739fbf65696d6167650065736c6f74739fbf64736c6f74006473697a651a"
    "00066000ffbf64736c6f74016473697a651a000670006f75706c6f61645f696d6167655f69"
    "6400ffffffffff")


def _fail(checks: list[str], condition: bool, description: str) -> None:
    if not condition:
        checks.append(description)


def self_test() -> int:
    problems: list[str] = []

    # 1. The three device goldens decode to the values recorded beside them.
    asm = Reassembler()
    asm.feed(GOLDEN_PARAMS)
    _fail(problems, len(asm.messages) == 1, "params: expected one message")
    params = render(asm.messages[0])
    _fail(problems, params["name"] == "params" and params["dir"] == "rsp",
          f"params: rendered as {params['name']}/{params['dir']}")
    _fail(problems, params["projection"] == {"buf_size": 2475, "buf_count": 4},
          f"params: projection {params['projection']}")
    _fail(problems, params["version"] == 1, f"params: version {params['version']}")
    _fail(problems, asm.accounts(), "params: bytes do not account")

    asm = Reassembler()
    asm.feed(GOLDEN_STATE)
    state = render(asm.messages[0])
    _fail(problems, state["name"] == "state", f"state: rendered as {state['name']}")
    _fail(problems, state["projection"]["slot_count"] == 1,
          f"state: slot_count {state['projection'].get('slot_count')}")
    _fail(problems, state["projection"]["flags"] == [
        {"slot": 0, "active": True, "pending": False, "confirmed": True, "permanent": False}],
        f"state: flags {state['projection'].get('flags')}")
    _fail(problems, state.get("body_trailing") == 0,
          f"state: {state.get('body_trailing')} trailing body bytes")

    asm = Reassembler()
    asm.feed(GOLDEN_SLOTINFO)
    slotinfo = render(asm.messages[0])
    _fail(problems, slotinfo["name"] == "slot-info", f"slot-info: {slotinfo['name']}")
    _fail(problems, slotinfo["version"] == 0,
          f"slot-info: version {slotinfo['version']} (expected the recorded 0)")
    _fail(problems, slotinfo.get("body_trailing") == 0,
          "slot-info: five consecutive breaks were not consumed")

    # 2. Fragmentation is invisible: the same message at every fragment size.
    for size in (1, 7, 8, 20, 244, len(GOLDEN_STATE)):
        asm = Reassembler()
        for i in range(0, len(GOLDEN_STATE), size):
            asm.feed(GOLDEN_STATE[i:i + size])
        _fail(problems, len(asm.messages) == 1 and asm.accounts(),
              f"fragment size {size}: {len(asm.messages)} messages, "
              f"accounts={asm.accounts()}")
        if asm.messages:
            got = render(asm.messages[0])
            _fail(problems, got["projection"] == state["projection"],
                  f"fragment size {size}: projection differs")

    # 3. Two messages back to back split at 8 + length, with no framing.
    asm = Reassembler()
    asm.feed(GOLDEN_PARAMS + GOLDEN_STATE)
    _fail(problems, len(asm.messages) == 2, f"concatenated: {len(asm.messages)} messages")
    _fail(problems, asm.accounts() and asm.trailing_bytes == 0,
          "concatenated: bytes do not account")

    # 4. A mid-message start is reported, not absorbed.
    asm = Reassembler()
    asm.feed(GOLDEN_STATE[40:] + GOLDEN_PARAMS)
    _fail(problems, asm.truncated_start, "mid-message start was not flagged")
    _fail(problems, len(asm.messages) == 1, f"after resync: {len(asm.messages)} messages")
    _fail(problems, asm.accounts(), "resync: bytes do not account")

    # 5. A partial tail is counted, not dropped.
    asm = Reassembler()
    asm.feed(GOLDEN_PARAMS + GOLDEN_STATE[:20])
    _fail(problems, asm.trailing_bytes == 20, f"partial tail: {asm.trailing_bytes}")
    _fail(problems, asm.accounts(), "partial tail: bytes do not account")

    # 6. Implausible headers are refused.
    _fail(problems, parse_header(b"\xe0" + bytes(7)) is None, "reserved bits accepted")
    _fail(problems, parse_header(bytes([0x11]) + bytes(7)) is None, "version 0b10 accepted")
    _fail(problems, parse_header(bytes([0x04]) + bytes(7)) is None, "op 4 accepted")
    _fail(problems, parse_header(bytes([0x00, 0x01]) + bytes(6)) is None, "set flag accepted")
    _fail(problems, parse_header(bytes([0x00, 0x00, 0xFF, 0xFF]) + bytes(4)) is None,
          "implausible length accepted")

    # 7. The CBOR reader on both encodings of the same value.
    definite = bytes.fromhex("a1636f666600")            # {"off": 0}
    indefinite = bytes.fromhex("bf636f666600ff")         # the same, indefinite
    _fail(problems, cbor_load(definite)[0] == {"off": 0}, "definite map failed")
    _fail(problems, cbor_load(indefinite)[0] == {"off": 0}, "indefinite map failed")
    try:
        cbor_load(bytes.fromhex("bf636f6666"))
        problems.append("an unterminated map was accepted")
    except CborError:
        pass

    # 8. The required subsequence is not vacuous.
    def req(name, **projection):
        return {"dir": "req", "name": name, "group": 1, "command": 0,
                "projection": projection}
    good = [req("state", slot_count=1), req("upload-run"),
            req("state", confirm=False, has_hash=True), req("reset"),
            req("state", confirm=True)]
    _fail(problems, required_subsequence(good)["ok"], "a good sequence was rejected")
    _fail(problems, required_subsequence(good)["extra"] == ["state"],
          f"extra was {required_subsequence(good)['extra']}")
    _fail(problems, not required_subsequence(good[:-1])["ok"],
          "a missing confirm was accepted")
    out_of_order = [good[3], good[1], good[2], good[4]]
    _fail(problems, not required_subsequence(out_of_order)["ok"],
          "an out-of-order sequence was accepted")

    if problems:
        print("smp_decode self-test FAILED:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print("smp_decode self-test OK")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true",
                        help="decode the recorded device goldens and exit")
    parser.add_argument("--capture", type=Path, help="a pcapng to decode")
    parser.add_argument("--out", type=Path, help="write the decoded operations here")
    parser.add_argument("--tshark", type=Path, default=TSHARK)
    parser.add_argument("--smp-handle", type=lambda v: int(v, 0), default=None,
                        help="force the ATT handle instead of finding it")
    parser.add_argument("--require-op", action="append", default=None,
                        metavar="GROUP:CMD",
                        help="debug: also require this op, to prove the check bites")
    parser.add_argument("--all-streams", action="store_true",
                        help="report every SMP-carrying connection, not just the "
                             "busiest -- one trace can hold several clients")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if not args.capture:
        parser.error("either --self-test or --capture is required")

    result = decode_capture(args.capture, tshark=args.tshark,
                            smp_handle=args.smp_handle, all_streams=args.all_streams)
    required = None
    if args.require_op:
        required = list(REQUIRED)
        for spec in args.require_op:
            group, command = (int(p, 0) for p in spec.split(":", 1))
            required.append((COMMAND_NAMES.get((group, command), f"cmd{command}"), {}))

    streams = result if isinstance(result, list) else [result]
    for decoded in streams:
        decoded["required"] = required_subsequence(decoded["ops"], required)
    if args.out:
        args.out.write_text(json.dumps(result, indent=2, default=str) + "\n",
                            encoding="utf-8")
        lines = []
        for decoded in streams:
            lines.append(f"# chandle={decoded.get('smp_chandle')} "
                         f"handle={decoded.get('smp_handle')} "
                         f"messages={decoded.get('messages')} "
                         f"first_ms={decoded.get('first_ms')}")
            lines += [f"{op['dir']} {op['name']:12} "
                      f"{json.dumps(op.get('projection', {}), default=str)}"
                      for op in decoded["ops"]]
        args.out.with_suffix(".txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps([{k: d[k] for k in ("smp_chandle", "smp_handle", "messages",
                                         "accounts", "first_ms", "last_ms", "required")
                       if k in d} for d in streams], indent=2, default=str))
    return 0 if any(d.get("messages") for d in streams) else 2


if __name__ == "__main__":
    sys.exit(main())
