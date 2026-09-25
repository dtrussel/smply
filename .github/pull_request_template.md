<!-- SPDX-License-Identifier: Apache-2.0 -->

## What changed, and why

<!-- The roadmap item this works, and the reasoning a reviewer needs. -->

## Documentation

<!--
Which of architecture.md, design.md, api.md, protocol-notes.md or the roadmap
changed. If code under include/smply/, src/smp/, src/dfu/ or src/groups/
changed and no documentation needed to, replace this comment with a line that
starts "Docs-Impact: none" and gives the reason on the same line
(docs/quality-gates.md section 11).
-->

## Checks run locally

- [ ] Every Linux preset builds and `ctest` passes on each
- [ ] `tools/format.sh --check` and `tools/lint.sh`
- [ ] `tools/check_public_headers.py`, `tools/check_deps.py`, `tools/check_docs.py`
- [ ] `tools/coverage.sh --enforce` (for a change under `src/`)
- [ ] `tools/verify_gates.sh` (for a change under `tools/` or `cmake/`)
