# Quality review and cleanup plan

Written 2026-09-24. The roadmap's "In progress" section points here. Work the stages in order, and record each one under Progress as it lands. This file is deleted in Stage 6.

## Progress

- **Stage 0: done** (2026-09-24). See "Stage 0 results" below.
- **Stage 1: done** (2026-09-24). R6 now covers source, build, CI and tool files as well as documents, so the history cannot creep back.
- **Stage 2: done** (2026-09-24): 2a the header cycle, 2b namespaces, 2c the layering gate, 2d `architecture.md` accuracy.
- **Stage 3: in progress.** 3a (one image number) and 3b (the upload header, resume) are done.
- Stages 4–6: not started.

### Stage 0 results

All ten Linux presets build. Each runs 707 tests, apart from `linux-clang-fuzz`, which builds the fuzz targets and has no ctest suite.

**One pre-existing defect:** every `cli_dfu` ctest writes the same temporary file (`$TMPDIR/cli_dfu_demo_image.bin`, truncated on open). Under `ctest -j` the runs race. `cli_dfu_demo` or `cli_dfu_flaky_reconnect` then fails with "short read from the image file" in 6 of 10 preset runs.
- Worse, `cli_dfu_reconnect_gives_up` is a `WILL_FAIL` test, so a short read makes it pass for the wrong reason.
- It is fixed in its own commit, ahead of Stage 5, because it made every later verification step unreliable.

**Found while doing 1d:** the documentation cases in `verify_gates.sh` had been passing without testing anything. The scratch copy had no git index, and R5 reads `git ls-files`, so `check_docs.py` failed on the unmodified tree. Fixed in 1d.

## Context

smply is a C++20 MCUmgr/SMP client library, version 0.1.0. Every development phase is `Complete`. The code is small (about 7k lines under `include/`, `src/`, `transports/`, `support/` and `examples/`), but the documentation has grown to about 13k lines. Most of that is history nobody needs any more:
- `docs/roadmap.md` is 3,298 lines. Nearly all of it is the outcome write-ups of each phase, plus many struck-out follow-up rows.
- `docs/handoff.md` is 2,844 lines. About 82% of it is the session log.
- The process these files serve (a phase roadmap whose Status lines `check_docs.py` R2/R6 enforce, a required session-log entry for every session) was built for development phase by phase. That is over.

The review goes from the top down: first the process and docs, then architecture, then design and the API, then the implementation, then tests, tooling and CI. Each stage fixes what it finds.

Decisions the user made:
- **Delete the history.** Git keeps it. The docs point to the last commit that still has it, `97f1647`.
- **Simplify the process**, with a new ADR that supersedes the process part of ADR-0013.
- **Breaking API changes are allowed.** Pre-1.0, per ADR-0016: bump to 0.2.0 and record the changes in CHANGELOG.

Constraints from the repo: stay under about 1,000 lines of diff per commit. Architecture, design and API docs change in the same commit as the code they describe. An ADR decision is never edited in place; it is superseded. Before a stage is done, every gate must pass on every Linux preset.

---

## Stage 0: baseline, no commits

- Run `apt-get install -y cppcheck libclang-rt-18-dev && pip install gcovr`.
- Configure, build and run `ctest` on every Linux preset. Check each build's exit status separately.
- Run `tools/format.sh --check`, `tools/lint.sh`, the three `check_*.py` gates, `coverage.sh --enforce` and `verify_gates.sh`.
- Record the numbers (test count, coverage) to compare against after each stage.

## Stage 1: process and docs cleanup

**1a. The new process decision.** Add a new `docs/decisions/ADR-NNNN-maintenance-process.md`, which supersedes the process part of ADR-0013 (roadmap as execution state, session log). What stays: docs are part of the product (R1, R3, R4, R5), and ADRs are never edited in place. Also:
- Update ADR-0013's Status line to "partly superseded by the new ADR".
- Update `docs/decisions/README.md`. It is also missing ADR-0017.

