# SPDX-License-Identifier: Apache-2.0
"""P17c cross-check: the same update by smply and by smpmgr, oracled over UART.

Each *arm* starts from an identical freshly flashed baseline (image A) and
installs image B the test-then-confirm way. Device state is read through the
**oracle** -- `mcumgr-client list` over the UART shell transport, a path neither
BLE client touches -- at three checkpoints, and the arms' normalised states are
compared with each other and with what each checkpoint should show.

`mcumgr-client` is also *selectable* as a third arm, and is not in the default
set: it reads state reliably but could not complete an upload over the shell
transport on this peer in any configuration tried (protocol-notes.md section 9,
"the UART arm is an oracle, not a third client"). So this is two clients
compared against each other against an independent oracle, which is worth
saying plainly rather than describing a three-way comparison that did not
happen.

Third-party clients are for behavioural comparison and as an independent oracle,
never as a protocol reference (ADR-0015 decision 3). A divergence is traced to
Zephyr or MCUboot source and recorded in protocol-notes.md section 9 *before*
smply changes.

### Why three checkpoints, and not two

The obvious design reads state before and after an update, and it cannot fail.
Every arm is already required to end up running image B confirmed, so comparing
"the state afterwards" compares fields that were just asserted, and the only free
variable is which slots are listed -- which is the same for every client on this
bootloader. The first version of this script did exactly that and its
`states_agree` was `True` by construction.

The information is in the **trial boot**. Between the reset and the confirm, the
active slot holds B *unconfirmed* while the fallback slot holds A *confirmed*
(protocol-notes.md section 7), so four fields differ at once and a client that
takes a shortcut -- confirming without testing, or setting `permanent` directly
-- shows up as a different state rather than the same one. So every arm installs
and confirms as **two separate invocations**, and `S1` is read in between.

### Two tiers, independent by construction

* **Tier A** -- normalised device state at `S0`, `S1` and `S2`, per arm and
  across arms. Needs no capture and is the load-bearing comparison.
* **Tier B** -- the decoded SMP operation sequence from an HCI capture, compared
  between the two BLE arms, tolerating different sequence numbers, fragmentation,
  timing and chunk sizes (docs/testing.md section 6). See `tools/smp_decode.py`.

Tier B being unavailable **never** turns a Tier A pass into a failure, and never
the other way round. In an unelevated shell with no BTVS running, the correct
outcome of this script is **exit 2** with Tier B unavailable on both BLE arms --
and that does *not* discharge P17c's acceptance, which needs a run with a
non-empty capture.

  python tests/hil/crosscheck.py --evidence build/hil-peer-out/evidence \
      --address 80:E1:26:00:65:E2 --uart COM4 \
      --dfu build/windows-hil/examples/winrt_ble_dfu/winrt_ble_dfu.exe \
      --smpmgr build/hil-tools/Scripts/smpmgr.exe \
      --mcumgr-client build/mcumgr-client/mcumgr-client-windows-x86/mcumgr-client.exe

Exit status: 0 everything compared and agreed; 1 a client failed or the arms
diverged; 2 nothing failed but something was unavailable.
"""
import argparse
import hashlib
import json
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "tools"))

from bench_support import Bench, CUBE_CLI, UartPort, run_step, slug  # noqa: E402
from hci_capture import BTVS, CAPINFOS, TSHARK, HciCapture  # noqa: E402
import smp_decode  # noqa: E402

# MCUboot image TLV constants (docs/protocol-notes.md section 7).
TLV_INFO_MAGIC = 0x6907
TLV_PROT_INFO_MAGIC = 0x6908
TLV_SHA256 = 0x10


# --- inputs -------------------------------------------------------------------

def image_hash(path: Path) -> str:
    """The `IMAGE_TLV_SHA256` of a signed image, which is what the device reports.

    Not the file's own digest: MCUboot reports the hash from the image's TLV
    trailer, and comparing the wrong one would make every arm look wrong.
    """
    data = path.read_bytes()
    hdr_size = int.from_bytes(data[8:10], "little")
    img_size = int.from_bytes(data[12:16], "little")
    off, end = hdr_size + img_size, len(data)
    while off + 4 <= end:
        # The two TLV-area headers carry a magic where a type would be. Checked
        # first because they are skipped, not decoded; no real TLV type collides.
        magic = int.from_bytes(data[off:off + 2], "little")
        if magic in (TLV_INFO_MAGIC, TLV_PROT_INFO_MAGIC):
            off += 4
            continue
        tlv_type = magic
        tlv_len = int.from_bytes(data[off + 2:off + 4], "little")
        if tlv_type == TLV_SHA256 and tlv_len == 32:
            return data[off + 4:off + 36].hex()
        off += 4 + tlv_len
    raise RuntimeError(f"no SHA256 TLV in {path}")


