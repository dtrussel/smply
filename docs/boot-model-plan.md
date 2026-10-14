# One boot model behind both device fakes

Status: ready for implementation. Written 2026-10-14 from an architecture
review. The roadmap's backlog row "One boot model behind both device fakes"
points here. Work the stages below in order of their blockers, and check
each one off in the commit that lands it. This file is deleted with the last
stage
([ADR-0018](decisions/ADR-0018-maintenance-process.md)).

## Problem Statement

smply's update logic is proven without hardware against two pretend devices:
`ServerSimulator`, which the component suites trust, and the stub device, which
the example tools (`cli_dfu`, `serial_dfu`) run against in CI. Each holds its
own copy of what MCUboot and Zephyr's image group do with the slots: the swap
types, the trial, pending and confirmed flags derived from them, the reboot
transitions, and the apply and commit of an image the device commits itself.
The simulator alone also has direct-XIP, overwrite, downgrade prevention and
the A27 confirm-denial rules.

For a maintainer, that means:

* **The copies have drifted.** The stub does not refuse a confirm of image 1
  (A27). Its own header and `testing.md` say it only *reports* the MCUboot
  mode it is given and swaps with revert whatever it claims. So
  `cli_dfu --stub-mode direct-xip-with-revert` shows a direct-XIP mode
  reported, never a direct-XIP update carried out, and an upgrade-only demo
  still swaps.
* **A boot rule can only be tested over the wire.** In the simulator, the rules
  of protocol-notes §7 are tangled with SMP decoding, the upload session and
  fault injection. Checking one means encoding a set-state request and decoding
  a slot table, so a rule bug is diagnosed through CBOR.
* **The next mode lands in two places.** Every hardware-free backlog row on
  bootloader modes (single slot, RAM load, the direct-XIP image that is not
  newer) would change those rules, in both copies.

## Solution

One pure **boot model**: the slot table of a device's images and what the
bootloader and the image group do to it, with no wire, no thread and no clock.
Both fakes become wire adapters over it:

* `ServerSimulator` keeps the SMP framing, the upload session (offset
  correction, resume by `sha`, the already-present check, `match`), the
  scripted faults, the error shapes and the response delay. Every boot
  decision is the model's.
* The stub device keeps its thread, its five commands and its happy-path
  upload. It gets its slot table, flags, set-state answers and reboot from the
  model, built in **the mode it reports**. A direct-XIP stub then runs from
  either slot and uploads to the one not running. An upgrade-only stub
  overwrites, and a stub with two images refuses a client confirm of image 1
  as Zephyr's defaults do.

Each rule of protocol-notes §7 (and the multi-image rules of §6) then has a
unit test at the model's interface: a slot table in, an operation, a slot table
or an `ImageError` out.

## User Stories

