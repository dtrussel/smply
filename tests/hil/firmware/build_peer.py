# SPDX-License-Identifier: Apache-2.0
"""Build the pinned P17 peer for the NUCLEO-WB55RG. Never flashes, never tests.

Two complete `west build --sysbuild` runs of the unmodified smp_svr sample, one
per signed version, so that BOTH images are signed by Zephyr's own signing
command with the geometry the board's device tree and MCUboot's Kconfig dictate
(header size, slot size, write alignment). The previous recipe copied those
numbers by hand from another board; deriving them twice is how that goes wrong.

Output: <build-dir>/evidence/ with the signed images, the bootloader, every
effective Kconfig, both device trees, the frozen manifest, the Python freeze and a
SHA-256 inventory -- everything tests/hil/README.md needs to reproduce a run.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ZEPHYR = "e71ff182603865f59e2e25f05655d6affda4f288"
MCUBOOT = "ee39e2d694bd827ffd1bebbce2f571a9154e6ec2"
HAL_STM32 = "d1d3c0c9ddf697f6bcda911f158777136aa21c5c"
BOARD = "nucleo_wb55rg"
SAMPLE = "zephyr/samples/subsys/mgmt/mcumgr/smp_svr"

# Image A is the baseline the bench is flashed with; B is what every update
# installs. Same source, different signed version, therefore different hashes.
IMAGES = {"a": "1.0.0", "b": "2.0.0"}


def check_pins(workspace: Path, parser: argparse.ArgumentParser) -> None:
    for directory, expected in (
        ("zephyr", ZEPHYR),
        ("bootloader/mcuboot", MCUBOOT),
        ("modules/hal/stm32", HAL_STM32),
    ):
        actual = subprocess.check_output(
            ["git", "-C", str(workspace / directory), "rev-parse", "HEAD"], text=True
        ).strip()
        if actual != expected:
            parser.error(f"{directory}: expected {expected}, got {actual}")


def west_build(workspace: Path, build: Path, env: dict, conf: Path, version: str) -> None:
    subprocess.run(
        [
            sys.executable, "-m", "west", "build", "-b", BOARD, "--sysbuild",
            "-d", str(build), SAMPLE, "--",
            f"-DEXTRA_CONF_FILE=bt.conf;{conf.as_posix()}",
            f'-DCONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="{version}"',
        ],
        cwd=workspace, env=env, check=True,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True,
                        help="west workspace created from tests/hil/firmware/west.yml")
    parser.add_argument("--build-dir", type=Path, required=True,
                        help="root for the per-image build directories and evidence/")
    parser.add_argument("--sdk", type=Path, required=True, help="Zephyr SDK install dir")
    args = parser.parse_args()
    workspace, root = args.workspace.resolve(), args.build_dir.resolve()
    firmware = Path(__file__).resolve().parent
    check_pins(workspace, parser)

    env = dict(os.environ, ZEPHYR_SDK_INSTALL_DIR=str(args.sdk.resolve()),
               ZEPHYR_TOOLCHAIN_VARIANT="zephyr")
    evidence = root / "evidence"
    evidence.mkdir(parents=True, exist_ok=True)

    for tag, version in IMAGES.items():
        build = root / tag
        west_build(workspace, build, env, firmware / "peer.conf", version)
        app = build / "smp_svr/zephyr"
        files = {
            f"{tag}.signed.bin": app / "zephyr.signed.bin",
            f"{tag}.signed.hex": app / "zephyr.signed.hex",
            f"{tag}.application.config": app / ".config",
            f"{tag}.application.dts": app / "zephyr.dts",
        }
        if tag == "a":
            # One bootloader. It is identical between the two builds by
            # construction (same Kconfig, same key); the hash check below is
            # what proves that rather than asserting it.
            files.update({
                "mcuboot.hex": build / "mcuboot/zephyr/zephyr.hex",
                "mcuboot.config": build / "mcuboot/zephyr/.config",
                "mcuboot.dts": build / "mcuboot/zephyr/zephyr.dts",
                "sysbuild.config": build / "zephyr/.config",
            })
        else:
            files["b.mcuboot.hex"] = build / "mcuboot/zephyr/zephyr.hex"
        for name, source in files.items():
            shutil.copyfile(source, evidence / name)

    with (evidence / "west-frozen.yml").open("w") as output:
        subprocess.run([sys.executable, "-m", "west", "manifest", "--freeze"],
                       cwd=workspace, stdout=output, check=True)
    with (evidence / "python-freeze.txt").open("w") as output:
        subprocess.run([sys.executable, "-m", "pip", "freeze"], stdout=output, check=True)

    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
              for p in sorted(evidence.iterdir()) if p.is_file() and p.name != "sha256.json"}
    (evidence / "sha256.json").write_text(json.dumps(hashes, indent=2) + "\n")
    if hashes["mcuboot.hex"] != hashes["b.mcuboot.hex"]:
        sys.exit("the two builds produced different bootloaders; the recipe is not deterministic")
    print(f"Peer artifacts: {evidence}")


if __name__ == "__main__":
    main()