# --- the oracle ---------------------------------------------------------------

# Zephyr's shell transport frames at **127** bytes (protocol-notes.md section 8,
# 124 of payload), and `mcumgr-client` 0.0.9 defaults to 128 -- one byte over. A
# sustained upload with the default dies within seconds on a framing error. Set
# explicitly on every invocation rather than only on the ones observed to need
# it: a short read that happens to fit is not a reason to send a frame the
# device cannot accept.
UART_LINE_LENGTH = "127"


def uart_command(args, *rest) -> list:
    """The UART tool, with the framing option it needs, and then `rest`."""
    return [args.mcumgr_client, "-d", args.uart, "-l", UART_LINE_LENGTH, *rest]


def oracle_state(args, port: UartPort, log: Path) -> dict:
    """Device image state as `mcumgr-client` reports it over UART.

    Three attempts, and they really are three: `run_step()` returns a record
    rather than raising, so a timeout is retried like any other failure. The
    earlier version could not retry a timeout at all -- its helper threw past
    the loop.

    The console logger is stopped for the duration: only one process may hold
    the port.
    """
    detail = "no attempt was made"
    with port.client():
        for _ in range(3):
            step = run_step(uart_command(args, "list"), log, 90)
            if step["timed_out"]:
                detail = "the oracle timed out"
            else:
                match = re.search(r"response:\s*(\{.*\})", step["stdout"], re.S)
                if step["rc"] == 0 and match:
                    try:
                        return json.loads(match.group(1))
                    except json.JSONDecodeError as error:
                        detail = f"unreadable oracle response: {error}"
                else:
                    detail = f"oracle exit {step['rc']}"
            time.sleep(2)
    raise Unavailable(f"the UART oracle did not answer: {detail}")


class Unavailable(RuntimeError):
    """The bench or an instrument could not answer. Never a pass, never a fail."""


class ClientFailed(RuntimeError):
    """A client under comparison failed. That is a result."""


# --- normalisation ------------------------------------------------------------

SLOT_FLAGS = ("bootable", "pending", "confirmed", "active", "permanent")


def normalise(state: dict, hashes: dict[str, str]) -> dict:
    """Device state in a form two clients' answers can be compared in.

    Three rules earn their keep:

    * **`hash_id` is the comparison key**, not the 64-character hash: a diff
      that reads `B != A` is actionable and one that reads two hex strings is
      not, and the negative control depends on that being legible.
    * **Every slot**, sorted, not just the active one. A trial boot lists two,
      and the second is where the interesting flags are. A slot entry with no
      `slot` key goes to `malformed` rather than raising.
    * **A missing boolean is `False`**, in both directions, so a server that
      omits false flags (`FRUGAL_LIST`, off on this peer) compares equal to one
      that spells them out.

    `splitStatus` is recorded and **not** compared: `mcumgr-client` renders the
    CBOR `0` as the string `"NotApplicable"`, which is its rendering choice, not
    a fact about the device.
    """
    by_hash = {v: k for k, v in hashes.items()}
    entries = state.get("images") or []
    slots, malformed = [], []
    for entry in entries:
        if not isinstance(entry, dict) or "slot" not in entry:
            malformed.append(entry)
            continue
        raw = (entry.get("hash") or "").lower()
        slots.append({
            "image": entry.get("image", 0),
            "slot": entry["slot"],
            "hash": raw,
            "hash_id": by_hash.get(raw, f"unknown:{raw[:8]}" if raw else "none"),
            "version": entry.get("version"),
            **{flag: bool(entry.get(flag, False)) for flag in SLOT_FLAGS},
        })
    slots.sort(key=lambda s: (s["image"], s["slot"]))
    active = next((s for s in slots if s["active"]), None)
    others = [s for s in slots if not s["active"]]
    return {
        "slots": slots,
        "active": active,
        "fallback": others[0] if len(others) == 1 else None,
        "slot_count": len(slots),
        "malformed": malformed,
        "split_status_raw": state.get("splitStatus"),
    }