**1b. `docs/roadmap.md` becomes a short backlog** of under about 150 lines, with these sections:
- Status: one paragraph. It says the per-phase history is in git at `97f1647`.
- Open questions: O3, O5, O6, O7, keeping their IDs, because code and docs cite them. Resolved questions are dropped.
- Backlog: the follow-up rows that are still open, grouped by area rather than by the phase that found them, each with a short "why". Struck rows are dropped.
- Known acceptance gaps: the fresh-clone device update and commissioning the bench runner.

**1c. `docs/handoff.md` becomes a short contributor guide** of about 300 lines:
- A short session protocol: read the docs, work an item from the backlog, run the checklist.
- § Standing caveats, keeping only the rules themselves:
  - Remove the duplicate A16 bullet (`:212` repeats `:162`).
  - Change "two directories" to three.
  - Move the misplaced compiler paragraphs (`:323-351`) under "Build every preset".
  - Strip the "until Pn…" narrative.
- The session log and its template are deleted.
- Update `CLAUDE.md` to match: rules 2 and 8, and the "session-log entry" wording. Also fix CLAUDE.md's "cppcheck skipped silently" wording.

**1d. Gates follow the new process** (`tools/check_docs.py`, `tools/verify_gates.sh`, `docs/quality-gates.md` §11 and §12):
- R2: replace the phase Status/Remaining rule with a structural check of the backlog. At minimum, it checks that the open-question IDs cited anywhere exist in the roadmap.
- R6: replace the "(planned, phase)" rule, which cannot work without phases, with a ban on development-phase IDs in living documents and, from 1f, in source comments. ADR bodies are exempt. It lands with 1e, once the documents are clean.
- R3: add a check that every `ADR-*.md` file is listed in `decisions/README.md`. That is how the missing ADR-0017 entry went unnoticed.
- Update the self-check cases that relied on old roadmap content: case 8 and case 11b, which relied on a phase's Status line.
- Keep the exact tree line of case 11a in `architecture.md` untouched.

**1e. Remove history from the living docs.** Keep each document's current-state reference; cut the phase stories.
- Main targets:
  - `quality-gates.md`, about 25–30% history.
  - `security.md` "What the audit found": turn it into current state.
  - The api.md intro and its per-phase "Shipped" table.
  - The WinRT stories in `design.md` §10.
  - `testing.md:561`, which is stale because `send_counters()` was kept.
  - `dependencies.md` stories.
  - The `README.md` "Status" section: replace it with a short, number-free summary.
  - The `CHANGELOG.md` sentence at `:16-18`.
- `protocol-notes.md` keeps its dated sources, which are there on purpose. Only its references to the roadmap and handoff get fixed.

**1f. Code comments.** About 150 `Pn` mentions across `src/`, `include/`, `tests/`, `transports/`, `examples/`, `tools/`, `.github/` and `cmake/`, for example `include/smply/limits.hpp:39` and `src/dfu/update_state_machine.cpp:209,254`. Rewrite each to state the current invariant and its reason.
- Keep `A`, `S` and `T` IDs, because they are stable anchors into protocol-notes and security.
- Fix the three references to "handoff.md 'Lifetime'" and the roadmap phase references in `tests/hil/`, `hil.yml` and `nightly-fuzz.yml`.
- Split into 1f-i (`src/`, `include/`, `transports/`, `support/`, `examples/`) and 1f-ii (`tests/`, `tools/`, CI) to keep the diffs small.

## Stage 2: architecture review

`architecture.md` §3–§9 was read against the code.

**Checked and accurate:**
- the §9 limits (all 23 match `limits.hpp`);
- the §5 threading rules;
- the §6 ownership table;
- the §7 `ErrorCode` list.

No ADR constrains anything below. What it found, and the fix for each:

- **2a. A header cycle.**
  - `mcuboot_image.hpp` included all of `groups/image.hpp` (and so `smp_client.hpp` and `transport.hpp`) for `ImageHash` and `ImageVersion`.
  - Meanwhile `ImageManagement::upload()` calls `sha256()` from `mcuboot_image.hpp`.
  - The §3 diagram showed the image-file box as "no deps", with no edge from the image group.
  - Fix: both types are MCUboot's, so they move down into `mcuboot_image.hpp`, and their code into `src/image/image_values.cpp`. `groups/image.hpp` includes `mcuboot_image.hpp`, and the diagram gains the edge.
