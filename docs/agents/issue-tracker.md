# Issue tracker: the roadmap, in the repository

smply tracks its work in the repository, not in an external tracker
([ADR-0018](../decisions/ADR-0018-maintenance-process.md)).
[`roadmap.md`](../roadmap.md) is the backlog. A piece of work too big for one
roadmap row gets a plan document in `docs/`, which the roadmap's "In progress"
section points at and which is deleted when the work is done. The quality
review's `docs/review-plan.md` was the first; `git show 0ae02de^:docs/review-plan.md`
shows its shape.

GitHub Issues are not the tracker. The only issues are the ones
`nightly-fuzz.yml` opens for a fuzz crash, labelled `fuzz`. Security reports go
through [`SECURITY.md`](../../SECURITY.md). So skills must not call `gh issue`,
`glab`, or write under `.scratch/`.

## When a skill says "publish to the issue tracker"

* **A spec** (`/to-spec`): write it to `docs/<slug>-plan.md`, using the skill's
  template, under a short "Status" line. Then add a line to the roadmap
  pointing at it: under "In progress" if work starts now, otherwise as a
  backlog row in the matching area with its "When". One plan is in progress at
  a time.
* **Tickets** (`/to-tickets`): write them into that plan as numbered stages
  under a `## Stages` heading, one `### Stage N: <title>` per ticket with its
  "Blocked by", what it delivers, and its acceptance criteria as checkboxes.
  Not one file per ticket, and not a separate tickets file.
* **A small, self-contained item**: a roadmap backlog row, in the area it
  belongs to, with its "When". A question that needs a decision before work
  can start is an open question (`O<n>`, the next free number) in the roadmap's
  table.

Every write is a normal repository change: it goes through a commit and a pull
request like code, and `tools/check_docs.py` must pass on it.

## When a skill says "fetch the relevant ticket"

Read the roadmap row or plan stage the user names, or the plan document the
roadmap's "In progress" section points at. Reference them by roadmap heading,
plan path and stage number, or open-question ID: there are no issue numbers.

## Triage states

The `triage` skill is not installed, and there are no triage labels. Where a
skill says to apply `ready-for-agent`, record it as the plan's Status line
(`Status: ready for implementation`). Whether a human or an agent works an item
is decided when it is picked up, not labelled in advance.

## Closing work

A stage is checked off in its plan as it lands, in the same commit. When the
last stage lands, the plan file is deleted and its roadmap line removed. The
commit message carries the reasoning; there is no other history.