def _paths(normalised: dict) -> dict:
    """Flattens a normalised state into the fields the diff walks."""
    out = {"slot_count": normalised["slot_count"],
           "malformed": len(normalised["malformed"])}
    for role in ("active", "fallback"):
        entry = normalised.get(role)
        if entry is None:
            out[f"{role}"] = None
            continue
        for key in ("slot", "hash_id", "version", *SLOT_FLAGS):
            out[f"{role}.{key}"] = entry[key]
    return out


def diff(left_name: str, left: dict, right_name: str, right: dict,
         checkpoint: str, kind: str = "cross-client") -> list[dict]:
    """Every field at which two normalised states differ."""
    lp, rp = _paths(left), _paths(right)
    divergences = []
    for path in sorted(set(lp) | set(rp)):
        if lp.get(path) != rp.get(path):
            divergences.append({"tier": "A", "class": kind, "checkpoint": checkpoint,
                                "path": path, "clients": [left_name, right_name],
                                "left": lp.get(path), "right": rp.get(path)})
    return divergences


def expectation(checkpoint: str, target: str, source: str) -> dict:
    """The state a checkpoint should show, as a normalised form to diff against.

    Written out rather than asserted field by field so that a mismatch is
    reported by the same diff machinery as a cross-client one, in the same shape.
    """
    if checkpoint == "S0":
        return {"slots": [], "slot_count": 1, "malformed": [],
                "active": {"slot": 0, "hash_id": source, "version": None,
                           "bootable": True, "pending": False, "confirmed": True,
                           "active": True, "permanent": False},
                "fallback": None}
    if checkpoint == "S1":
        return {"slots": [], "slot_count": 2, "malformed": [],
                "active": {"slot": 0, "hash_id": target, "version": None,
                           "bootable": True, "pending": False, "confirmed": False,
                           "active": True, "permanent": False},
                "fallback": {"slot": 1, "hash_id": source, "version": None,
                             "bootable": True, "pending": False, "confirmed": True,
                             "active": False, "permanent": False}}
    return {"slots": [], "slot_count": 1, "malformed": [],
            "active": {"slot": 0, "hash_id": target, "version": None,
                       "bootable": True, "pending": False, "confirmed": True,
                       "active": True, "permanent": False},
            "fallback": None}


def against_expectation(arm: str, observed: dict, checkpoint: str, target: str,
                        source: str) -> list[dict]:
    """Diffs an observation against the checkpoint's expected shape.

    `version` is excluded here (it is compared *across* arms instead) because
    the expected strings belong to the images, not to the protocol, and hard-
    coding them here would duplicate what the image files already say.
    """
    want = expectation(checkpoint, target, source)
    return [d for d in diff(arm, observed, "expected", want, checkpoint, "expectation")
            if not d["path"].endswith(".version")]


# --- the arms -----------------------------------------------------------------
#
# Every arm is two invocations -- install-and-trial, then confirm -- so that S1
# can be read in between. smply's example grew `--mode test-only` and
# `--mode confirm-only` for exactly this; the other two clients already work
# that way.

def smply_install(ctx) -> None:
    ctx.step([ctx.args.dfu, "--mode", "test-only", "--image", ctx.image,
              "--address", ctx.args.address], 600, "install")


def smply_confirm(ctx) -> None:
    ctx.step([ctx.args.dfu, "--mode", "confirm-only", "--address", ctx.args.address],
             180, "confirm")


def smpmgr_install(ctx) -> None:
    # Explicit subcommands rather than `smpmgr upgrade`, which does upload,
    # mark-for-test and reset in one call: the point of this comparison is that
    # the three arms perform the *same* steps, and an arm whose steps are
    # bundled cannot be read at S1. `upgrade` is worth a separate variant run,
    # not the shape of the comparison.
    ble = [ctx.args.smpmgr, "--ble", ctx.args.address, "--timeout", "40"]
    ctx.step(ble + ["image", "upload", ctx.image], 900, "upload")
    # `state-write` takes the hash positionally -- confirmed from `--help`, not
    # assumed. With a hash it marks for test; with `--confirm` and no hash it
    # confirms the running image.
    ctx.step(ble + ["image", "state-write", ctx.image_hash], 120, "mark-for-test")
    ctx.step(ble + ["os", "reset"], 120, "reset")
    ctx.settle()


