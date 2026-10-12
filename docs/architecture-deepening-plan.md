# Architecture deepening plan

Status: in progress. Written 2026-10-12. The roadmap's "In progress" section
points here. Work the stages in order, and check each one off in the commit
that lands it. This file is deleted with the last stage
([ADR-0018](decisions/ADR-0018-maintenance-process.md)).

## Context

An architecture review looked for shallow modules: places where the
interface is nearly as complicated as the implementation, or where one
decision is spread over several modules. Four candidates were chosen:

1. **`dfu::Machine` decides what to do, but `FirmwareUpdater::Impl` decides
   when.** The machine is pure and has thorough unit tests. The timing rules
   (the reconnect grace, the apply poll, the apply timeout and ADR-0022's
   "the apply timeout is not restarted by a reconnect"), and the reading of
   a bootloader-info refusal, live in `Impl` instead, where only component
   tests reach them.
2. **The three example tools repeat the same update command.** The shared
   flags, their validation, the event printer and the report are copied
   across `cli_dfu`, `serial_dfu` and `winrt_ble_dfu`, and the copies have
   drifted: only `cli_dfu` prints the per-image lines and the revert warning
   when an update fails.
3. **Whether a failed upload can be resumed is decided twice.**
   `ImageManagement` keeps a session only when the link dropped, and the
   machine guesses the same from `ErrorCode::Disconnected`. The two disagree
   when an `ImageSource` read fails with `Disconnected`: the machine asks for
   a reconnect, and the resume is then refused with `InvalidState`.
4. **`UpdateRun` leaves its own wiring to the caller.** Every caller must
   pass `run.event_handler()` to `updater.start()` by hand, and the run
   ignores the result of `confirm()` and `resume_after_reconnect()`.

Decisions taken with the user:

* The public interface of `FirmwareUpdater` does not change. No new ADR is
  needed: time passed in as a value is what ADR-0003 and ADR-0004 allow, and
  nothing here changes a decision.
* Time moves into the machine as a value: each event carries `now`, the
  machine keeps every deadline and says which comes next, and `poll(now)`
  becomes a tick. `Impl` keeps only carrying out effects, `validate()` and
  `read_target()`.
* Suspension becomes `ImageManagement`'s answer, asked through a new public
  `resumable(handle)` query. An image source failing with `Disconnected`
  then fails the update. That is a behaviour change, recorded in the
  changelog.
* `UpdateRun::run()` takes the image list and the plan, and starts the
  update itself. If `confirm()` or `resume_after_reconnect()` is refused,
  the run cancels the update and reports `ErrorCode::Internal`.
* The shared update command lives in `smply::dfu_app`. Its validation is the
  strictest of the three tools' (`serial_dfu`'s). Each tool states which
  flag groups it offers; `winrt_ble_dfu` offers no package flags.
* Each stage is pushed to the working branch once every check in
  [`handoff.md`](handoff.md) passes on every Linux preset. User-visible
  changes go under `[Unreleased]` in `CHANGELOG.md`.

## Stages

### Stage 0: the missing failure report

Blocked by: none.

`serial_dfu` and `winrt_ble_dfu` print what `cli_dfu` prints when an update
fails: the mode, each image's line, and the warning that a swap is scheduled
but unconfirmed.

- [x] Both tools print the per-image lines and the revert warning on failure.
- [x] A `serial_dfu` ctest checks the failure report.

### Stage 1: the machine owns time

Blocked by: Stage 0.

- [x] A component test pins ADR-0022's rule: with an apply timeout set, a
      link that drops while the device applies and then reconnects still
      times out on the original deadline. If it fails today, the fix lands
      first, in its own commit.
- [x] Every `dfu::Event` carries `now`. The machine keeps the reconnect
      grace, the apply poll and the apply timeout, exposes
      `next_deadline()`, and takes a `Tick` event. `Impl::poll(now)` sends
      a tick, and `Impl` has no timer or phase state left.
- [x] The machine reads the bootloader-info refusal: `Impl` passes the
      error, and the machine decides that `ProtocolError` and `CborDecode`
      mean "not available".
- [x] Unit tests pin each timing rule, ADR-0022's included.
- [x] `design.md` §8 and `architecture.md` describe the new split.

### Stage 2: `ImageManagement` says whether an upload can resume

Blocked by: Stage 1.

- [x] `ImageManagement::resumable(const UploadHandle&)`.
- [x] `Impl` asks it when an upload fails, and sends the machine
      `UploadSuspended` or `Failed`. The machine reads no error codes for
      this.
- [x] A test shows an image source failing with `Disconnected` fails the
      update.
- [x] `api.md` and `CHANGELOG.md` updated.

### Stage 3: `UpdateRun` starts the update

Blocked by: Stage 2.

- [x] `UpdateRun::run(ImageSource&, const UpdatePlan&)` and
      `run(std::span<const ImageTarget>, const UpdatePlan&)` start the
      update with the run's own handler. A refused start is the outcome.
      `event_handler()` is gone.
- [x] A refused `confirm()` or `resume_after_reconnect()` cancels the
      update and ends the run with `ErrorCode::Internal`, naming the call.
- [x] The examples, the component tests and `test_update_run.cpp` use it.

### Stage 4: one update command for the example tools

Blocked by: Stage 3.

- [ ] A `smply::dfu_app` update-command module: the shared flags, their
      validation, the conversion to `UpdateInputs` and `UpdatePlan`, one
      event printer and one report, the failure path included. Every
      `Error` it prints goes through `printable()`.
- [ ] Each tool parses its own flags first and states the flag groups it
      offers. `winrt_ble_dfu` offers no package, board or commit flags.
- [ ] `DemoImageFile` moves to `examples/stub_device/`.
- [ ] New ctests cover the rejected flag combinations.
- [ ] The two backlog rows on the tools' printers and unescaped errors are
      deleted.