1. As a maintainer, I want each MCUboot and image-group boot rule written once, so that a correction to one rule reaches every fake device.
2. As a maintainer, I want to test a boot rule without encoding an SMP request, so that a rule bug is diagnosed in the rule and not through CBOR.
3. As a maintainer, I want the boot model's tests to read like protocol-notes §7, one case per rule, so that a reviewer can check them against the source citations.
4. As a maintainer, I want `ServerSimulator`'s answers to every existing request to stay byte-identical, so that the 35 simulator tests and the component suites prove the extraction changed nothing.
5. As a maintainer, I want the simulator to shrink to the wire, the upload session and the faults, so that each of its parts can be read on its own.
6. As a maintainer adding a bootloader mode, I want one module to change, so that the simulator and the stub cannot disagree about it.
7. As a maintainer, I want the boot model to read slot contents with its own image reader, independent of `src/image/`, so that a test cannot pass because the parser under test and the oracle agree on a misreading.
8. As a maintainer, I want the boot model to link only smply's public headers, so that an example may use it, as the build rules for examples require.
9. As a maintainer, I want the boot model in `support/`, not `tests/`, so that the examples can link it while it stays out of the installed package (ADR-0016).
10. As a user of `cli_dfu`, I want `--stub-mode direct-xip-with-revert --demo-builds` to carry out a real direct-XIP update on the stub, so that the demo shows the build for the free slot being sent and the device booting from it.
11. As a user of `cli_dfu`, I want `--stub-mode upgrade-only --allow-no-revert` to overwrite the image, so that the demo shows an update with nothing to revert.
12. As a user of `cli_dfu`, I want `--stub-mode direct-xip` (without revert) to show set-state as not supported, so that the demo shows the mode the updater has to cope with.
13. As a user of `cli_dfu` or `serial_dfu` with a package, I want the stub's image 1 to refuse a client confirm, so that `--commit 1=client` fails as it would against a default Zephyr build (A27).
14. As a user of `cli_dfu`, I want `--stub-mode` described as the mode the stub *runs*, so that the usage text no longer says "it still swaps".
15. As a test author, I want to configure a boot model with the device's build (mode, number of images, downgrade prevention, the three A27 Kconfigs, which images the device commits), so that one description sets up a device for both a unit test and a fake.
16. As a test author, I want to load a slot with image bytes and start a direct-XIP device in either slot, so that a test can set up any starting point the simulator can today.
17. As a test author, I want the model to say which slot an upload of an image writes to, or that the device has no slot for it, so that the upload target rule (secondary, or the direct-XIP slot not running) is decided in one place.
18. As a test author, I want starting an upload to erase the target slot, and its direct-XIP boot state with it, so that a fake cannot forget the trailer erase.
19. As a test author, I want set-state by hash and the hashless confirm to return the `ImageError` Zephyr returns, so that the denial rules (test on the active slot, already pending, A27) are tested directly.
20. As a test author, I want the model to say whether set-state exists in the current mode, so that a fake answers `ENOTSUP` for plain direct-XIP without deciding that itself.
21. As a test author, I want an erase of a slot marked for the next boot to be refused by the model, so that the erase rule lives with the swap state it reads.
22. As a test author, I want a reboot to apply every image's swap type, overwrite, downgrade prevention or direct-XIP choice, so that each reboot transition is one unit test.
23. As a test author, I want reading the state to advance any apply or commit a device-committed image has in progress, so that "each state read is a while passing" stays one rule.
24. As a test author, I want the set-state answer's slot table without advancing an apply, so that the fakes return the refreshed table exactly as they do today.
25. As a test author, I want each slot entry to carry the image number, the slot, the version string, the hash, `bootable` and the four flags, so that both fakes encode the same listing.
26. As a test author, I want an invalid slot content left out of the listing, so that the "invalid images are skipped" rule is the model's.
27. As a test author, I want to query each image's swap type and the direct-XIP active slot, so that the simulator's existing inspection methods keep working.
28. As a test author, I want `ServerSimulator`'s public interface (`ServerConfig`, `SwapType`, `ApplyOutcome`, `CommitOutcome`, `load_slot`, `reboot`, `device_commits`, `boot_from_slot`, `swap_type`, `active_slot`) unchanged, so that no existing test is edited for this refactor.
29. As a test author, I want the stub device's constructor unchanged, so that the examples keep building the device the way they do.
30. As an agent working the plan, I want the docs that describe the two fakes (`testing.md` §2 and §3, `architecture.md`'s layout tree, the stub's header) updated in the same change, so that no document still says the stub only reports its mode.
31. As an agent working the plan, I want the backlog row about the stub's missing A27 rules deleted in the stage that closes it, so that the roadmap stays a list of open work.
32. As a reviewer, I want the model to stay independent of `dfu::Machine`, so that the device the updater is tested against is not the updater's own idea of a device.
33. As a reviewer, I want the model to be single-threaded with no mutex, so that the stub's device thread owns it the way it owns its slots today.
34. As a reviewer, I want every rule in the model to cite its protocol-notes entry (S14, S35, S41 and so on), so that each rule is traced to a primary source (CLAUDE.md rule 5).

## Implementation Decisions

* **A new support module, the boot model**, built as its own static target
  beside `smply::minicbor`, `smply::dfu_app` and `smply::dfu_package`. It is not
  installed. It links `smply::smply` for `ConstBytes` and `ImageError`, and
  nothing private. `ServerSimulator`'s test-support target and the stub
  device's target both link it.