def smpmgr_confirm(ctx) -> None:
    ctx.step([ctx.args.smpmgr, "--ble", ctx.args.address, "--timeout", "40",
              "image", "state-write", "--confirm"], 180, "confirm")


def mcumgr_install(ctx) -> None:
    # Kept, and **not** in the default client set: P17c could not get a 134 KiB
    # upload through this transport in any configuration (protocol-notes.md
    # section 9, "the UART arm is an oracle, not a third client"). Selecting it
    # explicitly is how a future bench revision -- logs off USART1, or the raw
    # UART transport instead of the shell one -- gets retried.
    uart = uart_command(ctx.args)
    with ctx.port.client():
        # The UART upload is the slow one: 134 KiB at 115200 through base64,
        # CRC16 and a 124-byte frame payload (protocol-notes.md section 8).
        ctx.step(uart + ["upload", ctx.image], 1200, "upload")
        ctx.step(uart + ["test", ctx.image_hash], 120, "mark-for-test")
        ctx.step(uart + ["reset"], 120, "reset")
    ctx.settle()


def mcumgr_confirm(ctx) -> None:
    with ctx.port.client():
        ctx.step(uart_command(ctx.args, "test", ctx.image_hash, "--confirm", "true"),
                 120, "confirm")


ARMS = {
    "smply": {"transport": "ble", "install": smply_install, "confirm": smply_confirm,
              "tool": "dfu"},
    "smpmgr": {"transport": "ble", "install": smpmgr_install, "confirm": smpmgr_confirm,
               "tool": "smpmgr"},
    "mcumgr-client": {"transport": "uart", "install": mcumgr_install,
                      "confirm": mcumgr_confirm, "tool": "mcumgr_client"},
}


class ArmContext:
    """What an arm's phases are given, and where their steps are recorded."""

    def __init__(self, args, port: UartPort, log_dir: Path, image: Path,
                 image_hash_hex: str) -> None:
        self.args = args
        self.port = port
        self.log_dir = log_dir
        self.image = str(image)
        self.image_hash = image_hash_hex
        self.steps: list[dict] = []
        self._n = 0

    def step(self, cmd, timeout: int, name: str) -> dict:
        self._n += 1
        log = self.log_dir / f"{self._n:02d}-{name}.log"
        record = run_step(cmd, log, timeout)
        record["name"] = name
        self.steps.append({k: record[k] for k in
                           ("name", "cmd", "rc", "timed_out", "seconds")})
        if record["timed_out"]:
            raise ClientFailed(f"{name}: no result within {timeout}s")
        if record["rc"] != 0:
            raise ClientFailed(f"{name}: exit {record['rc']}")
        return record

    def settle(self) -> None:
        """Waits out a reset: the swap plus re-advertising.

        A20 measured ~6.3 s of unreachability after a swap reset and up to
        ~13 s to the first advertisement on a cold scan, so this is a margin
        rather than a guess -- but it is still a fixed wait, which is why every
        following step has its own generous timeout instead of relying on it.
        """
        time.sleep(15)


# --- the oracle self-test -----------------------------------------------------

