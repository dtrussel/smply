# Claude Code setup: shared skills, rules and hooks

Everything here is committed, so a fresh clone opened in Claude Code has the
same skills, instructions and checks as everyone else. Nothing is installed per
developer, and nothing updates itself.

| Path | What it is |
| ---- | ---------- |
| `CLAUDE.md` | Always-loaded project instructions; its "Agent skills" section maps the skills onto this project |
| `.claude/skills/` | Vendored third-party skills (below), discovered by Claude Code as project skills |
| `.claude/rules/` | Path-scoped pointers into the docs, loaded when Claude reads or edits a matching file |
| `.claude/settings.json` | The shared hook; personal settings go in the git-ignored `.claude/settings.local.json` |
| `.claude/hooks/check_format.py` | PostToolUse hook: clang-format check of the one C++ file just edited |
| `.claude/validate.py` | Validates all of the above; run it after any change here |
| `skills-lock.json` | The `skills` CLI's record of what was installed, from which commit, with a content hash |
| `docs/agents/issue-tracker.md`, `domain.md` | Where the skills read and write project state |

## The skills

Upstream: [mattpocock/skills](https://github.com/mattpocock/skills), MIT, at
commit `b0618bc436ad893b3c5e84e55fba86586d34a404` (2026-10-08). The licence and
provenance are in `.claude/skills/THIRD-PARTY-NOTICES.md`. The copies are
byte-identical to that commit: the project's adaptations are in
`docs/agents/` and `CLAUDE.md`, never in the skill files, so an update is a
clean diff.

User-invoked (type the name; the model cannot start them):

| Skill | Use it to |
| ----- | --------- |
| `/grill-with-docs` | Interview you about a plan until it is settled, recording terms and ADR candidates as they resolve |
| `/to-spec` | Turn the conversation into a plan document, published per `issue-tracker.md` |
| `/to-tickets` | Break a plan into stages with blocking edges, in that plan document |
| `/implement` | Work a plan stage test-first, then review it, then commit |
| `/retro` | Suggest improvements to this setup after a session |
| `/setup-matt-pocock-skills` | Re-run the tracker and domain setup. Already done; edit `docs/agents/` instead |

Model-invoked (Claude may also reach for these itself):

| Skill | Use it for |
| ----- | ---------- |
| `code-review` | Two-axis review of a diff: the project's standards, and the plan it implements. **Replaces the built-in `/code-review`**; the built-in reviewer is still `/review` |
| `tdd` | Red-green loop at agreed seams, with Catch2 and the doubles in `docs/testing.md` |
| `diagnosing-bugs` | Build a tight failing loop first, then hypothesise and fix |
| `codebase-design` | Deep-module vocabulary for interface and seam decisions |
| `domain-modeling` | Sharpen terms and decisions, writing to `GLOSSARY.md` and `docs/decisions/` |
| `grilling` | The interview behind `/grill-with-docs` |
| `research` | Background read of primary sources into a cited note |
| `writing-for-agents` | How to write CLAUDE.md, rules and skills |

`grilling`, `domain-modeling`, `tdd`, `code-review`, `codebase-design` and
`writing-for-agents` are also dependencies: `grill-with-docs`, `implement`,
`tdd` and `retro` call them through the Skill tool.

### Left out, and why

* **`triage`, `wayfinder`, `implement-spec`**: they run an issue queue (labels,
  claims, parallel implementers on an integration branch). smply's backlog is
  the roadmap, worked one item at a time (ADR-0018).
* **`handoff`**: smply keeps no session log; state goes into the repository,
  and `docs/handoff.md` already means something else here.
* **`pr`**: the PR body shape is `.github/pull_request_template.md`.
* **`ask-matt`**: a router over the whole upstream set, most of it not
  installed.
* **`improve-codebase-architecture`, `prototype`, `wizard`**: an HTML report,
  throwaway UI prototypes and setup wizards; none fits a library whose
  architecture is held by ADRs. Add one later if a need appears.
* **`grill-me`, `teach`, `to-questionnaire`, `wait-what`**: general productivity,
  not engineering workflow.
* **`misc/` and `in-progress/`**: frozen or beta upstream.

### Where the skills' assumptions differ from this project

The skills were written for GitHub Issues, `GLOSSARY.md` and `docs/adr/`.
`docs/agents/issue-tracker.md` and `docs/agents/domain.md` say what each of
those means here, and `CLAUDE.md` tells every skill to read them first:

* "the issue tracker" is `docs/roadmap.md` plus a plan document, never `gh`;
* `ready-for-agent` is a plan's Status line, not a label;
* ADRs are `docs/decisions/ADR-NNNN-*.md` in the full format, superseded,
  never edited;
* "type-check" in `/implement` means building a preset; "the full test suite"
  means the checklist in `docs/handoff.md`.

## Hook

`check_format.py` runs after every Edit or Write. If the file is one of
smply's C++ sources (the set `tools/sources.sh` lists), it runs
`clang-format --dry-run --Werror` on that file with the repository's
`.clang-format` and reports differences to Claude, which then fixes them. It
never rewrites anything, and it checks only the file just edited.

* Needs `python3` and `clang-format` on `PATH` (`CLANG_FORMAT` overrides the
  binary, as for `tools/format.sh`). Without clang-format it says so and
  skips. CI's format job is the authority; the hook only gets the answer
  sooner. Use the clang-format major version CI uses (Ubuntu 24.04's, 18), or
  the two can disagree.
* Native Windows: Claude Code runs hooks with Git Bash when it is installed.
  The Python installer from python.org provides `python`, not `python3`; if
  `python3` is missing the hook fails with a visible, non-blocking error.
  `docs/handoff.md` already recommends running the Linux tooling from a WSL
  clone, where both work.
* Builds, clang-tidy, cppcheck and `check_docs.py` are deliberately not hooks:
  they take seconds to minutes and belong to the finishing checklist and CI.

## Validating

```sh
python3 .claude/validate.py
```

It checks every skill's frontmatter against the Agent Skills specification,
that the files each skill links to exist, that every skill another one calls
is installed, that the copies still match `skills-lock.json`, that the hook
agrees with `tools/sources.sh`, and runs the hook on a well-formatted and a
badly formatted file. To see what Claude Code itself loaded, run `/skills` in a
session, or `/context` for the instruction files.

## Updating the skills

Updates are manual and reviewed like any dependency bump. Do not use
`npx skills update` (it takes upstream `HEAD`) or the self-updating
`mattpocock-skills` plugin, which would install every skill a second time.

1. Pick the upstream commit and read what changed:
   `git log b0618bc..<new-sha>` in a clone of mattpocock/skills, or its
   CHANGELOG. Read every changed `SKILL.md` and any script before taking it.
2. Re-install the same set at that commit, with telemetry off:

   ```sh
   DISABLE_TELEMETRY=1 npx -y skills@1.7.1 add mattpocock/skills#<new-sha> \
     -a claude-code --copy -y \
     -s setup-matt-pocock-skills -s grill-with-docs -s grilling \
     -s domain-modeling -s to-spec -s to-tickets -s implement -s tdd \
     -s code-review -s codebase-design -s diagnosing-bugs -s research \
     -s retro -s writing-for-agents
   ```

   `--copy` writes real files (symlinks break on Windows checkouts); `-a
   claude-code` writes only `.claude/skills/`. The command overwrites the
   listed skills and `skills-lock.json` and touches nothing else. A skill
   removed upstream must be deleted by hand.
3. Update the commit in this file and in `.claude/skills/THIRD-PARTY-NOTICES.md`,
   and the licence text there if it changed.
4. Run `python3 .claude/validate.py`, then `git diff`: check new references to
   GitHub, `.scratch/` or `docs/adr/` against `docs/agents/`, and new skill
   dependencies against the installed set.
5. Commit the skills, the lock file and these notes together, saying in the
   message what changed upstream and why it was taken.

Adding or dropping a skill is the same procedure with a changed `-s` list,
plus this file's tables.
