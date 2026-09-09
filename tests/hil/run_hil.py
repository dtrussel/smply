# SPDX-License-Identifier: Apache-2.0
"""The HIL supervisor: baseline, capture, one case at a time, verdict.

Runs the `smply_hil` cases against the bench described in tests/hil/README.md,
wrapping each *group* of cases in a baseline reflash over ST-LINK and a console
UART capture, giving every case a hard deadline, and keeping its stdout, the
device's log and the Catch2 report under <out>/<run>/<case>/.

Verdicts are **pass**, **fail** or **unavailable** (ADR-0015): a case that
SKIPped because it found no bench, a reflash that found no probe, or a run that
never produced a report is unavailable -- never a pass, never a fail.

  python tests/hil/run_hil.py --exe build/windows-hil/tests/hil/smply_hil.exe \
      --evidence build/hil-peer-out/evidence --address 80:E1:26:00:65:E2 --uart COM4

Exit status: 0 every selected case passed; 1 at least one failed; 2 at least one
was unavailable and none failed.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
FLASH = HERE / "firmware" / "flash_baseline.py"
UART_LOG = HERE / "tools" / "uart_log.py"
CUBE_CLI = Path(r"C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer"
                r"\bin\STM32_Programmer_CLI.exe")

# Groups run from one baseline flash; a case inside a group relies on what the
# previous one left behind (the restart pair), so order matters within a group.
GROUPS = [
    {"name": "presence", "cases": ["hil: the bench answers params and slot info and echo"]},
    {"name": "clean-update", "cases": ["hil: a clean update installs the other image and confirms it"]},
    {"name": "confirm-immediately",
     "cases": ["hil: confirm-immediately installs without the confirmation pause"]},
    {"name": "already-running", "cases": ["hil: an image the device already runs is not uploaded again"]},
    {"name": "already-present",
     "cases": ["hil: the server's own already-present check completes a re-upload on the first packet"]},
    {"name": "interrupted", "cases": ["hil: an interrupted upload completes after a reconnect and resume"]},
    {"name": "restart", "cases": ["hil: part 1 -- an upload is abandoned by a process that exits",
                                  "hil: part 2 -- a new process resumes the abandoned upload by sha"]},
    {"name": "corrupt", "cases": ["hil: a corrupted image is refused and the device keeps running what it had"]},
    {"name": "rollback", "cases": ["hil: a trial boot that nobody confirms is reverted on the next reset"]},
    {"name": "reset", "cases": ["hil: a reset drops the link and the device comes back"]},
    {"name": "erase", "cases": ["hil: erase clears the secondary slot even when it is marked for test"]},
]

# Manual cases: not in the unattended default (an automated device-removal races
# the reconnect; see the case comment). Run one with `--cases give-up`; a person
# powers the board off on the HIL-MARK line. `--cases all` does NOT include these.
# `manual` is what the notice below is printed from -- it replaced an
# `erase-on-marker` fault the supervisor used to inject with the programmer,
# which contradicted both this comment and testing.md and was dead code.
MANUAL_GROUPS = [
    {"name": "give-up", "manual": True,
     "cases": ["hil: reconnection gives up when the device does not come back"]},
]


def slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")[:60]


class Bench:
    def __init__(self, args):
        self.args = args
        self.python = args.python or sys.executable

    def flash_baseline(self, log: Path) -> str:
        """'ok', 'unavailable' or 'failed'."""
        result = subprocess.run([self.python, str(FLASH), "--evidence", str(self.args.evidence),
                                 "--cli", str(self.args.cli)], capture_output=True, text=True)
        log.write_text(result.stdout + result.stderr, encoding="utf-8")
        return {0: "ok", 2: "unavailable"}.get(result.returncode, "failed")

    def start_uart(self, out: Path):
        if not self.args.uart:
            return None
        return subprocess.Popen([self.python, str(UART_LOG), "--port", self.args.uart, "--out", str(out)],
                                stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    @staticmethod
    def stop_uart(proc) -> None:
        if proc is None:
            return
        try:
            proc.stdin.close()
            proc.wait(timeout=10)
        except Exception:  # noqa: BLE001 -- best effort; the logger is not the test
            proc.kill()


def run_case(bench: Bench, case: str, case_dir: Path) -> dict:
    case_dir.mkdir(parents=True, exist_ok=True)
    junit = case_dir / "report.xml"
    env = dict(os.environ,
               SMPLY_HIL_ADDRESS=bench.args.address,
               SMPLY_HIL_IMAGE_A=str(bench.args.evidence / "a.signed.bin"),
               SMPLY_HIL_IMAGE_B=str(bench.args.evidence / "b.signed.bin"))
    command = [str(bench.args.exe), case, "--reporter", f"junit::out={junit}",
               "--reporter", "console::out=-", "--success"]
    started = time.monotonic()
    proc = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace", bufsize=1)
    lines: list[str] = []

    def pump() -> None:
        for line in proc.stdout:
            lines.append(line)

    reader = threading.Thread(target=pump, daemon=True)
    reader.start()
    timed_out = False
    try:
        proc.wait(timeout=bench.args.case_timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.kill()
        proc.wait()
    reader.join(timeout=5)
    (case_dir / "stdout.log").write_text("".join(lines), encoding="utf-8")

    metrics = {}
    for line in lines:
        m = re.match(r"HIL-METRIC (\S+)=(-?\d+)", line.strip())
        if m:
            metrics[m.group(1)] = int(m.group(2))

    verdict = "unavailable"
    detail = ""
    if timed_out:
        verdict, detail = "fail", f"no result within {bench.args.case_timeout}s (killed)"
    elif junit.exists():
        try:
            root = ET.parse(junit).getroot()
            cases = list(root.iter("testcase"))
            if not cases:
                verdict, detail = "unavailable", "no testcase in the report"
            elif any(c.find("skipped") is not None for c in cases):
                skipped = next(c.find("skipped") for c in cases if c.find("skipped") is not None)
                verdict, detail = "unavailable", (skipped.get("message") or "skipped")
            elif any(c.find("failure") is not None or c.find("error") is not None for c in cases):
                verdict, detail = "fail", f"exit {proc.returncode}"
            else:
                verdict, detail = "pass", ""
        except ET.ParseError as error:
            verdict, detail = "unavailable", f"unreadable report: {error}"
    else:
        verdict, detail = "unavailable", f"no report written (exit {proc.returncode})"
    return {"case": case, "verdict": verdict, "detail": detail, "exit": proc.returncode,
            "seconds": round(time.monotonic() - started, 1), "metrics": metrics}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path, required=True, help="smply_hil executable")
    parser.add_argument("--evidence", type=Path, required=True,
                        help="build_peer.py's evidence/ directory (images and mcuboot.hex)")
    parser.add_argument("--address", required=True, help="the peer's Bluetooth address")
    parser.add_argument("--uart", default=None, help="the peer's console COM port, e.g. COM4")
    parser.add_argument("--out", type=Path, default=Path("build/hil-runs"))
    parser.add_argument("--cases", default="all",
                        help="'all', or a comma-separated list of group names or case names")
    parser.add_argument("--no-flash", action="store_true",
                        help="do not reflash the baseline between groups (debugging only)")
    parser.add_argument("--cli", type=Path, default=CUBE_CLI, help="STM32CubeProgrammer CLI")
    parser.add_argument("--python", default=None, help="interpreter for the helper scripts")
    parser.add_argument("--case-timeout", type=int, default=600, help="seconds per case")
    args = parser.parse_args()

    if not args.exe.exists():
        print(f"run_hil: {args.exe} does not exist -- build the windows-hil preset first",
              file=sys.stderr)
        return 2

    wanted = None if args.cases == "all" else {x.strip() for x in args.cases.split(",")}
    catalogue = GROUPS + MANUAL_GROUPS
    groups = [g for g in (GROUPS if wanted is None else catalogue)
              if wanted is None or g["name"] in wanted or any(c in wanted for c in g["cases"])]
    if wanted is not None:
        for g in groups:
            g["cases"] = [c for c in g["cases"] if g["name"] in wanted or c in wanted]

    run_dir = args.out / time.strftime("%Y%m%d-%H%M%S")
    run_dir.mkdir(parents=True, exist_ok=True)
    bench = Bench(args)
    results = []

    for group in groups:
        group_dir = run_dir / group["name"]
        group_dir.mkdir(parents=True, exist_ok=True)
        if group.get("manual"):
            print(f"run_hil: {group['name']} needs a person at the bench -- power the "
                  "board off when the case prints its HIL-MARK line", flush=True)
        if not args.no_flash:
            flashed = bench.flash_baseline(group_dir / "flash-baseline.log")
            if flashed != "ok":
                for case in group["cases"]:
                    results.append({"case": case, "verdict": "unavailable" if flashed == "unavailable" else "fail",
                                    "detail": f"baseline flash {flashed}", "metrics": {}})
                if flashed == "unavailable":
                    break
                continue
            time.sleep(3)  # let the peer boot and start advertising
        uart = bench.start_uart(group_dir / "uart.log")
        try:
            for case in group["cases"]:
                result = run_case(bench, case, group_dir / slug(case))
                results.append(result)
                print(f"{result['verdict']:12} {result.get('seconds', 0):6}s  {case}"
                      + (f"  -- {result['detail']}" if result.get("detail") else ""), flush=True)
                if result["verdict"] == "unavailable" and "bench unavailable" in result.get("detail", ""):
                    break
        finally:
            Bench.stop_uart(uart)

    if not args.no_flash and results:
        # Leave the bench as the next run expects to find it, whatever the last
        # case did -- including the give-up group, which erased it.
        bench.flash_baseline(run_dir / "final-baseline.log")

    summary = {"run": run_dir.name, "address": args.address, "results": results,
               "counts": {v: sum(1 for r in results if r["verdict"] == v)
                          for v in ("pass", "fail", "unavailable")}}
    (run_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary["counts"]))
    if summary["counts"]["fail"]:
        return 1
    if summary["counts"]["unavailable"]:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