def oracle_self_test(args, bench: Bench, port: UartPort, run_dir: Path,
                     hashes: dict[str, str], negate: bool = False) -> dict:
    """Proves the oracle can tell two device states apart, before anything else.

    "The three clients agree" is worthless if the instrument cannot detect
    disagreement, and nothing else in this script establishes that it can. So:
    flash A, flash B, flash A again, and require that the diff notices exactly
    the change that happened and **invents nothing**. It needs no radio and no
    third-party BLE client, because the baseline flasher already takes
    `--image {a,b}`.

    A failure here makes the whole run `unavailable` and no arm runs.
    """
    log_dir = run_dir / "self-test"
    log_dir.mkdir(parents=True, exist_ok=True)
    log = log_dir / "self-test.log"
    reads = {}
    for label, image in (("a1", "a"), ("b", "b"), ("a2", "a")):
        if bench.flash_baseline(log_dir / f"flash-{label}.log", image) != "ok":
            raise Unavailable(f"the self-test could not flash image {image}")
        time.sleep(3)
        reads[label] = normalise(oracle_state(args, port, log), hashes)

    problems = []
    expected_paths = {"active.hash_id", "active.version"}
    changed = {d["path"] for d in diff("a", reads["a1"], "b", reads["b"], "self-test")}
    if negate:
        # Debug: invert the expectation, so a passing self-test can itself be
        # shown to be capable of failing. A control you cannot run is not one.
        expected_paths = {"active.slot"}
    if changed != expected_paths:
        problems.append(f"A vs B changed {sorted(changed)}, expected "
                        f"{sorted(expected_paths)}")
    repeat = diff("a", reads["a1"], "a-again", reads["a2"], "self-test")
    if repeat:
        problems.append(f"two reads of the same state differed at "
                        f"{[d['path'] for d in repeat]}")
    for label, want in (("a1", "A"), ("b", "B"), ("a2", "A")):
        active = reads[label]["active"]
        if active is None or active["hash_id"] != want:
            problems.append(f"{label}: active is "
                            f"{active and active['hash_id']}, expected {want}")
        elif not active["confirmed"] or active["pending"]:
            problems.append(f"{label}: a freshly flashed image is "
                            f"confirmed={active['confirmed']} pending={active['pending']}")
    return {"reads": reads, "changed": sorted(changed), "problems": problems,
            "ok": not problems}


# --- the run ------------------------------------------------------------------

def run_arm(args, bench: Bench, name: str, run_dir: Path, hashes: dict[str, str],
            source: str) -> dict:
    """One client, three checkpoints, one capture."""
    spec = ARMS[name]
    log_dir = run_dir / slug(name)
    log_dir.mkdir(parents=True, exist_ok=True)
    port = UartPort(bench, log_dir)
    target = "B"
    image = args.evidence / "b.signed.bin"
    entry: dict = {"client": name, "transport": spec["transport"],
                   "target": target, "steps": [],
                   "tier_a": "unavailable", "tier_b": "not-applicable",
                   # None until decided, and *not* "unavailable": an initial
                   # value that is also a real verdict short-circuits the
                   # combination below, which is how a Tier A failure came out
                   # reported as an unavailable arm.
                   "verdict": None, "detail": "", "divergences": []}
    if name in args.skip_confirm:
        entry["debug"] = "--skip-confirm: this arm deliberately did not confirm"

    capture = None
    if spec["transport"] == "ble" and args.capture != "off":
        capture = HciCapture(log_dir / f"{slug(name)}.pcapng", mode=args.capture,
                             btvs=args.btvs, tshark=args.tshark, capinfos=args.capinfos)
    elif spec["transport"] == "ble":
        entry["tier_b"] = "unavailable"
        entry["hci_capture"] = {"status": "unavailable",
                                "detail": "capture disabled by request"}
    else:
        entry["hci_capture"] = {"status": "not-applicable",
                                "detail": "UART transport; there is no HCI traffic"}

    checkpoints: dict[str, dict] = {}
    try:
        flashed = bench.flash_baseline(log_dir / "flash-baseline.log")
        if flashed == "unavailable":
            raise Unavailable("baseline flash found no probe")
        if flashed != "ok":
            raise ClientFailed("the baseline flash failed")
        time.sleep(3)

        ctx = ArmContext(args, port, log_dir, image, image_hash(image))
        if spec["transport"] == "uart":
            # This arm holds the console port for its whole upload, so there is
            # no console capture to be had. Recorded rather than left implicit.
            entry["uart_console"] = {"status": "unavailable",
                                     "detail": "the client holds the port"}
        if capture is not None:
            entry["capture_how"] = capture.start()

        with port.console(slug(name)) if spec["transport"] == "ble" else _null():
            checkpoints["S0"] = normalise(oracle_state(args, port, log_dir / "oracle.log"),
                                          hashes)
            spec["install"](ctx)
            checkpoints["S1"] = normalise(oracle_state(args, port, log_dir / "oracle.log"),
                                          hashes)
            if name not in args.skip_confirm:
                spec["confirm"](ctx)
            checkpoints["S2"] = normalise(oracle_state(args, port, log_dir / "oracle.log"),
                                          hashes)
        entry["steps"] = ctx.steps
    except Unavailable as error:
        entry["verdict"], entry["detail"] = "unavailable", str(error)
    except ClientFailed as error:
        entry["verdict"], entry["detail"] = "fail", str(error)
    finally:
        if capture is not None:
            entry["hci_capture"] = capture.stop()

    entry["checkpoints"] = checkpoints

    # Tier A, per arm: every observed checkpoint against its expected shape.
    expectation_divergences = []
    for checkpoint, observed in checkpoints.items():
        expectation_divergences += against_expectation(name, observed, checkpoint,
                                                       target, source)
    entry["divergences"] += expectation_divergences
    if not checkpoints:
        entry["tier_a"] = "unavailable"
    elif len(checkpoints) < 3:
        entry["tier_a"] = "unavailable"
        entry["detail"] = entry["detail"] or "not every checkpoint was observed"
    elif expectation_divergences:
        entry["tier_a"] = "fail"
    else:
        entry["tier_a"] = "pass"

    # Tier B, per arm: decode the capture if there is one worth decoding.
    if capture is not None:
        entry["tier_b"], entry["tier_b_detail"], decoded = decode_arm(entry, log_dir, args)
        if decoded is not None:
            entry["ops"] = decoded["ops"]

    if entry["verdict"] is None:
        entry["verdict"] = _combine(entry["tier_a"], entry["tier_b"])
    return entry


