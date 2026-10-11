# Plan: one owner for an update's inputs, and a suspended upload session

Status: ready for implementation

Two deepening refactors found by an architecture review. They touch disjoint
code and each stands alone; the second lands first because it is the smaller:

1. A link drop suspends the upload session inside the session, and a resume is
   a decision of the session.
2. An **update inputs** module in `smply::dfu_app` owns what an update sends --
   a file, a direct-XIP build pair or a package -- and always hands the updater
   an image list.

Neither changes behaviour, the library's public interface or an ADR.

## Problem Statement

**For a maintainer of the upload decisions**, the rule that a dropped link
keeps the session, and what a resume resets, live in `UploadDriver`, not in the
pure session ADR-0008 says owns the decisions. The session answers
`Disconnected` with `Phase::Failed`; the driver overrules it with a private
`resumable_` flag, and its `restart()` writes four of the session's fields
directly. The session's table tests cannot reach either rule; only the driver
tests, through a fake client, can.

**For an application author**, choosing what to send is rebuilt by hand in
each of the three examples: a `FileImageSource` that is faked to fail in package
mode, an optional second build, a `PackageUpdate`, a one-element target array
for the build pair, and a lambda that picks one of the two `start()` overloads
-- which read `plan.upload.image` differently. The `--commit` loop, the
"secondary needs a primary" check and `parse_mode` are copied too. The choice
has no unit test; only the examples' ctests run it.

## Solution

**The upload session.** `upload::on_response` answers `Disconnected` with
`Phase::Suspended` and a new `Action::Suspend` carrying the error. A pure
`upload::resume()` takes a suspended session back to a first packet. The driver
reports `Suspend` to its callback as it reports `Fail`, asks the session's
phase whether it is resumable, and writes none of the session's fields.

**The update inputs.** `smply::dfu_app::UpdateInputs` replaces
`PackageUpdate`. It is built from files (`from_files(primary, secondary)`,
image 0, a secondary making a direct-XIP build pair) or from a package
(`from_package(path)`, `from_package_bytes(bytes)`), takes `set_commit()`, and
always yields `targets()` for the image-list `start()`. `parse_mode` moves next
to `parse_commit`. The examples call only the image-list `start()`.

## Decisions

* **Only `Disconnected` suspends.** A `Timeout` that exhausts its retries stays
  terminal, as today.
* **`resume()` resets exactly what `restart()` reset**: the phase, the first
  packet, `retries` and `consecutive_no_progress`. It keeps `restarts` (one
  budget for the whole upload) and `progressed` (the A19 already-present
  guard). It refuses a session that is not suspended with `InvalidState`. The
  session does not bound resumes; the updater's reconnect policy does.
* **`Action::Suspend` is its own action**, not `Fail` with a phase, so the
  driver's exhaustive switch names it.
* **`UpdateInputs` is support code** (ADR-0021 §5), non-movable and handed out
  on the heap like `PackageUpdate`. Both `start()` overloads stay public; the
  single-image one is a convenience around eighty tests and the hardware rig
  use, and its doc says plainly that the image-list `start()` replaces
  `plan.upload.image`.
* **`from_files` is image 0 only.** No caller names another image from a file.
  `set_commit` refuses an image the inputs do not hold.
* **It takes parsed values, never argv.** Each example keeps its own option
  parser; "a secondary needs a primary" is `from_files`' refusal.
* **`cli_dfu` keeps writing its demo images to files**, so the demo still runs
  `FileImageSource` through a real update. No in-memory single-image factory.
* **The examples' event printers stay** in each example (a backlog row).

## Stages

1. **Done: the upload session suspends.** `upload_session.{hpp,cpp}`,
   `upload_driver.{hpp,cpp}`; table tests in `test_upload_session.cpp` for the
   suspend, what `resume()` resets and keeps, and its refusal;
   `test_upload_driver.cpp` passes unedited. `design.md`'s upload section.
2. **Update inputs.** `support/dfu_app/update_inputs.{hpp,cpp}` and
   `tests/unit/test_update_inputs.cpp` (the package cases of
   `test_package_update.cpp` move there); the three examples migrate;
   `PackageUpdate` is deleted. `architecture.md` §10, `api.md`, `testing.md`,
   `CLAUDE.md`, the roadmap. The examples' ctests pass unchanged.

## Out of scope

The examples' console printers; retiring the single-image `start()`; an image
number for file inputs; a TLV walker shared by the core and the package reader.
