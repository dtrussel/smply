# Triage roles, without labels

`/triage` speaks in five state roles and two category roles. smply's tracker is
[`roadmap.md`](../roadmap.md) ([`issue-tracker.md`](issue-tracker.md)), which
has no labels, so each role is recorded in the roadmap itself. No new roadmap
section exists for triage, and nothing is created on GitHub.

| Role | How it is recorded |
| ---- | ------------------ |
| `needs-triage` | A backlog row in the area table it belongs to, with "When" `to triage` |
| `needs-info` | The same row, with "When" `waiting on <who>: <what>` |
| `ready-for-agent` | Promoted to a plan document with `Status: ready for implementation`, linked from the roadmap ([`issue-tracker.md`](issue-tracker.md)) |
| `ready-for-human` | Work only a person can do. Bench work moves under "Acceptance gaps that need the hardware bench"; anything else keeps its row, with "When" naming the person |
| `wontfix` | The row is deleted; the commit message says why |
| `bug` / `enhancement` | The row's first words: `**Bug:** …` or `**Enhancement:** …` |

"Show me what needs attention" means the rows whose "When" is `to triage` or
`waiting on …`, oldest in `git log` first.

## Rejected requests

Upstream keeps rejected requests in `.out-of-scope/`. Here a rejection worth
remembering is a resolved open question in the roadmap's table, "Resolved:
no", with the reasoning, as O2 and O4 are. A rejection that is a design
decision is an ADR. There is no `.out-of-scope/` directory.

## Pull requests

**PRs as a request surface: no.** Triage covers roadmap rows only.

## Comments

Triage writes no comments: every change is a repository edit, made in a
commit like any other. The AI-generated disclaimer the skill asks for goes in
that commit's message.
