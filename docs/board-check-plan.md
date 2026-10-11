# Package board check

Status: ready for implementation. Written 2026-10-11 from a design interview.
Not started: the roadmap's backlog row ("Core library and API") points here and
keeps the trigger, "when a product ships several boards from one tool". Delete
this file, and that row, in the commit that lands the work.

## Problem Statement

A person updating a device with `cli_dfu` or `serial_dfu --package` can hand it
a package built for a different board. Nothing stops them. smply reads the
package, uploads every image in full, and resets the device. The mismatch shows
only when the device's MCUboot refuses the signature, or worse, when an image
that happens to verify boots on the wrong hardware. The package's manifest
records the board of every image, and smply throws that away. A tool that ships
packages for several boards has no way to say "this device is an `nrf54l15dk`"
and have a wrong package refused before the first byte is sent.

## Solution

The caller states the expected **board name** (protocol-notes §5). smply then
refuses, before anything is sent, a package in which any image was built for
another board or does not say which board it was built for. The comparison is
by board name, because nRF Connect SDK's manifest writes a bare board name for
the application image and a **board target** for a network-core image. Each
side is cut at its first `/` or `@`, so a caller may also pass a board target.
The refusal names the image and both boards. On the command line this is
`--board NAME` on `cli_dfu` and `serial_dfu`.

## User Stories

1. As a person running `serial_dfu`, I want to pass `--board nrf54l15dk` with a package, so that a package for another board is refused before the upload starts.
2. As a person running `cli_dfu`, I want the same `--board` option, so that the portable tool and the serial tool behave alike.
3. As a person who copied the board target from a build log (`nrf54l15dk/nrf54l15/cpuapp`), I want it accepted as the expectation, so that I do not have to know which half of it the manifest uses.
4. As a person whose board has a revision (`nrf54l15dk@1.0.0/…`), I want the revision ignored, so that a package built without naming the revision still matches.
5. As a person updating a dual-core device, I want a package whose network-core image names `nrf5340dk/nrf5340/cpunet` to match `--board nrf5340dk`, so that a correct dual-core package is not refused.
6. As a person who was handed a package for the wrong board, I want the refusal to name the image, the board I expected and the board the package names, so that I can tell which file is wrong.
7. As a person whose package has a correct image 0 and a wrong image 1, I want it refused, so that a partly wrong package never reaches the device.
8. As a person whose package's manifest does not record a board for some image, I want it refused when I asked for the check, so that "unknown" is never taken as "matches".
9. As a person who passes `--board` with `--image` or a build pair rather than a package, I want a usage error, so that I am not told the board was checked when nothing was compared.
10. As a person who passes `--board ""` or `--board /cpuapp`, I want a usage error, so that an empty expectation cannot match an empty board.
11. As a person who omits `--board`, I want the tools to behave exactly as today, so that existing scripts keep working.
12. As a person updating a direct-XIP device from a direct-XIP package, I want the check to cover both builds of the image, so that a pair with one build for another board is refused.
13. As an application developer using `smply::dfu_app`, I want `UpdateInputs::from_package` and `from_package_bytes` to take the expected board, so that an inputs object for the wrong board never exists.
14. As an application developer with my own package handling, I want `dfu_package::check_board` as a free function over a `DfuPackage`, so that I can apply the same rule without `UpdateInputs`.
15. As an application developer, I want a mismatch reported as `InvalidArgument`, consistent with the other packages smply will not take, so that my error handling needs no new code.
16. As a maintainer, I want the comparison rules in one function with unit tests, so that the board-name rule is defined in one place.
17. As a maintainer, I want the core library untouched, so that `FirmwareUpdater` and `ImageTarget` stay free of file formats (ADR-0016, ADR-0021).
18. As a maintainer, I want the reader to refuse a direct-XIP pair whose builds disagree on board, as it already does on version, so that `PackageImage::board` speaks for both builds.
19. As a maintainer, I want the check fuzzed alongside the reader, so that device-independent but untrusted manifest strings cannot crash it.
20. As a reviewer, I want ctests that show the tool completing with a matching board and failing before any upload with a mismatched one, so that the end-to-end behaviour is pinned.

## Implementation Decisions

- **Terms.** "Board name" and "board target" are defined in protocol-notes §5. Use them in comments, help text and messages. Do not say "board" alone where the distinction matters.
- **Comparison.** Cut each string at its first `/` or `@`, then compare what is left exactly and case-sensitively. No other normalisation: no case folding, no alias handling. Zephyr's board aliases are a build-time concept the manifest does not record.
- **`dfu_package::check_board(const DfuPackage&, std::string_view expected) -> Result<void>`**, a new free function in the package reader's module.
  - `InvalidArgument` if `expected` is empty, or empty once cut.
  - `InvalidArgument` if any image's `board` is absent. The message names the image index.
  - `InvalidArgument` if any image's board name differs. The message names the image index, the expected board name and the board string as the manifest wrote it.
  - Every image is checked, not only image 0.
  - A package with no images is not reachable: `read_package()` refuses one.
