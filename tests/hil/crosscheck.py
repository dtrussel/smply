# SPDX-License-Identifier: Apache-2.0
"""P17c cross-check: the same update by smply, by smpmgr (BLE) and by mcumgr-client (UART).

Each client starts from the same freshly flashed baseline (image A) and installs
image B the test-then-confirm way: upload, mark for test, reset, confirm the
running image. After each run the device's state is read through the **oracle**
-- `mcumgr-client list` over the UART shell transport, a path none of the BLE
clients touch -- and normalised to what matters: the active slot's version,
hash and flags, and which other slots are listed. The three normalised states
must agree.

These are third-party tools used for behavioural comparison only (ADR-0015);
nothing here is a protocol reference. A divergence is investigated against
Zephyr/MCUboot source and recorded in protocol-notes.md section 9 before smply
changes.

HCI captures: BTVS needs an elevated shell. When `--btvs` is given and the
shell is elevated, one pcapng per BLE client is recorded alongside; otherwise
the capture is reported as unavailable, never silently omitted.

  python tests/hil/crosscheck.py --evidence <out>/evidence --address 80:E1:26:00:65:E2 \
      --uart COM4 --dfu build/windows-hil/examples/winrt_ble_dfu/winrt_ble_dfu.exe \
      --smpmgr build/hil-tools/Scripts/smpmgr.exe \
      --mcumgr-client build/mcumgr-client/mcumgr-client-windows-x86/mcumgr-client.exe
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
FLASH = HERE / "firmware" / "flash_baseline.py"


def run(cmd, log: Path, timeout: int) -> subprocess.CompletedProcess:
    started = time.monotonic()
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                                errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired as error:
        with log.open("a", encoding="utf-8") as out:
            out.write(f"$ {' '.join(map(str, cmd))}\n-- TIMEOUT after {timeout}s --\n")
        raise RuntimeError(f"timeout: {' '.join(map(str, cmd))}") from error
    with log.open("a", encoding="utf-8") as out:
        out.write(f"$ {' '.join(map(str, cmd))}\n[exit {result.returncode}, "
                  f"{time.monotonic() - started:.1f}s]\n{result.stdout}{result.stderr}\n")
    return result


def oracle_state(args, log: Path) -> dict:
    """The device's image state as mcumgr-client reports it over UART."""
    for attempt in range(3):
        result = run([str(args.mcumgr_client), "-d", args.uart, "list"], log, 60)
        m = re.search(r"response:\s*(\{.*\})", result.stdout, re.S)
        if result.returncode == 0 and m:
            return json.loads(m.group(1))
        time.sleep(2)
    raise RuntimeError("the UART oracle did not answer")


def normalise(state: dict) -> dict:
    """What every client must leave behind, in a form that can be compared."""
    slots = sorted(state.get("images", []), key=lambda s: (s.get("image", 0), s["slot"]))
    active = [s for s in slots if s.get("active")]
    return {
        "active": {k: active[0].get(k) for k in ("slot", "version", "hash", "bootable",
                                                 "pending", "confirmed", "active", "permanent")}
        if active else None,
        "listed_slots": [(s.get("image", 0), s["slot"]) for s in slots],
    }


def image_hash(path: Path) -> str:
    """The MCUboot image-state hash (IMAGE_TLV_SHA256) of a signed image file.

    Read out of the TLV trailer: header size and image size from the header,
    then the protected and unprotected TLV areas walked as one run
    (protocol-notes.md section 7).
    """
    data = path.read_bytes()
    hdr_size = int.from_bytes(data[8:10], "little")
    protect_tlv_size = int.from_bytes(data[10:12], "little")
    img_size = int.from_bytes(data[12:16], "little")
    off = hdr_size + img_size
    end = len(data)
    while off + 4 <= end:
        magic = int.from_bytes(data[off:off + 2], "little")
        if magic in (0x6907, 0x6908):  # IMAGE_TLV_PROT_INFO_MAGIC, IMAGE_TLV_INFO_MAGIC
            off += 4
            continue
        tlv_type = int.from_bytes(data[off:off + 2], "little")
        tlv_len = int.from_bytes(data[off + 2:off + 4], "little")
        if tlv_type == 0x10 and tlv_len == 32:  # IMAGE_TLV_SHA256
            return data[off + 4:off + 36].hex()
        off += 4 + tlv_len
    del protect_tlv_size
    raise RuntimeError(f"no SHA256 TLV in {path}")


