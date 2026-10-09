# smply — instructions for coding agents

**Before doing anything, read [`docs/handoff.md`](docs/handoff.md)**: how to
start and finish a piece of work, and its **§ Standing caveats**, the rules
earlier work learned the hard way. This file is only the short version.

## The rules that matter

1. **The repository is the authoritative project state.** Never rely on
   conversation history. If something is worth knowing next session, write it
   down.
2. **Work what [`docs/roadmap.md`](docs/roadmap.md) says is in progress**, or a
   backlog item whose time has come, and stay inside its scope. Work you find
   outside it goes into the roadmap's backlog. Finished items are deleted, not
   struck through. History lives in git, not in the docs
   ([ADR-0018](docs/decisions/ADR-0018-maintenance-process.md)).
3. **Documentation is part of the product.** A change that makes
   `architecture.md`, `design.md` or `api.md` inaccurate is not complete until
   they are updated *in the same change*
   ([ADR-0013](docs/decisions/ADR-0013-living-documentation.md)).
4. **Never silently deviate from an ADR.** To change a decision: write a new ADR
   that supersedes the old one, update the docs and the roadmap, *then*
   implement. See [`docs/handoff.md`](docs/handoff.md).
5. **Protocol behaviour is traced to primary sources.** Zephyr and MCUboot
   documentation and source only — third-party clients are for behavioural
   comparison, never for copying code or inferring the protocol. New findings go
   in [`docs/protocol-notes.md`](docs/protocol-notes.md).
6. **Treat everything from the device as untrusted.** Bound every length before
   using it; never allocate on a device-supplied size.
7. **Keep the core platform-independent.** No WinRT, no Windows, no BLE and no
   clock in `include/smply/` or `src/`. The core also **starts no threads and
   contains no mutex, atomic or condition variable** — but that is a rule about
   the *core*, not about the directories: `smply::Dispatcher`
   (`include/smply/util/`, `src/util/`) is a mutex and a queue, shipped as a
   separate target that adapters opt into and `libsmply` never links
   ([ADR-0004](docs/decisions/ADR-0004-threading-model.md),
   [`architecture.md`](docs/architecture.md) §5). The only other mention of a
   thread under `src/` is the debug-only client-context assertion, compiled out
   in release.
8. **Finish with the checklist** in [`docs/handoff.md`](docs/handoff.md). Put
   the reasoning in the commit message. There is no session log.

## Layout

`include/smply/` public headers · `src/` implementation · `transports/` the
portable BLE helpers in `common/` and the platform adapters beside them ·
`support/` code shared by the tests and the examples and part of neither
(`smply::minicbor`, `smply::dfu_app` for `FileImageSource`,
`ReconnectPolicy` and `PackageUpdate`, and `smply::dfu_package` for the multi-image package reader) · `examples/` `cli_dfu` (portable, runs in CI) and
`winrt_ble_dfu` (Windows) · `tests/` unit, component, fuzz, HIL · `docs/` living
documentation and ADRs · `.claude/` the shared Claude Code skills, rules and
hook. Full description:
[`docs/architecture.md`](docs/architecture.md) §10.

## Conventions

C++20, no compiler extensions. Every file starts with
`// SPDX-License-Identifier: Apache-2.0` (`#` for CMake, Python and shell).
Warnings are errors for smply's own targets.
No owning raw pointers, no C-style casts, no `reinterpret_cast` over device
data. Exhaustive `switch` over internal enums with no `default`. Public entry
points validate arguments and return `InvalidArgument` rather than asserting.
See [`docs/design.md`](docs/design.md) §11.

## Before you finish

`tools/format.sh --check`, `tools/lint.sh`, the three `tools/check_*.py` gates
and `ctest` must all pass. `tools/verify_gates.sh` proves the gates themselves
still work — run it if you touch anything under `tools/` or `cmake/`.

Three ways this has gone wrong before, all cheap to avoid:

* **Neither `cppcheck` nor `gcovr` is installed here, and both fail soft
  locally.** `lint.sh` skips cppcheck with only a note, `coverage.sh` falls
  back to plain `gcov` and reports a number that is not comparable, and
  `verify_gates.sh` prints SKIP. CI (`CI=true`) turns each of those into a
  failure, so a clean local run can still fail CI. Run
  `apt-get update && apt-get install -y cppcheck libclang-rt-18-dev && pip install gcovr`
  first.
* **A failed build leaves the old test binary in place**, so `ctest` then
  reports the *previous* suite passing. Check the build's exit status
  separately — never read "N tests passed" as proof anything was rebuilt.
* **Build every preset, not just one.** GCC and Clang reject different things,
  in both directions, and Clang's ASan finds dangling callback captures that
  GCC's does not report at all.

## Agent skills

Shared skills from mattpocock/skills are vendored in `.claude/skills/`;
[`docs/agents/skills.md`](docs/agents/skills.md) lists them, what each is for,
and how they are updated. They were written for GitHub Issues, `GLOSSARY.md`
and `docs/adr/`. **Where a skill's text and these files disagree, these
files win.**

### Issue tracker

Work is tracked in the repository: `docs/roadmap.md`, plus a `docs/<slug>-plan.md`
for work too big for one row. Never `gh issue`, `glab` or `.scratch/`. See
[`docs/agents/issue-tracker.md`](docs/agents/issue-tracker.md).

### Triage labels

None: triage roles are recorded on roadmap rows, and rejected requests become
resolved open questions. See [`docs/agents/triage-labels.md`](docs/agents/triage-labels.md).

### Domain docs

Single-context. Terms are defined where they are used (`protocol-notes.md`,
`api.md`, `security.md`); a root `GLOSSARY.md` is created lazily and only
points at them. ADRs are `docs/decisions/ADR-NNNN-*.md` in the full format,
superseded and never edited. See [`docs/agents/domain.md`](docs/agents/domain.md).

### Standards for review

`code-review`'s standards are this file's Conventions, `docs/design.md` §11,
the standing caveats in `docs/handoff.md`, `docs/quality-gates.md` and
`CONTRIBUTING.md`. The project's `code-review` and `prototype` skills replace
the built-ins of the same name; the built-in reviewer is still `/review`. In `/implement`,
"typechecking" means building a preset, and "the full test suite" means the
checklist in `docs/handoff.md`.

A PR body keeps the headings of `.github/pull_request_template.md`. The `pr`
skill's Summary and Merge Danger go under "What changed, and why", and its
Evidence under "Checks run locally".

Personal agent output goes in the git-ignored `.local/`, never the repository
root: `/teach` in `.local/teach/<topic>/`, `/to-questionnaire` in
`.local/questionnaires/`, one-off wizards in `.local/wizards/`.

A hook reports clang-format differences in any C++ source Claude edits. Fix
them in that file only.
