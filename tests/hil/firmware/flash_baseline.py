# SPDX-License-Identifier: Apache-2.0
"""Return the NUCLEO-WB55RG to the P17 baseline: MCUboot + image A, verified.

This is the bench's one recovery primitive, and the reason every HIL case can run
unattended: whatever a case leaves in flash -- a pending swap, a corrupted slot,
a half-written upload -- a full CPU1 erase and reprogram over ST-LINK undoes it.
CPU2 (the wireless coprocessor) is untouched: its firmware lives above the secure
flash boundary and is installed once, by hand, per tests/hil/README.md.

Exit status: 0 baseline restored and read back; 2 no probe or board; 1 anything
else. A supervisor must treat 2 as "bench unavailable", never as a test result.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

DEFAULT_CLI = Path(r"C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer"
                   r"\bin\STM32_Programmer_CLI.exe")
CONNECT = ["-c", "port=swd", "mode=UR", "reset=HWrst"]


def run(cli: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run([str(cli), *args], capture_output=True, text=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, required=True,
                        help="the evidence/ directory build_peer.py produced")
    parser.add_argument("--cli", type=Path, default=DEFAULT_CLI)
    parser.add_argument("--image", default="a", choices=["a", "b"],
                        help="which signed image to place in slot 0 (default a)")
    parser.add_argument("--serial", default=None, help="ST-LINK serial, if several are attached")
    parser.add_argument("--no-reset", action="store_true", help="leave the core halted afterwards")
    args = parser.parse_args()

    if not args.cli.exists():
        print(f"flash_baseline: STM32CubeProgrammer CLI not found at {args.cli}", file=sys.stderr)
        return 2
    mcuboot = args.evidence / "mcuboot.hex"
    app = args.evidence / f"{args.image}.signed.hex"
    for f in (mcuboot, app):
        if not f.exists():
            print(f"flash_baseline: missing {f}", file=sys.stderr)
            return 1

    connect = list(CONNECT)
    if args.serial:
        connect.append(f"sn={args.serial}")

    # "-e all" erases CPU1's flash up to the secure flash start (SFSA), so the
    # CPU2 firmware above it is not touched. "-v" reads every programmed word back.
    steps = [connect + ["-e", "all"],
             connect + ["-d", str(mcuboot), "-v", "-d", str(app), "-v"]
             + ([] if args.no_reset else ["-hardRst"])]
    for step in steps:
        result = run(args.cli, *step)
        out = result.stdout + result.stderr
        if re.search(r"No (ST-LINK|debug probe) detected|Error: No debug probe", out, re.I):
            print("flash_baseline: no ST-LINK probe detected", file=sys.stderr)
            return 2
        if result.returncode != 0 or re.search(r"\bError\b", out):
            print(out, file=sys.stderr)
            print(f"flash_baseline: step failed: {' '.join(step)}", file=sys.stderr)
            return 1
    print(f"flash_baseline: mcuboot + image {args.image.upper()} programmed and verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