* **Its configuration describes the device's build, not a test script**, the
  same split `ServerConfig` makes. It holds the boot behaviour, the number of
  images, `no_downgrade`, the three A27 Kconfigs, and per image whether the
  device commits it (the apply outcome, the apply reads, the commit outcome
  and the commit reads). Making an image device-committed after construction
  stays possible, because `ServerSimulator::device_commits()` does it today.
* **The boot behaviour is an enum of the behaviours modelled**: swap with
  revert, overwrite (upgrade-only), direct-XIP, and direct-XIP with revert.
  The *reported* mode number stays with each fake, which maps it to a
  behaviour. Every swap mode (scratch, move, offset), a mode that is not
  reported, `-1` and any number the model does not cover all map to swap with
  revert. This keeps `ServerConfig::bootloader_mode`'s meaning, a reported
  number that may be absent, negative or out of range, without asking the
  model to model a mode it does not.
* **The interface is operations on a slot table**, each answering with values:
  * load a slot's content; start a direct-XIP device in a given slot;
  * the upload target for an image number, or "no slot" (`NoFreeSlot`);
  * begin an upload to a slot: erase it to a given size, and, under
    direct-XIP, clear that slot's boot state; then write bytes at an offset.
    The upload *session* (offsets, `sha`, resume, `match`) stays in the
    simulator. The model only holds flash;
  * set-state with an optional hash and the confirm flag, answering an
    `ImageError`. It finds a hash in any image's slots, a hashless confirm
    names the running image's active slot, and A27, the "test on the active
    slot" denial and the already-pending rules are applied in Zephyr's order
    (S35). Hash *encoding* checks (missing on a test, wrong length) are the
    wire's;
  * whether set-state exists in this behaviour (false only for direct-XIP
    without revert, S10);
  * erase a slot, answering `InvalidSlot`, or `NoFreeSlot` for a slot marked
    for the next boot;
  * reboot;
  * the slot listing, in two forms: a *state read*, which first advances
    device-committed applies and commits by one read, and the plain listing
    that set-state answers with;
  * inspection: a slot's content, an image's swap type, the direct-XIP
    active slot, the image count.
* **The listing is the model's own image reader's output**: magic, header
  size, image size, flags (`bootable`), the version string as
  `img_mgmt_ver_str()` formats it, and the SHA-256 hash TLV from the
  unprotected area. The simulator's reader moves into the model unchanged. The
  stub stops reading slots with smply's public parser, so both fakes read
  flash the same independent way.
* **`ServerSimulator` keeps its public interface.** `SwapType`, `ApplyOutcome`
  and `CommitOutcome` become the model's types, re-exported under their
  current names in the test namespace, so no test changes. Its private boot
  state (the image pairs, the direct-XIP state, the trial flag) and the
  methods that decide on it move into the model. It keeps the request
  decoding, the upload session, `fail_next`, `answer_offset_once`,
  `drop_next_response`, `reset_busy_once`, the v1 error translation and the
  delivery queue.
* **The stub runs the mode it reports.** Its constructor keeps its
  parameters: the running image, the optional second image, and the optional
  reported mode. The reported mode now also picks the model's behaviour,
  mapped as above. Its upload writes to the model's upload target. Its
  set-state answers come from the model, so a plain direct-XIP stub answers
  `ENOTSUP` and an image-1 client confirm is denied. Its `ApplyOutcome` becomes
  the model's, re-exported under the current name. It does not gain
  `no_downgrade`, slot info or any wire behaviour the simulator has and it
  lacks.
* **The stub's stance changes, and the documents say so.** The boot rules are
  now shared and trusted. The stub is still not a reference for the *wire*:
  the upload's offset correction, resume and `match` are the simulator's
  alone, and if the two disagree on the wire, the simulator is right. The
  stub's header, `testing.md` §2 (`ServerSimulator`) and §3 (the two example
  sections), `architecture.md`'s layout tree, the `support/` CMake comment and
  `cli_dfu`'s usage text are updated in the same change.
* **No ADR.** No ADR records the stub's stance or the simulator's internals.
  The model sits in `support/` under the rules that already govern it
  (ADR-0016: not installed). It is a test double, not the library, so the
  core's rules on platform independence and threading (ADR-0004) are not
  touched.