class _null:
    def __enter__(self):
        return None

    def __exit__(self, *_):
        return False


def decode_arm(entry: dict, log_dir: Path, args) -> tuple[str, str, dict | None]:
    """Turns one arm's capture into an operation sequence, or says why not."""
    capture_result = entry.get("hci_capture") or {}
    if capture_result.get("status") != "ok":
        return ("unavailable",
                capture_result.get("detail") or f"capture {capture_result.get('status')}",
                None)
    try:
        decoded = smp_decode.decode_capture(Path(capture_result["file"]),
                                            tshark=args.tshark,
                                            smp_handle=args.smp_handle)
    except (RuntimeError, OSError) as error:
        return "unavailable", f"the capture could not be decoded: {error}", None

    (log_dir / f"{slug(entry['client'])}.ops.json").write_text(
        json.dumps(decoded, indent=2, default=str) + "\n", encoding="utf-8")
    (log_dir / f"{slug(entry['client'])}.ops.txt").write_text(
        "\n".join(f"{op['dir']} {op['name']:12} "
                  f"{json.dumps(op.get('projection', {}), default=str)}"
                  for op in decoded["ops"]) + "\n", encoding="utf-8")

    if not decoded.get("messages"):
        return "unavailable", "the capture held no SMP traffic", decoded
    if not decoded.get("accounts"):
        return "fail", "the decoder could not account for every captured byte", decoded
    if decoded.get("clean_headers") is False:
        return "fail", "a message carried a reserved flag", decoded
    if decoded.get("seq_echo_ok") is False:
        return "fail", "a response did not echo an outstanding request's sequence", decoded

    required = smp_decode.required_subsequence(decoded["ops"], args.required)
    decoded["required"] = required
    if not required["ok"]:
        truncated = any(s.get("truncated_start")
                        for s in (decoded.get("streams") or {}).values())
        missing = ", ".join(m["name"] for m in required["missing"])
        if truncated:
            return ("unavailable",
                    f"missing {missing}, and the capture started mid-stream", decoded)
        return "fail", f"missing required operations: {missing}", decoded
    return "pass", "", decoded