- **2b. Namespaces.**
  - `smply::image` names two components: `src/image/` and the upload internals in `src/groups/image/`.
  - `src/smp/assembler.hpp` puts internal types in the public `smply` namespace.
  - Rule: the public API lives in `smply`, and an internal type in `smply::<component>`. The upload internals become `smply::upload`, and the assembler `smply::smp`.
- **2c. The layering becomes a gate.** `check_public_headers.py` gets a layer table for public headers and an allowed-dependency table for `src/` directories, with `verify_gates.sh` cases for both.
- **2d. `architecture.md` accuracy.**
  - §4 promises a `smply::asyncutil` target that does not exist.
  - §4 says every operation returns a `RequestHandle`.
  - §7 misdescribes `UpdateReport::cause`.
  - The §3 diagram names a `PendingRequestTable` class that does not exist.

## Stage 3: design and public API review

Breaking changes are allowed. Each one gets a CHANGELOG entry under 0.2.0 and updates to `api.md` and `design.md`. Candidates, in order of value:
1. **`UpdatePlan::image` vs `UpdatePlan::upload.image`.** `firmware_updater.cpp:403` silently overwrites one with the other. Keep one source of truth.
2. **Split `groups/image.hpp` (603 lines)** into value types (already moved in Stage 2), `ImageError`, and the upload API plus `ImageManagement`. Keep `groups/image.hpp` as the umbrella header.
3. **`ImageSlot::version` stays a string.** This corrects the original plan: a device reports `"<???>"` when it cannot format a version, so parsing must stay a separate, fallible step (`groups/image.hpp`, `ImageVersion::parse`). At most, add `ImageSlot::parsed_version()` as a convenience.
4. **Callback styles.**
   - Upload progress becomes a named `ProgressCallback` alias next to `Callback<T>`.
   - `UpdateEvent` with a `Kind` enum, optional fields and `const Result<UpdateReport>*` gets either a `std::variant` or separate typed callbacks. Choose after reading `examples/*/main.cpp` usage. If this changes ADR-0003, write a new ADR.
5. **`ImageManagement::resume()`** returns `void` and has a callback that "fires again", which is a different contract from `upload()`. Align them.
6. **Timeouts.** Unify the per-operation timeout fields: `ResetOptions`, `EraseOptions`, the three in `UploadOptions`, `RequestSpec`.
7. **`UpdateReport` and the state machine's `Context`** hold nearly the same fields (`firmware_updater.cpp:438-467`). Make one contain the other.

Deliberately out of scope, stays as recorded in the roadmap: `std::error_code` interop (O6), pipelining (O3), new groups.

## Stage 4: implementation review

- **Duplicated group helpers.** `reject`/`defer` exists three times (`image_management.cpp:69`, `os_management.cpp:58`, `client.cpp:396`), and `encode_empty`, `command_id` and `kRequestBufferSize` are duplicated too. Move them to `src/groups/group_common.hpp`.
- **Two helpers named `narrow` with different meanings.** `src/image/source_reader.hpp:32` is an unchecked cast; `image_management.cpp:83` is checked. Move both to `src/detail/narrow.hpp` as `narrow_cast` and `checked_narrow`, and replace the hand-written range checks at `image_management.cpp:203,252` and `os_management.cpp:182-189`.
- **`image_management.cpp` (784 lines).** Move the response decoders (`:24-343`) into `src/groups/image/decode.cpp`, and flatten `decode_slot_info`'s nested lambdas.
- **`firmware_updater.cpp` `apply()`** (117-line switch). Factor out the near-identical `MarkForTest` and `Confirm` blocks.
- **`update_state_machine.cpp` `advance()`** (about 230 lines). Split it into one function per state.
- **Error construction.** `fail(ErrorCode,…)`, `fail(Error{…})` and raw `unexpected<Error>{…}` are all used. Pick one style, as `result.hpp:74` already asks.
- **Little-endian loads** (`source_reader.hpp:75`, `smp_ble_uuid.hpp:81`): share one helper where the layering allows.
- **Unreachable guards.** Review the 11 `LCOV_EXCL` blocks. Keep them only when they guard untrusted input. Remove the ones that are provably unreachable.
- **Review against the rules.** Each changed file is checked against `design.md` §11 (robustness rules) and CLAUDE.md rule 6 (bound every device-supplied length).

