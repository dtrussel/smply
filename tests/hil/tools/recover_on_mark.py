# SPDX-License-Identifier: Apache-2.0
"""The BL54L15's give-up case, with the device removed by a J-Link recover.

`hil: reconnection gives up when the device does not come back` needs the
device gone between its HIL-MARK line and its first reconnect attempt, 10 s
later. On the WB55 that is a person pulling the board. On the BL54L15 this
instrument does it: on the mark it runs `nrfutil device recover`, which erases
the whole chip through the control port -- an empty nRF54L15 cannot advertise,
so "never comes back" holds by construction. `recover` rather than `erase`,
because the mark lands while MCUboot is swapping and the application core's
access port is then protected (tests/hil/README.md).

Why not run_hil.py: the same recover issued from the supervisor did not land
inside the window in four runs, for a reason not found; from here it did in
every run. So the give-up case is run with this, and its evidence bundle is
this script's output.

  python tests/hil/tools/recover_on_mark.py --exe build/windows-hil/tests/hil/smply_hil.exe \\
      --evidence <out>/evidence --address F0:71:FF:FF:4E:B1 --serial 1059920902 \\
      --out build/hil-evidence/bl54l15

Restores the baseline afterwards. Exit status: the case's own (0 pass), or 2 if
the recover never succeeded -- then the case's verdict means nothing.
"""
import argparse
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
CASE = "hil: reconnection gives up when the device does not come back"
NRFUTIL = Path(r"C:\ncs\toolchains\936afb6332\nrfutil\bin\nrfutil.exe")
NRFUTIL_HOME = Path(r"C:\ncs\toolchains\936afb6332\nrfutil\home")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--address", required=True)
    parser.add_argument("--serial", required=True, help="the J-Link serial (never 'all probes')")
    parser.add_argument("--nrfutil", type=Path, default=NRFUTIL)
    parser.add_argument("--out", type=Path, default=Path("build/hil-evidence/bl54l15"))
    args = parser.parse_args()

    out = args.out / ("give-up-" + time.strftime("%Y%m%d-%H%M%S"))
    out.mkdir(parents=True)
    env = dict(os.environ, SMPLY_HIL_ADDRESS=args.address,
               SMPLY_HIL_IMAGE_A=str(args.evidence / "a.signed.bin"),
               SMPLY_HIL_IMAGE_B=str(args.evidence / "b.signed.bin"))
    env.setdefault("NRFUTIL_HOME", str(NRFUTIL_HOME))
    flash = [sys.executable, str(HERE / "firmware" / "flash_baseline.py"), "--profile", "bl54l15",
             "--evidence", str(args.evidence), "--serial", args.serial]
    if subprocess.run(flash).returncode != 0:
        print("recover_on_mark: baseline flash failed", file=sys.stderr)
        return 2
    time.sleep(10)  # the Windows-side settle, as run_hil.py --settle 10

    t0 = time.monotonic()
    log = (out / "instrument.log").open("w", encoding="utf-8")
    recovered = threading.Event()

    def note(text: str) -> None:
        line = f"[{time.monotonic() - t0:6.1f}s] {text}"
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    def recover() -> None:
        started = time.monotonic()
        while time.monotonic() - started < 8:
            r = subprocess.run([str(args.nrfutil), "device", "recover", "--serial-number",
                                args.serial], env=env, capture_output=True, text=True)
            note(f"recover exit {r.returncode} after {time.monotonic() - started:.1f}s "
                 f"{(r.stdout + r.stderr).strip()[:300]}")
            if r.returncode == 0:
                recovered.set()
                return

    case = subprocess.Popen([str(args.exe), CASE, "--success"], env=env, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                            errors="replace")
    worker = None
    with (out / "stdout.log").open("w", encoding="utf-8") as stdout:
        for line in case.stdout:
            stdout.write(line)
            if line.startswith("HIL-MARK") and worker is None:
                note("mark")
                worker = threading.Thread(target=recover)
                worker.start()
    case.wait()
    if worker is not None:
        worker.join()
    note(f"case exit {case.returncode}; device removed: {recovered.is_set()}")
    log.close()
    subprocess.run(flash)
    if not recovered.is_set():
        return 2
    return case.returncode


if __name__ == "__main__":
    sys.exit(main())