def _combine(tier_a: str, tier_b: str) -> str:
    """`fail` beats `unavailable` beats `pass`, and the tiers are independent.

    Tier B being unavailable must never demote a Tier A pass to a failure: a
    missing capture is a missing instrument, not a divergence. That is the whole
    reason the two are computed separately.
    """
    order = {"fail": 3, "unavailable": 2, "pass": 1, "not-applicable": 0}
    worst = max((tier_a, tier_b), key=lambda v: order.get(v, 0))
    return worst if order.get(worst, 0) > 0 else "pass"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--address", required=True)
    parser.add_argument("--uart", required=True)
    parser.add_argument("--dfu", type=Path, help="winrt_ble_dfu.exe")
    parser.add_argument("--smpmgr", type=Path)
    parser.add_argument("--mcumgr-client", type=Path, required=True,
                        help="also the oracle, so it is always required")
    parser.add_argument("--out", type=Path, default=Path("build/hil-crosscheck"))
    # The two BLE clients by default. The UART client is the **oracle** in every
    # run whether or not it is in this list; adding it here asks it also to
    # perform an update, which it cannot do on this peer (see mcumgr_install).
    parser.add_argument("--clients", default="smply,smpmgr",
                        help="comma-separated; default the two BLE clients")
    parser.add_argument("--cli", type=Path, default=CUBE_CLI)
    parser.add_argument("--python", default=None)
    parser.add_argument("--capture", default="auto",
                        choices=["auto", "attach", "spawn", "off"])
    parser.add_argument("--btvs", type=Path, default=BTVS)
    parser.add_argument("--tshark", type=Path, default=TSHARK)
    parser.add_argument("--capinfos", type=Path, default=CAPINFOS)
    parser.add_argument("--smp-handle", type=lambda v: int(v, 0), default=None)
    parser.add_argument("--self-test-only", action="store_true",
                        help="run the oracle self-test and stop")
    parser.add_argument("--no-self-test", action="store_true",
                        help="skip the self-test (debugging only; it gates the run)")
    parser.add_argument("--dry-run", action="store_true",
                        help="resolve every tool and input, touch no hardware")
    # Debug controls. They stay in the shipped script deliberately: the negative
    # controls that make this comparison trustworthy have to be re-runnable, and
    # a control nobody can re-run is not evidence.
    #
    # There is deliberately no "point an arm at the wrong image" control. It
    # sounds like the obvious one and it cannot work here: the bench holds two
    # images, every arm starts from a baseline of A and installs B, so the only
    # other image *is* the running one -- and marking the running image for test
    # is refused by the server (protocol-notes.md section 7). That arm would
    # fail at its mark step rather than reach a divergent state, which tests the
    # arm machinery and not the comparison. `--skip-confirm` is the control that
    # does what the other one only appears to: it leaves the device in its trial
    # boot, so the offending arm's S2 differs from the others in four fields at
    # once (slot count, the active slot's `confirmed`, and the fallback slot's
    # existence and flags).
    parser.add_argument("--self-test-negate", action="store_true",
                        help="debug: invert the self-test's expectation; must fail")
    parser.add_argument("--skip-confirm", action="append", default=[], metavar="ARM",
                        help="debug: leave one arm in its trial boot; must diverge")
    parser.add_argument("--require-op", action="append", default=None,
                        metavar="GROUP:CMD", help="debug: require an extra operation")
    args = parser.parse_args()

    clients = [c.strip() for c in args.clients.split(",") if c.strip()]
    unknown = [c for c in clients if c not in ARMS]
    if unknown:
        parser.error(f"unknown client(s) {unknown}; choose from {sorted(ARMS)}")
    for name in clients:
        tool = getattr(args, ARMS[name]["tool"], None)
        if tool is None:
            parser.error(f"--{ARMS[name]['tool'].replace('_', '-')} is required "
                         f"for the {name} arm")
    for arm in args.skip_confirm:
        if arm not in clients:
            parser.error(f"debug flag names {arm!r}, which is not in --clients")

    args.required = None
    if args.require_op:
        args.required = list(smp_decode.REQUIRED)
        for spec in args.require_op:
            group, command = (int(p, 0) for p in spec.split(":", 1))
            args.required.append(
                (smp_decode.COMMAND_NAMES.get((group, command), f"cmd{command}"), {}))

    missing = [str(p) for p in (args.evidence / "a.signed.bin",
                                args.evidence / "b.signed.bin") if not p.exists()]
    for name in clients:
        tool = Path(getattr(args, ARMS[name]["tool"]))
        if not tool.exists():
            missing.append(str(tool))
    if missing:
        print("crosscheck: missing input(s):", file=sys.stderr)
        for item in missing:
            print(f"  - {item}", file=sys.stderr)
        return 2

    hashes = {"A": image_hash(args.evidence / "a.signed.bin"),
              "B": image_hash(args.evidence / "b.signed.bin")}
    if args.dry_run:
        probe = HciCapture(Path("unused.pcapng"), mode=args.capture, btvs=args.btvs,
                           tshark=args.tshark, capinfos=args.capinfos).probe()
        print(json.dumps({"clients": clients, "hashes": hashes,
                          "capture": {"how": probe[0], "detail": probe[1]},
                          "cli": str(args.cli), "cli_exists": args.cli.exists()},
                         indent=2))
        return 0

    run_dir = args.out / time.strftime("%Y%m%d-%H%M%S")
    run_dir.mkdir(parents=True, exist_ok=True)
    bench = Bench(args)
    summary: dict = {"run": run_dir.name, "address": args.address, "hashes": hashes,
                     "clients": clients, "arms": {}, "divergences": [],
                     "self_test": None, "counts": {}}
    try:
        if not (args.no_self_test or args.dry_run):
            port = UartPort(bench, run_dir / "self-test")
            (run_dir / "self-test").mkdir(parents=True, exist_ok=True)
            summary["self_test"] = oracle_self_test(args, bench, port, run_dir, hashes,
                                                    negate=args.self_test_negate)
            ok = summary["self_test"]["ok"]
            print(f"{'pass' if ok else 'FAIL':12} oracle self-test: "
                  f"changed {summary['self_test']['changed']}", flush=True)
            for problem in summary["self_test"]["problems"]:
                print(f"             - {problem}", flush=True)
            if not ok:
                summary["counts"] = {"unavailable": len(clients)}
                return 2
        if args.self_test_only:
            return 0

        for name in clients:
            entry = run_arm(args, bench, name, run_dir, hashes, source="A")
            summary["arms"][name] = entry
            print(f"{entry['verdict']:12} {name}: tier_a={entry['tier_a']} "
                  f"tier_b={entry['tier_b']}"
                  + (f" -- {entry['detail']}" if entry["detail"] else ""), flush=True)
            for divergence in entry["divergences"]:
                print(f"             ! {divergence['checkpoint']} "
                      f"{divergence['path']}: {divergence['left']!r} vs "
                      f"{divergence['right']!r} ({divergence['class']})", flush=True)

        # Cross-client Tier A: the same checkpoint, arm against arm.
        names = [n for n in clients if summary["arms"][n]["checkpoints"]]
        for i, left in enumerate(names):
            for right in names[i + 1:]:
                for checkpoint in ("S0", "S1", "S2"):
                    lhs = summary["arms"][left]["checkpoints"].get(checkpoint)
                    rhs = summary["arms"][right]["checkpoints"].get(checkpoint)
                    if lhs and rhs:
                        summary["divergences"] += diff(left, lhs, right, rhs, checkpoint)
        # Cross-client Tier B, between the BLE arms that decoded.
        decoded = [n for n in names if summary["arms"][n].get("ops")]
        for i, left in enumerate(decoded):
            for right in decoded[i + 1:]:
                summary["divergences"] += smp_decode.compare(
                    left, summary["arms"][left]["ops"], right,
                    summary["arms"][right]["ops"])

        for divergence in summary["divergences"]:
            print(f"DIVERGENCE  {divergence.get('checkpoint', 'tier-B')} "
                  f"{divergence['path']}: {divergence['clients']} "
                  f"{divergence['left']!r} vs {divergence['right']!r}", flush=True)
    finally:
        arms = summary["arms"]
        summary["counts"] = {
            verdict: sum(1 for a in arms.values() if a["verdict"] == verdict)
            for verdict in ("pass", "fail", "unavailable")}
        summary["counts"]["tier_b_unavailable"] = sum(
            1 for a in arms.values() if a["tier_b"] == "unavailable")
        summary["counts"]["divergences"] = len(summary["divergences"])
        summary["inputs"] = {
            name: hashlib.sha256((args.evidence / name).read_bytes()).hexdigest()
            for name in ("a.signed.bin", "b.signed.bin")}
        (run_dir / "summary.json").write_text(json.dumps(summary, indent=2, default=str)
                                              + "\n", encoding="utf-8")
        # Always restore the baseline, so the next run of anything on this bench
        # starts from a known board rather than from whatever this left behind.
        bench.flash_baseline(run_dir / "final-baseline.log")
        print(json.dumps(summary["counts"]), flush=True)

    if summary["counts"]["fail"] or summary["counts"]["divergences"]:
        return 1
    if summary["counts"]["unavailable"] or summary["counts"]["tier_b_unavailable"]:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