## Stage 5: tests, tooling and CI

Bugs to fix first:
- `nightly-fuzz.yml:120-127` leaves `fuzz_serial_deframe` out of its matrix.
- The coverage artifact path in `ci.yml:214` never matches the gcovr output.
- The `gate-self-check` job never installs cppcheck or gcovr, so those self-check cases always print SKIP. Install both, and make `verify_gates.sh` fail on a SKIP in CI (`CI=true`).
- `lint.sh` passes when cppcheck is missing. Make that an error in CI.

Simplification:
- Remove the `core-without-winrt` preset and CI row; it is identical to `windows-msvc`.
- Add hidden base presets to `CMakePresets.json` to remove about 160 lines of copied test-preset entries.
- Stop running CI twice on PR branches: limit `push` to `main`.
- Add `actions/cache` for `_deps`.
- `check_deps.py` and `sbom.py` both parse `dependencies.cmake`. Share the parser.
- Rename `tests/consumer/` to `tests/interface_flags/` so it is not confused with `tests/consumption/`.
- Remove the stale TSan comment in `cmake/sanitizers.cmake`.

Gaps to fill:
- Unit tests for `support/dfu_app/file_image_source.cpp`.
- A direct test for `src/image/source_reader.hpp`.
- Prune the fuzz corpora (`-merge=1`).

Repository metadata:
- Add `.github/pull_request_template.md` explaining the `Docs-Impact: none` waiver.
- Add `.github/dependabot.yml` for GitHub Actions.
- Add a short `CONTRIBUTING.md` that points to the contributor guide from 1c, `quality-gates.md` and the build presets.

## Stage 6: final pass and release

- Run a full `check_docs.py`. Read `architecture.md`, `design.md` and `api.md` once more against the final code.
- Bump the version to 0.2.0 in `CMakeLists.txt`. Close the CHANGELOG section.
- Rewrite the roadmap backlog to reflect what the review closed.
- `README.md`: check the build commands and the docs index.

---

## Order and commits

Each stage, and each numbered sub-step of Stage 1, is one commit (or a few), each under about 1,000 lines of diff, all on `claude/project-quality-review-l7odk7`. Stage 1 comes first, so the later stages work against a lean doc set and the new process. The order of Stage 2 before Stage 3 matters, because the value-type move is what the header split builds on.

## Verification, for every commit

1. Build each Linux preset and check the exit status of each build separately:
   - `linux-gcc`, `linux-clang`, `linux-gcc-release`
   - `linux-clang-asan-ubsan`, `linux-gcc-asan-ubsan`, `linux-clang-tsan`
   - `linux-gcc-fallback-expected`, `linux-gcc-cxx23-std-expected`
   - `linux-clang-fuzz`
2. Then run `ctest --preset <p>`.
3. Run the gates: `tools/format.sh --check`, `tools/lint.sh` (with cppcheck installed), `tools/check_public_headers.py`, `tools/check_deps.py`, `tools/check_docs.py --verbose`, and `tools/coverage.sh --enforce` on `linux-gcc-coverage`. Coverage must not fall below the Stage 0 baseline.
4. `tools/verify_gates.sh` must pass with no SKIP after any change under `tools/` or `cmake/`, and after Stage 1d.
5. After Stage 3 and Stage 6, run `tools/check_install.sh`, which covers the three out-of-tree consumption modes, and `examples/cli_dfu`'s ctests, which run a full update against the stub device.
6. Fuzz smoke: run each target for 20k runs after any change under `src/` (Stages 2–4).
7. The Windows presets and the hardware tests cannot run in this container. After pushing, CI shows the Windows result, and the push says the hardware tests were not run.