- **`read_package()`** refuses a direct-XIP pair whose two builds' `board` values differ, as `MalformedMessage`, beside the existing version-equality refusal. One present and one absent counts as differing. `PackageBuild` gains no `board` field. Update the function's documented refusals.
- **`UpdateInputs::from_package(path, expected_board)` and `from_package_bytes(bytes, expected_board)`** take `std::optional<std::string>` (or an equivalent optional view), defaulted to none.
  - With a value, they read the package, then call `check_board`, and return its error unchanged.
  - Without one, they behave as today.
  - `from_files` gains no parameter.
- **`cli_dfu` and `serial_dfu`** gain `--board NAME`. The help text says it takes a board name or board target. It is a usage error together with any non-package input: `--image`, a build pair, or the stub's built-in single image. It is passed to whichever package factory the tool calls, including `--demo-package` and `--demo-xip-package` in `cli_dfu`.
- **The stub's demo packages** gain a `board` per file in their manifests, in nRF Connect SDK's shape. Image 0 gets a bare board name; image 1 gets a board target with the same board name and a different qualifier. The demo direct-XIP package gets the same board name on both builds. The demo packages invent the board name (a neutral name, documented as invented).
- **Core untouched.** No change to `FirmwareUpdater`, `ImageTarget`, `Refusal` or `ErrorCode`.
- **No ADR.** This is support code, not installed, and cheap to reverse (`docs/agents/domain.md`'s test).
- **Documentation in the same change:**
  - the `dfu_package.hpp` and `update_inputs.hpp` comments;
  - the examples' usage text and README, if they list options;
  - `architecture.md` §10's description of `support/` if it lists what `UpdateInputs` does;
  - `testing.md` if it enumerates the examples' ctests;
  - delete the roadmap row and this plan.

## Testing Decisions

- **Test the external behaviour of each seam.** Call the public function with a built package and assert the result and the message's key content: the image index and both boards. Do not assert the exact wording beyond those, and do not test the cutting helper separately.
- **`dfu_package` module**, in the reader's unit tests, building packages with the zip builder as the existing reader tests do:
  - a match by bare board name;
  - a match when the manifest has a board target;
  - a match when the expectation is a board target;
  - a revision (`@`) ignored on either side;
  - a mismatch on image 0;
  - a mismatch on image 1 only;
  - a missing `board` on one image;
  - an empty expectation, and one empty once cut;
  - a direct-XIP pair whose builds disagree on board, or where one omits it: `MalformedMessage` from `read_package()`. Prior art: "the two builds of a direct-XIP image must be the same version".
- **`UpdateInputs`**, in its unit tests, through `from_package_bytes`. A matching expectation yields the same targets as no expectation, and a mismatch yields the `check_board` error and no inputs. Prior art: "a package becomes one target per image, image 0 committed by the client".
- **`cli_dfu` ctests**, in the style of `cli_dfu_mode_refused`, with `PASS_REGULAR_EXPRESSION`:
  - `--demo-package --board <the demo's board>` completes;
  - `--demo-package --board other` fails with the refusal, and the pattern shows no upload began;
  - `--board` without a package is a usage error.
- **Fuzzing**: the package fuzz target also calls `check_board` with a fixed expectation on every package it reads successfully. The result is ignored; the target is only to run the check over arbitrary manifests.
- **No simulator or stub-device changes.** The check runs before any updater exists.

## Out of Scope

- **Asking the device for its board** through the OS information command. It stays a backlog row: it is off by default, and its trigger has not arrived.
- **`winrt_ble_dfu`**, which has no `--package` (its own backlog row).
- **Checking `soc`**, or telling an application-core image from a network-core one. MCUboot's slot and signature checks already do that.
- **Single-file updates.** An MCUboot image carries no board, so there is nothing to compare.
- **Authenticating the manifest.** A tampered manifest can name any board. The check catches mistakes, not attacks (the "package manifest is not authenticated" backlog row).
- **Board aliases and case-insensitive matching.**

## Further Notes

- The facts behind the comparison rule are protocol-notes §5 (sources S59 and S60, read from Zephyr and nRF Connect SDK `main` on 2026-10-11). The manifest writes the application image's `CONFIG_BOARD`, but the network-core image's `BOARD` as supplied. A device's `info` reports `CONFIG_BOARD_TARGET`.
- The commit message carries the reasoning (ADR-0018), including why the comparison is by board name and why a missing board is refused.