## Testing Decisions

* **A good test here drives the model through its interface only:** set up a
  device, apply operations, assert the slot listing, the `ImageError`, the
  swap type or the active slot. No test reads the model's private state, and
  none encodes SMP.
* **New: a unit suite for the boot model**, in the unit executable, tagged by
  rule area. At least one case per rule:
  * the flag table per swap type (S14);
  * the reboot transitions (None, Test, Perm, Revert);
  * overwrite;
  * downgrade prevention erasing an older image (S41);
  * direct-XIP preference: the newer valid slot, a tie to the lower slot,
    with revert only a marked or confirmed slot, and an unconfirmed trial
    reverted and erased;
  * direct-XIP's set-state rules;
  * the A27 denial order and each Kconfig relaxing only its rule;
  * a hash found in any image, and a hashless confirm naming image 0;
  * the upload target and `NoFreeSlot`;
  * the erase rule;
  * invalid content skipped from the listing;
  * an apply or commit advancing by state reads, image 0's confirm starting
    the device's commits, and an immediate commit when image 0 is not on
    trial (ADR-0022).
* **Prior art:** `test_upload_session.cpp` and `test_update_state_machine.cpp`
  are table-driven tests of pure decision modules (ADR-0008's split). The
  boot model's suite follows them. Its images come from
  `tests/support/image_builder.hpp`.
* **Unchanged, as the proof of the refactor:**
  * `test_simulator.cpp`'s 35 cases, driven with hand-built requests;
  * `test_round_trip.cpp`, `test_firmware_update.cpp` and `test_update_run.cpp`;
  * every `cli_dfu` and `serial_dfu` ctest, regular expressions included.

  If one of these needs editing for any reason other than the stub now
  running its mode, the extraction changed behaviour, and that is a bug.
* **The stub's new behaviour is shown by the example ctests:**
  * `cli_dfu_mode_two_builds`, `cli_dfu_xip_package` and
    `cli_dfu_xip_package_board` must still complete, now through a real
    direct-XIP device;
  * `cli_dfu_mode_allow_no_revert` must complete through an overwrite.

  Add one ctest each, matching their output (`PASS_REGULAR_EXPRESSION`):
  * a package update with `--commit 1=client`, failing with the confirmation
    denial;
  * `--stub-mode direct-xip --demo-builds`, showing how the updater handles
    a device without set-state.

  Write the expected output from what the updater does against the
  simulator in the same configuration, not from what the stub prints.
* **The finishing checklist in `handoff.md`** applies on every preset, ASan
  included. The stub's model is touched only on its device thread, so the
  sanitizer builds must report nothing new.

## Out of Scope

* **The upload session.** Offset correction, resume by `sha`, the
  already-present check and `match` stay in `ServerSimulator`. Sharing them
  with the stub would grow the stub's wire into a second reference.
* **New bootloader modes** (single slot, the firmware loader, RAM load,
  serial recovery). The model makes them cheaper to add. Each remains its own
  backlog row.
* **The direct-XIP image that is not newer** (backlog row). The model will
  show it on the stub, but refusing it is updater behaviour that ADR-0025 did
  not decide.
* **Downgrade prevention, slot info and the response delay on the stub.** It
  reports none of them today, and nothing in the examples asks.
* **`dfu::Machine`**, `FirmwareUpdater` and every other library module. No
  library code changes, so `Docs-Impact` applies only to the test and example
  documentation.
* **The remaining architecture-review candidates** (one call to read an
  image's identity, the update run owning its links, the decoders' status
  check). They are not on the roadmap.

## Further Notes

* **The name.** "Boot model" is used here for the module and its concept.
  It is defined where it is used: in `testing.md` §2, beside
  `ServerSimulator`, in the stage that adds it. No root `GLOSSARY.md` is
  created for it, because `check_docs.py` R6 does not read one yet (backlog).
* **The simulator's reader exists to be independent** of `src/image/`. Its
  file comment explains why. It moves with that comment, and the model's
  header repeats the rule.
* **The architecture review** behind this plan was published as a private
  artifact, outside the repository. This document is the record.

## Stages

Each stage leaves every check in [`handoff.md`](handoff.md) passing on every
Linux preset. Stage 3 may run beside stage 2.

### Stage 1: the boot model takes over the simulator's swap images

Blocked by: none.

The boot model exists, with its own image reader and the swap-with-revert
rules, and `ServerSimulator` decides every swap-mode answer through it.
Direct-XIP, overwrite and downgrade prevention stay in the simulator for now.

- [x] The boot model module and its target in `support/`, linking only
      smply's public headers, not installed.
- [x] Its image reader is the simulator's, moved with the comment on why it
      is independent of `src/image/`.
- [x] The model owns, for swap with revert: the flag table, the reboot
      transitions, set-state by hash and hashless (A27 and Zephyr's denial
      order included), erase, the upload target and the upload's erase and
      writes, the listing in both forms, and device-committed applies and
      commits (ADR-0022).
- [x] `ServerSimulator` uses it for all of the above. Its public interface is
      unchanged, `SwapType`, `ApplyOutcome` and `CommitOutcome` re-exported
      under their current names.
- [x] A unit suite for the model covers each of those rules, with a case per
      protocol-notes entry it cites.
- [x] `test_simulator.cpp`, the component suites and the example ctests pass
      unedited.
- [x] `testing.md` §2 defines the boot model beside `ServerSimulator`;
      `architecture.md`'s layout tree lists the new directory.

### Stage 2: the boot model takes over overwrite, downgrade prevention and direct-XIP

Blocked by: Stage 1.

The model carries all four behaviours, and the simulator holds no boot state.

- [x] The model's configuration takes the boot behaviour enum and
      `no_downgrade`. The simulator maps its reported mode number onto the
      behaviour, an unmodelled or absent mode as swap with revert.
- [x] Overwrite, downgrade prevention (S41) and direct-XIP with and without
      revert (S10, S14) are the model's, including whether set-state exists
      and starting a direct-XIP device in a given slot.
- [x] Unit cases for each: the newer valid slot, a tie to the lower slot,
      only a marked or confirmed slot with revert, an unconfirmed trial
      reverted and erased, direct-XIP's set-state rules, overwrite, and an
      older image erased under `no_downgrade`.
- [x] The simulator's own boot fields and methods are gone. Its existing
      suites and the component suites pass unedited.

### Stage 3: the stub uses the boot model for its swap images

Blocked by: Stage 1.

The stub device keeps its thread, its five commands and its upload, and
takes its slot table, flags, set-state answers and reboot from the model in
swap-with-revert mode. It refuses a client confirm of image 1, as a default
Zephyr build does.

- [ ] The stub's own swap type, flag derivation, apply and commit code are
      gone. It reads slots with the model's reader, not smply's parser.
- [ ] Its constructor is unchanged, and its `ApplyOutcome` is the model's
      under the current name.
- [ ] A new `cli_dfu` ctest: a package update with `--commit 1=client` fails
      with the confirmation denial, its expected output taken from the
      updater's behaviour against the simulator.
- [ ] Every existing `cli_dfu` and `serial_dfu` ctest passes unedited.
- [ ] The backlog row on the stub's missing A27 rules is deleted, and the
      stub's header no longer says it lacks them.

### Stage 4: the stub runs the mode it reports

Blocked by: Stage 2, Stage 3.

The mode the stub reports picks the model's behaviour, so a direct-XIP stub
runs from either slot and an upgrade-only stub overwrites.

- [ ] The stub maps its reported mode onto the model's behaviour as the
      simulator does. Its upload goes to the model's upload target, and a
      plain direct-XIP stub answers set-state `ENOTSUP`.
- [ ] `cli_dfu_mode_two_builds`, `cli_dfu_xip_package`,
      `cli_dfu_xip_package_board` and `cli_dfu_mode_allow_no_revert` pass
      unedited, now through the mode they name.
- [ ] A new `cli_dfu` ctest for `--stub-mode direct-xip --demo-builds`,
      matching what the updater does against the simulator in that mode.
- [ ] The stub's header, `testing.md` §3 (both example sections), the
      `support/` CMake comment and `cli_dfu`'s usage text say the stub runs
      its mode and is still not the wire's reference.
- [ ] The roadmap row is removed and this file is deleted.