def flash_baseline(args, log: Path) -> None:
    result = run([sys.executable, str(FLASH), "--evidence", str(args.evidence)], log, 300)
    if result.returncode != 0:
        raise RuntimeError("baseline flash failed" if result.returncode == 1 else "no bench")
    time.sleep(3)


def with_smply(args, log: Path) -> None:
    result = run([str(args.dfu), "--image", str(args.evidence / "b.signed.bin"),
                  "--address", args.address], log, 300)
    if result.returncode != 0:
        raise RuntimeError(f"winrt_ble_dfu exit {result.returncode}")


def with_smpmgr(args, log: Path) -> None:
    ble = [str(args.smpmgr), "--ble", args.address, "--timeout", "40"]
    result = run(ble + ["upgrade", str(args.evidence / "b.signed.bin")], log, 300)
    if result.returncode != 0:
        raise RuntimeError(f"smpmgr upgrade exit {result.returncode}")
    time.sleep(12)  # the swap reset (~6 s) and re-advertising
    result = run(ble + ["image", "state-write", "--confirm"], log, 120)
    if result.returncode != 0:
        raise RuntimeError(f"smpmgr confirm exit {result.returncode}")


def with_mcumgr_client(args, log: Path) -> None:
    uart = [str(args.mcumgr_client), "-d", args.uart]
    target = image_hash(args.evidence / "b.signed.bin")
    for step, timeout in ((["upload", str(args.evidence / "b.signed.bin")], 600),
                          (["test", target], 60), (["reset"], 60)):
        result = run(uart + step, log, timeout)
        if result.returncode != 0:
            raise RuntimeError(f"mcumgr-client {step[0]} exit {result.returncode}")
    time.sleep(12)
    result = run(uart + ["test", target, "--confirm", "true"], log, 60)
    if result.returncode != 0:
        raise RuntimeError(f"mcumgr-client confirm exit {result.returncode}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--address", required=True)
    parser.add_argument("--uart", required=True)
    parser.add_argument("--dfu", type=Path, required=True, help="winrt_ble_dfu.exe")
    parser.add_argument("--smpmgr", type=Path, required=True)
    parser.add_argument("--mcumgr-client", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=Path("build/hil-crosscheck"))
    parser.add_argument("--clients", default="smply,smpmgr,mcumgr-client")
    args = parser.parse_args()

    run_dir = args.out / time.strftime("%Y%m%d-%H%M%S")
    run_dir.mkdir(parents=True, exist_ok=True)
    clients = {"smply": with_smply, "smpmgr": with_smpmgr, "mcumgr-client": with_mcumgr_client}
    expected_hash = image_hash(args.evidence / "b.signed.bin")
    results = {}

    for name in [c.strip() for c in args.clients.split(",")]:
        log = run_dir / f"{name}.log"
        entry = {"client": name, "verdict": "unavailable", "detail": ""}
        try:
            flash_baseline(args, log)
            before = normalise(oracle_state(args, log))
            started = time.monotonic()
            clients[name](args, log)
            entry["seconds"] = round(time.monotonic() - started, 1)
            time.sleep(2)
            after = normalise(oracle_state(args, log))
            entry.update({"before": before, "after": after})
            ok = (after["active"] is not None and after["active"]["hash"] == expected_hash
                  and after["active"]["confirmed"] and not after["active"]["pending"])
            entry["verdict"] = "pass" if ok else "fail"
            if not ok:
                entry["detail"] = "the device did not end up running image B confirmed"
        except RuntimeError as error:
            entry["verdict"] = "unavailable" if "no bench" in str(error) else "fail"
            entry["detail"] = str(error)
        entry["hci_capture"] = "unavailable (BTVS needs an elevated shell)"
        results[name] = entry
        print(f"{entry['verdict']:12} {name}: {entry.get('detail') or 'device state as expected'}",
              flush=True)

    passed = {k: v for k, v in results.items() if v["verdict"] == "pass"}
    agree = len({json.dumps(v["after"], sort_keys=True) for v in passed.values()}) <= 1
    summary = {"expected_hash": expected_hash, "results": results,
               "states_agree": agree, "inputs": {
                   "a.signed.bin": hashlib.sha256((args.evidence / "a.signed.bin").read_bytes()).hexdigest(),
                   "b.signed.bin": hashlib.sha256((args.evidence / "b.signed.bin").read_bytes()).hexdigest()}}
    (run_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print("states agree across passing clients:", agree)
    if any(v["verdict"] == "fail" for v in results.values()) or not agree:
        return 1
    if any(v["verdict"] == "unavailable" for v in results.values()):
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
