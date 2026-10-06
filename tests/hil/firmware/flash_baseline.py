# SPDX-License-Identifier: Apache-2.0
"""Return the bench peer to its baseline: MCUboot + image A, verified.

This is the bench's one recovery primitive, and the reason every HIL case can run
unattended: whatever a case leaves in flash -- a pending swap, a corrupted slot,
a half-written upload -- a full erase and reprogram over the debug probe undoes
it. One script, one exit-code contract, per bench profile (tests/hil/README.md):

* `wb55` (default) -- NUCLEO-WB55RG over ST-LINK with STM32CubeProgrammer. CPU2
  (the wireless coprocessor) is untouched: its firmware lives above the secure
  flash boundary and is installed once, by hand.
* `bl54l15` -- Ezurio BL54L15 DVK (nRF54L15) over its on-board J-Link with
  `nrfutil device`, from the nRF Connect SDK toolchain.

Exit status: 0 baseline restored and read back; 2 no probe or board; 1 anything
else. A supervisor must treat 2 as "bench unavailable", never as a test result.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

CUBE_CLI = Path(r"C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer"
                r"\bin\STM32_Programmer_CLI.exe")
CUBE_CONNECT = ["-c", "port=swd", "mode=UR", "reset=HWrst"]

# The nRF Connect SDK toolchain bundle ships nrfutil with its `device` command
# installed under its own NRFUTIL_HOME. The bare `nrfutil` on a machine's PATH
# may have a different home with no `device` command at all, which fails with
# "command `device` not found" -- so the default names both.
NCS_TOOLCHAIN = Path(r"C:\ncs\toolchains\936afb6332")
NRFUTIL = NCS_TOOLCHAIN / "nrfutil" / "bin" / "nrfutil.exe"
NRFUTIL_HOME = NCS_TOOLCHAIN / "nrfutil" / "home"
# What nrfutil says when the named probe is not attached. Matched only on the
# first step, so a probe that vanishes mid-flash is a failure, not "no bench".
NO_PROBE = re.compile(r"no devices?\b[^\n]*\bfound|no matching device", re.I)


def run(cmd: list, env=None) -> subprocess.CompletedProcess:
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, env=env)


def flash_wb55(args, mcuboot: Path, app: Path) -> int:
    cli = args.programmer or CUBE_CLI
    if not cli.exists():
        print(f"flash_baseline: STM32CubeProgrammer CLI not found at {cli}", file=sys.stderr)
        return 2
    connect = list(CUBE_CONNECT)
    if args.serial:
        connect.append(f"sn={args.serial}")

    # "-e all" erases CPU1's flash up to the secure flash start (SFSA), so the
    # CPU2 firmware above it is not touched. "-v" reads every programmed word back.
    steps = [connect + ["-e", "all"],
             connect + ["-d", str(mcuboot), "-v", "-d", str(app), "-v"]
             + ([] if args.no_reset else ["-hardRst"])]
    for step in steps:
        result = run([cli, *step])
        out = result.stdout + result.stderr
        if re.search(r"No (ST-LINK|debug probe) detected|Error: No debug probe", out, re.I):
            print("flash_baseline: no ST-LINK probe detected", file=sys.stderr)
            return 2
        if result.returncode != 0 or re.search(r"\bError\b", out):
            print(out, file=sys.stderr)
            print(f"flash_baseline: step failed: {' '.join(step)}", file=sys.stderr)
            return 1
    return 0


def flash_bl54l15(args, mcuboot: Path, app: Path) -> int:
    nrfutil = args.programmer or NRFUTIL
    if not nrfutil.exists():
        print(f"flash_baseline: nrfutil not found at {nrfutil}", file=sys.stderr)
        return 2
    env = dict(os.environ)
    env.setdefault("NRFUTIL_HOME", str(NRFUTIL_HOME))

    # Name the probe on every step. Without --serial-number nrfutil acts on
    # every attached device, and "no probe" must be decided before anything is
    # erased, not inferred from an erase that touched nothing. With --serial
    # the listing is skipped; a serial that is not attached then fails the
    # first step, which is reported as "no probe" below.
    serials = []
    if not args.serial:
        listing = run([nrfutil, "device", "list"], env)
        serials = re.findall(r"^(\d{6,})\s*$", listing.stdout, re.M)
    if args.serial:
        serial = args.serial
    elif len(serials) == 1:
        serial = serials[0]
    elif not serials:
        print("flash_baseline: no J-Link probe detected", file=sys.stderr)
        return 2
    else:
        print(f"flash_baseline: several probes {serials}; pass --serial", file=sys.stderr)
        return 1

    device = [nrfutil, "device"]
    select = ["--serial-number", serial]
    # The application core's access port is protected while MCUboot runs (seen
    # on this bench: an erase issued during the post-reset swap was refused
    # with "access port is protected"), and opened once the application runs.
    # `recover` erases everything through the control port regardless, so it
    # is the fallback when a reflash lands while MCUboot is running.
    recover = device + ["recover", *select]
    # A full erase first, then each hex with ERASE_NONE so the second program
    # does not undo the first. VERIFY_READ reads every programmed byte back.
    program = "chip_erase_mode=ERASE_NONE,verify=VERIFY_READ"
    steps = [device + ["erase", "--all", *select],
             device + ["program", "--firmware", mcuboot, "--options", program, *select],
             device + ["program", "--firmware", app, "--options", program, *select]]
    if not args.no_reset:
        steps.append(device + ["reset", *select])
    for step in steps:
        result = run(step, env)
        out = result.stdout + result.stderr
        if result.returncode != 0 and step is steps[0] and re.search(r"protected", out, re.I):
            # The erase was refused because the access port is protected; see
            # `recover` above.
            print("flash_baseline: access port protected; recovering instead", file=sys.stderr)
            result = run(recover, env)
            out = result.stdout + result.stderr
        if result.returncode != 0:
            print(out, file=sys.stderr)
            print(f"flash_baseline: step failed: {' '.join(str(s) for s in step)}", file=sys.stderr)
            return 2 if step is steps[0] and NO_PROBE.search(out) else 1
    return 0


PROFILES = {"wb55": flash_wb55, "bl54l15": flash_bl54l15}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--evidence", type=Path, required=True,
                        help="the evidence/ directory build_peer.py produced")
    parser.add_argument("--profile", default="wb55", choices=sorted(PROFILES),
                        help="which bench board (default wb55)")
    parser.add_argument("--programmer", "--cli", dest="programmer", type=Path, default=None,
                        help="STM32_Programmer_CLI (wb55) or nrfutil (bl54l15); "
                             "default: the profile's usual install path")
    parser.add_argument("--image", default="a", choices=["a", "b"],
                        help="which signed image to place in slot 0 (default a)")
    parser.add_argument("--serial", default=None, help="probe serial, if several are attached")
    parser.add_argument("--no-reset", action="store_true", help="leave the core halted afterwards")
    args = parser.parse_args()

    mcuboot = args.evidence / "mcuboot.hex"
    app = args.evidence / f"{args.image}.signed.hex"
    for f in (mcuboot, app):
        if not f.exists():
            print(f"flash_baseline: missing {f}", file=sys.stderr)
            return 1

    status = PROFILES[args.profile](args, mcuboot, app)
    if status == 0:
        print(f"flash_baseline: {args.profile}: mcuboot + image {args.image.upper()} "
              "programmed and verified")
    return status


if __name__ == "__main__":
    sys.exit(main())
