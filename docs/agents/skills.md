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
| `docs/agents/issue-tracker.md`, `domain.md`, `triage-labels.md` | Where the skills read and write project state |

## The skills

Upstream: [mattpocock/skills](https://github.com/mattpocock/skills), MIT, at
commit `b0618bc436ad893b3c5e84e55fba86586d34a404` (2026-10-08). The licence and
provenance are in `.claude/skills/THIRD-PARTY-NOTICES.md`. The copies are
byte-identical to that commit: the project's adaptations are in
`docs/agents/` and `CLAUDE.md`, never in the skill files, so an update is a
clean diff.

All 27 of upstream's stable skills (its `engineering/` and `productivity/`
buckets) are installed. Upstream's `misc/` (frozen) and `in-progress/` (beta)
buckets are deliberately left out.

User-invoked (type the name; the model cannot start them, and nor can another
skill):

| Skill | Use it to |
| ----- | --------- |
| `/ask-matt` | Ask which skill or flow fits the situation |
| `/grill-with-docs` | Interview you about a plan until it is settled, recording terms and ADR candidates as they resolve |
| `/grill-me` | The same interview, recording nothing |
| `/wayfinder` | Chart work too big for one session as a map of decision tickets in a plan document |
| `/to-spec` | Turn the conversation into a plan document, published per `issue-tracker.md` |
| `/to-tickets` | Break a plan into stages with blocking edges, in that plan document |
| `/implement` | Work a plan stage test-first, then review it, then commit |
| `/implement-spec` | Work a whole plan's stages with parallel implementers, merged on one branch |
| `/triage` | Move roadmap rows through triage states (`triage-labels.md`) |
| `/improve-codebase-architecture` | Find deepening opportunities, as an HTML report in the temp directory, then grill one |
| `/handoff` | Summarise the session into a file in the OS temp directory for another agent |
| `/to-questionnaire` | Write a questionnaire for someone who holds the answer you lack |
| `/teach` | Teach a topic over several sessions, in a `.local/` workspace |
| `/wait-what` | Re-pitch a message that did not land |
| `/retro` | Suggest improvements to this setup after a session |
| `/setup-matt-pocock-skills` | Re-run the tracker and domain setup. Already done; edit `docs/agents/` instead |

Model-invoked (Claude may also reach for these itself, so each description is
always in context):

| Skill | Use it for |
| ----- | ---------- |
| `code-review` | Two-axis review of a diff: the project's standards, and the plan it implements. **Replaces the built-in `/code-review`**; the built-in reviewer is still `/review` |
| `tdd` | Red-green loop at agreed seams, with Catch2 and the doubles in `docs/testing.md` |
| `diagnosing-bugs` | Build a tight failing loop first, then hypothesise and fix |
| `codebase-design` | Deep-module vocabulary for interface and seam decisions |
| `domain-modeling` | Sharpen terms and decisions, writing to `GLOSSARY.md` and `docs/decisions/` |
| `grilling` | The interview behind `/grill-with-docs`, `/grill-me` and `/triage` |
| `research` | Background read of primary sources into a cited note |
| `writing-for-agents` | How to write CLAUDE.md, rules and skills |
| `pr` | The content of a PR body, inside the repository's template |
| `prototype` | Throwaway code that answers a design question. **Replaces the built-in `/prototype`** |
| `wizard` | A bash script that walks a person through steps only they can take |

Several are also dependencies, called through the Skill tool:
`grill-with-docs`, `grill-me`, `triage`, `wayfinder` and
`improve-codebase-architecture` call `grilling` (and all but `grill-me` call
`domain-modeling`); `wayfinder` also calls `prototype` and `research`;
`implement` and `implement-spec` call `tdd` and `code-review`; `tdd` and
`improve-codebase-architecture` call `codebase-design`; `retro` calls
`writing-for-agents`. `.claude/validate.py` checks every such
call resolves.

### Where the skills' assumptions differ from this project

The skills were written for GitHub Issues, `GLOSSARY.md`, `docs/adr/` and a
writable working directory. `CLAUDE.md` tells every skill that these files
win over its own text:

* "the issue tracker" is `docs/roadmap.md` plus a plan document, never `gh`
  ([`issue-tracker.md`](issue-tracker.md)); `/wayfinder`'s map and
  `/implement-spec`'s tickets live in a plan document too;
* triage roles are recorded on roadmap rows, not as labels, and rejected
  requests become resolved open questions, not `.out-of-scope/` files
  ([`triage-labels.md`](triage-labels.md));
* ADRs are `docs/decisions/ADR-NNNN-*.md` in the full format, superseded,
  never edited ([`domain.md`](domain.md)); `/wait-what` and the other
  `GLOSSARY.md` readers fall back to the defining documents named there;
* "type-check" in `/implement` means building a preset; "the full test suite"
  means the checklist in `docs/handoff.md`;
* `pr` fills `.github/pull_request_template.md`: its Summary and Merge Danger
  go under "What changed, and why", its Evidence under "Checks run locally",
  and the template's `Docs-Impact: none` rule stands;
* `prototype` code goes on a `prototype/<slug>` branch, under a top-level
  `prototypes/<slug>/` that no gate scans, and is never merged;
* `/teach` uses `.local/teach/<topic>/` as its workspace and
  `/to-questionnaire` writes to `.local/questionnaires/`, never the repository
  root. `.local/` is git-ignored: personal agent output, never project state;
* a `wizard` writes local values to `.local/<name>.env` (set `ENV_FILE`, since
  the template defaults to `.env`) and GitHub secrets only after its `confirm`
  step. It is ephemeral in `.local/` unless it becomes a repeatable bench
  procedure, which is committed under `tests/hil/tools/`;
* `/handoff` is unrelated to `docs/handoff.md`. Its file lives in the OS temp
  directory; anything that must outlive the session still goes into the
  repository;
* `/improve-codebase-architecture` treats an accepted ADR as settled: a
  candidate that contradicts one goes through the supersede process.

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

Updates are manual and reviewed like any dependency bump: re-install at an
explicit upstream commit someone has read. Do not use `npx skills update`,
which chooses the version for you, or the self-updating `mattpocock-skills`
plugin, which would install every skill a second time.

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
     -s retro -s writing-for-agents -s triage -s wayfinder \
     -s implement-spec -s pr -s ask-matt -s improve-codebase-architecture \
     -s prototype -s wizard -s handoff -s grill-me -s teach \
     -s to-questionnaire -s wait-what
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
