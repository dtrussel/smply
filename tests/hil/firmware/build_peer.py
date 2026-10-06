# SPDX-License-Identifier: Apache-2.0
"""Build the pinned bench peer for one bench profile. Never flashes, never tests.

Two complete `west build --sysbuild` runs of the unmodified smp_svr sample, one
per signed version, so that BOTH images are signed by Zephyr's own signing
command with the geometry the board's device tree and MCUboot's Kconfig dictate
(header size, slot size, write alignment). The previous recipe copied those
numbers by hand from another board; deriving them twice is how that goes wrong.

Profiles (tests/hil/README.md):

* `wb55` (default) -- NUCLEO-WB55RG, upstream Zephyr pinned by
  `firmware/west.yml`, built with a Zephyr SDK given by `--sdk`.
* `bl54l15` -- Ezurio BL54L15 DVK (nRF54L15), the nRF Connect SDK workspace
  named by `--workspace` and the NCS toolchain bundle named by `--toolchain`,
  with `firmware/bl54l15/peer.conf` and `sysbuild.conf`.

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

FIRMWARE = Path(__file__).resolve().parent
SAMPLE = "zephyr/samples/subsys/mgmt/mcumgr/smp_svr"

PROFILES = {
    "wb55": {
        "board": "nucleo_wb55rg",
        "pins": (("zephyr", "e71ff182603865f59e2e25f05655d6affda4f288"),
                 ("bootloader/mcuboot", "ee39e2d694bd827ffd1bebbce2f571a9154e6ec2"),
                 ("modules/hal/stm32", "d1d3c0c9ddf697f6bcda911f158777136aa21c5c")),
        "conf": [FIRMWARE / "peer.conf"],
        "sysbuild_conf": None,
        "modules": [],
        "freeze": True,
    },
    # nRF Connect SDK v3.3.0. The pins are the tagged revisions of the
    # repositories whose code reaches the peer's MCUmgr server and bootloader.
    "bl54l15": {
        "board": "bl54l15_dvk/nrf54l15/cpuapp",
        "pins": (("nrf", "ba167d9f3db4abbdc9b67887ca3ea66c64f2d956"),
                 ("zephyr", "fd9204a02d52630660ce8d729945a4dd743feabf"),
                 ("bootloader/mcuboot", "fce4dac2e6295cf98bd02a85f7c4254f6102106d"),
                 ("modules/hal/nordic", "1acb428a205bad58f3dfd4e38f2d1663bb784ba1")),
        "conf": [FIRMWARE / "bl54l15" / "peer.conf"],
        "sysbuild_conf": FIRMWARE / "bl54l15" / "sysbuild.conf",
        # The bench-only settings handler; the sample itself stays unmodified.
        "modules": [FIRMWARE / "bl54l15" / "bench_module"],
        # An NCS install leaves some manifest projects uncloned, and
        # `west manifest --freeze` refuses those; `west list` records the
        # revision of every project that is there.
        "freeze": False,
    },
}

# Image A is the baseline the bench is flashed with; B is what every update
# installs. Same source, different signed version, therefore different hashes.
IMAGES = {"a": "1.0.0", "b": "2.0.0"}


def check_pins(workspace: Path, pins, parser: argparse.ArgumentParser) -> None:
    for directory, expected in pins:
        actual = subprocess.check_output(
            ["git", "-C", str(workspace / directory), "rev-parse", "HEAD"], text=True
        ).strip()
        if actual != expected:
            parser.error(f"{directory}: expected {expected}, got {actual}")


def ncs_environment(toolchain: Path) -> tuple[dict, str]:
    """The environment the NCS toolchain bundle describes, and its Python.

    Read from the bundle's own `environment.json` rather than copied here, so a
    different bundle brings its own paths. Only the two entry types that file
    uses are understood; anything else is refused rather than guessed at.
    """
    spec = json.loads((toolchain / "environment.json").read_text())
    env = dict(os.environ)
    for var in spec["env_vars"]:
        if var["type"] == "string":
            env[var["key"]] = var["value"]
        elif var["type"] == "relative_paths":
            value = os.pathsep.join(str(toolchain / p) for p in var["values"])
            if var.get("existing_value_treatment") == "prepend_to" and env.get(var["key"]):
                value += os.pathsep + env[var["key"]]
            env[var["key"]] = value
        else:
            sys.exit(f"{toolchain}/environment.json: unknown entry type {var['type']!r}")
    return env, str(toolchain / "opt" / "bin" / "python.exe")


def west_build(python: str, workspace: Path, build: Path, env: dict, profile: dict,
               version: str) -> None:
    conf = ";".join(["bt.conf", *(c.as_posix() for c in profile["conf"])])
    extra = [f"-DSB_EXTRA_CONF_FILE={profile['sysbuild_conf'].as_posix()}"] \
        if profile["sysbuild_conf"] else []
    if profile["modules"]:
        extra.append("-DEXTRA_ZEPHYR_MODULES="
                     + ";".join(m.as_posix() for m in profile["modules"]))
    subprocess.run(
        [
            python, "-m", "west", "build", "-p", "always", "-b", profile["board"], "--sysbuild",
            "-d", str(build), SAMPLE, "--",
            f"-DEXTRA_CONF_FILE={conf}", *extra,
            f'-DCONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="{version}"',
        ],
        cwd=workspace, env=env, check=True,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--profile", default="wb55", choices=sorted(PROFILES))
    parser.add_argument("--workspace", type=Path, required=True,
                        help="wb55: the west workspace created from tests/hil/firmware/west.yml; "
                             "bl54l15: the NCS workspace, e.g. C:/ncs/v3.3.0")
    parser.add_argument("--build-dir", type=Path, required=True,
                        help="root for the per-image build directories and evidence/")
    parser.add_argument("--sdk", type=Path, help="wb55: Zephyr SDK install dir")
    parser.add_argument("--toolchain", type=Path,
                        help="bl54l15: NCS toolchain bundle, e.g. C:/ncs/toolchains/936afb6332")
    args = parser.parse_args()
    profile = PROFILES[args.profile]
    workspace, root = args.workspace.resolve(), args.build_dir.resolve()
    check_pins(workspace, profile["pins"], parser)

    if args.profile == "wb55":
        if not args.sdk:
            parser.error("--sdk is required for the wb55 profile")
        env = dict(os.environ, ZEPHYR_SDK_INSTALL_DIR=str(args.sdk.resolve()),
                   ZEPHYR_TOOLCHAIN_VARIANT="zephyr")
        python = sys.executable
    else:
        if not args.toolchain:
            parser.error("--toolchain is required for the bl54l15 profile")
        env, python = ncs_environment(args.toolchain.resolve())
    evidence = root / "evidence"
    # Start from an empty evidence/: sha256.json hashes whatever is in it, so
    # a file left by an earlier (or failed) run would be inventoried as if
    # this build had produced it.
    if evidence.exists():
        shutil.rmtree(evidence)
    evidence.mkdir(parents=True)

    for tag, version in IMAGES.items():
        build = root / tag
        west_build(python, workspace, build, env, profile, version)
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
            # NCS lays the slots out with its partition manager, when enabled;
            # the resolved map is the geometry a reader of a run needs.
            if (build / "partitions.yml").exists():
                files["partitions.yml"] = build / "partitions.yml"
        else:
            files["b.mcuboot.hex"] = build / "mcuboot/zephyr/zephyr.hex"
        for name, source in files.items():
            shutil.copyfile(source, evidence / name)

    if profile["freeze"]:
        with (evidence / "west-frozen.yml").open("w") as output:
            subprocess.run([python, "-m", "west", "manifest", "--freeze"],
                           cwd=workspace, env=env, stdout=output, check=True)
    else:
        with (evidence / "west-list.txt").open("w") as output:
            subprocess.run([python, "-m", "west", "list", "-f", "{name} {path} {revision} {sha}"],
                           cwd=workspace, env=env, stdout=output, check=True)
    with (evidence / "python-freeze.txt").open("w") as output:
        subprocess.run([python, "-m", "pip", "freeze"], env=env, stdout=output, check=True)

    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
              for p in sorted(evidence.iterdir()) if p.is_file() and p.name != "sha256.json"}
    (evidence / "sha256.json").write_text(json.dumps(hashes, indent=2) + "\n")
    if hashes["mcuboot.hex"] != hashes["b.mcuboot.hex"]:
        sys.exit("the two builds produced different bootloaders; the recipe is not deterministic")
    print(f"Peer artifacts: {evidence}")


if __name__ == "__main__":
    main()
