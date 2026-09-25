<!-- SPDX-License-Identifier: Apache-2.0 -->

# Contributing to smply

Thank you for looking. This file is the front door; the working guide is
[`docs/handoff.md`](docs/handoff.md), and it is short enough to read first.

## How work is organised

[`docs/roadmap.md`](docs/roadmap.md) is the backlog: what is in progress, what
is open, and the open questions. A change works one item from it and stays
inside that item. Anything else it finds goes into the backlog. Finished items
are deleted from the roadmap rather than struck through, because the history
is in git, and the reasoning behind a change belongs in its commit message
([ADR-0018](docs/decisions/ADR-0018-maintenance-process.md)).

Documentation is part of the product. A change that makes
[`architecture.md`](docs/architecture.md), [`design.md`](docs/design.md) or
[`api.md`](docs/api.md) wrong updates them in the same commit. An architectural
decision is changed by a new ADR that supersedes the old one, never by editing
it ([`docs/decisions/`](docs/decisions/)).

## Building and checking

Every CI configuration has a CMake preset; `cmake --list-presets` lists them.
Before sending a change, the checks in the README's
[Checks](README.md#checks) section must pass, on every Linux preset rather
than one: GCC and Clang reject different things. Some checks need tools a
fresh machine lacks:

```sh
apt-get update && apt-get install -y cppcheck libclang-rt-18-dev && pip install gcovr
```

Without them the scripts skip what they cannot run, locally. CI runs with
`CI=true`, which turns every such skip into a failure; set it yourself to see
what CI will. What each gate enforces, and why, is in
[`docs/quality-gates.md`](docs/quality-gates.md).

## Pull requests

The template asks what changed and why, which docs changed, and which checks
you ran. If a change to code under `include/smply/`, `src/smp/`, `src/dfu/` or
`src/groups/` really needs no documentation change, the documentation gate
accepts a line in the PR body starting with `Docs-Impact: none` followed by the
reason on the same line.

Protocol behaviour is traced to Zephyr and MCUboot sources and documentation
only ([`docs/protocol-notes.md`](docs/protocol-notes.md)). Security issues go
through [`SECURITY.md`](SECURITY.md), not a public issue.
