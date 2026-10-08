#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate the shared Claude Code setup (docs/agents/skills.md).

Checks, each reported PASS or FAIL:

  skills    every .claude/skills/<dir>/SKILL.md has frontmatter that meets the
            Agent Skills specification (name = directory, 1-64 lowercase
            alphanumerics and single hyphens; description 1-1024 characters)
            and uses only fields Claude Code knows;
  links     every relative Markdown link inside a skill resolves;
  deps      every skill a skill calls through the Skill tool is installed and
            model-invocable, and no installed skill calls an uninstalled one;
  lock      each skill's files hash to the computedHash in skills-lock.json,
            the way the skills CLI computes it, so a local edit to a vendored
            skill is visible;
  rules     every .claude/rules/*.md has a paths list, and every repository
            path it names in backticks exists;
  settings  .claude/settings.json parses and its hook script exists;
  sources   the hook's idea of "our C++ sources" equals tools/sources.sh;
  hook      the hook passes a formatted file, reports a misformatted one
            (exit 2), ignores a non-source file, and notes a missing
            clang-format without blocking;
  docs      the files CLAUDE.md sends the skills to exist;
  git       nothing under .claude/ is a symlink, and every file is tracked
            (a warning before the first commit, not a failure).

Usage: python3 .claude/validate.py
Needs Python 3.8+, git, bash (for tools/sources.sh) and clang-format (the hook
case is skipped, and says so, without it).
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CLAUDE = REPO / ".claude"
SKILLS = CLAUDE / "skills"
HOOK = CLAUDE / "hooks" / "check_format.py"

# Frontmatter fields Claude Code reads (code.claude.com/docs/en/skills).
KNOWN_FIELDS = {
    "name", "description", "when_to_use", "argument-hint", "arguments",
    "disable-model-invocation", "user-invocable", "allowed-tools", "disallowed-tools",
    "model", "effort", "context", "agent", "background", "hooks", "paths", "shell",
    "metadata", "license", "compatibility",
}
NAME = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*$")
SKILL_CALL = re.compile(r'Skill tool[^.\n]*?"([a-z0-9-]+)"(?:\s+and\s+"([a-z0-9-]+)")?')
MD_LINK = re.compile(r"\]\(([^)#\s]+)(?:#[^)]*)?\)")

results: list[tuple[str, bool, str]] = []


def record(check: str, ok: bool, detail: str = "") -> None:
    results.append((check, ok, detail))


def frontmatter(text: str) -> dict[str, object] | None:
    if not text.startswith("---\n"):
        return None
    end = text.find("\n---", 4)
    if end < 0:
        return None
    block = text[4:end]
    try:
        import yaml  # optional: exact parsing when PyYAML is present
        data = yaml.safe_load(block)
        return data if isinstance(data, dict) else None
    except ImportError:
        pass
    data: dict[str, object] = {}
    for line in block.splitlines():
        m = re.match(r"^([A-Za-z_-]+):\s*(.*)$", line)
        if m:
            value = m.group(2).strip()
            if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
                value = value[1:-1].replace('\\"', '"')
            data[m.group(1)] = {"true": True, "false": False}.get(value, value)
    return data


def skill_dirs() -> list[Path]:
    return sorted(p for p in SKILLS.iterdir() if p.is_dir())


def check_skills() -> dict[str, dict[str, object]]:
    meta: dict[str, dict[str, object]] = {}
    for d in skill_dirs():
        skill_md = d / "SKILL.md"
        if not skill_md.is_file():
            record("skills", False, f"{d.name}: no SKILL.md")
            continue
        fm = frontmatter(skill_md.read_text(encoding="utf-8"))
        if fm is None:
            record("skills", False, f"{d.name}: no YAML frontmatter")
            continue
        problems = []
        name = fm.get("name")
        if name != d.name:
            problems.append(f"name {name!r} does not match directory")
        if not isinstance(name, str) or len(name) > 64 or not NAME.match(name):
            problems.append("name is not 1-64 lowercase alphanumerics and single hyphens")
        desc = fm.get("description")
        if not isinstance(desc, str) or not 1 <= len(desc) <= 1024:
            problems.append("description missing or longer than 1024 characters")
        listing = f"{desc or ''}{fm.get('when_to_use') or ''}"
        if len(listing) > 1536:
            problems.append("description + when_to_use exceed Claude Code's 1536-character listing")
        unknown = set(fm) - KNOWN_FIELDS
        if unknown:
            problems.append(f"unknown frontmatter field(s): {', '.join(sorted(unknown))}")
        record("skills", not problems, f"{d.name}: " + ("; ".join(problems) or "ok"))
        meta[d.name] = fm
    return meta


def check_links() -> None:
    for d in skill_dirs():
        for md in sorted(d.rglob("*.md")):
            prose = re.sub(r"^```.*?^```", "", md.read_text(encoding="utf-8"),
                           flags=re.S | re.M)  # examples in code blocks are not links
            for target in MD_LINK.findall(prose):
                if re.match(r"^[a-z]+:", target):
                    continue  # a URL
                ok = (md.parent / target).exists()
                if not ok:
                    record("links", False, f"{md.relative_to(REPO).as_posix()} -> {target}")
    if not any(c == "links" and not ok for c, ok, _ in results):
        record("links", True, "every relative link inside .claude/skills resolves")


def check_deps(meta: dict[str, dict[str, object]]) -> None:
    calls = 0
    for d in skill_dirs():
        for md in sorted(d.rglob("*.md")):
            for m in SKILL_CALL.finditer(md.read_text(encoding="utf-8")):
                for target in filter(None, m.groups()):
                    calls += 1
                    where = f"{d.name} calls {target}"
                    if target not in meta:
                        record("deps", False, f"{where}, which is not installed")
                    elif meta[target].get("disable-model-invocation") is True:
                        record("deps", False, f"{where}, which only a user can invoke")
                    else:
                        record("deps", True, where)
    if calls == 0:
        record("deps", False, "found no Skill-tool calls; the pattern no longer matches upstream")


def cli_hash(skill_dir: Path) -> str:
    """The skills CLI's computeSkillFolderHash: sha256 over (path, bytes) pairs,
    sorted as JavaScript's localeCompare would. CRLF is folded to LF so a Windows
    checkout (core.autocrlf=true) hashes the same as the upstream files."""
    files = [p for p in skill_dir.rglob("*") if p.is_file()]
    rels = sorted((p.relative_to(skill_dir).as_posix() for p in files),
                  key=lambda s: (s.lower(), s.swapcase()))
    h = hashlib.sha256()
    for rel in rels:
        h.update(rel.encode("utf-8"))
        h.update((skill_dir / rel).read_bytes().replace(b"\r\n", b"\n"))
    return h.hexdigest()


def check_lock() -> None:
    lock_path = REPO / "skills-lock.json"
    try:
        lock = json.loads(lock_path.read_text(encoding="utf-8"))["skills"]
    except (OSError, KeyError, json.JSONDecodeError) as exc:
        record("lock", False, f"skills-lock.json unreadable: {exc}")
        return
    installed = {d.name for d in skill_dirs()}
    for name in sorted(installed | set(lock)):
        if name not in lock:
            record("lock", False, f"{name}: installed but not in skills-lock.json")
        elif name not in installed:
            record("lock", False, f"{name}: in skills-lock.json but not installed")
        else:
            entry = lock[name]
            ok = cli_hash(SKILLS / name) == entry.get("computedHash")
            pinned = bool(entry.get("ref"))
            record("lock", ok and pinned,
                   f"{name}: " + ("matches " if ok else "DIFFERS from ")
                   + f"{entry.get('source')}@{str(entry.get('ref'))[:10]}"
                   + ("" if pinned else " (no pinned ref)"))


def check_rules() -> None:
    for rule in sorted((CLAUDE / "rules").glob("*.md")):
        text = rule.read_text(encoding="utf-8")
        fm = frontmatter(text) or {}
        paths = fm.get("paths")
        rel = rule.relative_to(REPO).as_posix()
        if not isinstance(paths, list) or not paths:
            record("rules", False, f"{rel}: no paths list (it would load every session)")
            continue
        missing = []
        for token in re.findall(r"`([^`\s]+)`", text):
            candidate = token.rstrip("/")
            looks_like_path = "/" in candidate or re.search(r"\.(md|py|sh|hpp|json)$", candidate)
            if not looks_like_path or any(c in candidate for c in "<>*"):
                continue
            if not (REPO / candidate).exists():
                missing.append(token)
        record("rules", not missing,
               f"{rel}: " + (f"names missing path(s) {missing}" if missing else
                             f"{len(paths)} glob(s), named paths exist"))


def check_settings() -> None:
    try:
        settings = json.loads((CLAUDE / "settings.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        record("settings", False, f".claude/settings.json: {exc}")
        return
    commands = [h.get("command", "") for group in settings.get("hooks", {}).get("PostToolUse", [])
                for h in group.get("hooks", [])]
    ok = any(".claude/hooks/check_format.py" in c for c in commands) and HOOK.is_file()
    record("settings", ok, "PostToolUse runs .claude/hooks/check_format.py" if ok
           else "the format hook is not wired up")


def hook_module():
    sys.path.insert(0, str(HOOK.parent))
    try:
        import check_format  # type: ignore[import-not-found]
        return check_format
    finally:
        sys.path.pop(0)


def check_sources() -> None:
    listed = subprocess.run(["bash", "tools/sources.sh"], cwd=REPO, capture_output=True,
                            text=True, check=True).stdout.split()
    tracked = subprocess.run(["git", "ls-files"], cwd=REPO, capture_output=True, text=True,
                             check=True).stdout.split()
    hook = hook_module()
    listed_set = set(listed)
    wrong = [f for f in sorted(set(tracked) | listed_set)
             if (hook.first_party_source(REPO / f, REPO) is not None) != (f in listed_set)
             and (REPO / f).exists()]
    record("sources", not wrong,
           f"hook and tools/sources.sh agree on {len(listed)} sources" if not wrong
           else f"disagree on: {wrong[:5]}")


def run_hook(path: Path, env_extra: dict[str, str] | None = None) -> subprocess.CompletedProcess:
    env = dict(os.environ, CLAUDE_PROJECT_DIR=str(REPO), **(env_extra or {}))
    payload = json.dumps({"hook_event_name": "PostToolUse", "tool_name": "Edit",
                          "tool_input": {"file_path": str(path)}, "cwd": str(REPO)})
    return subprocess.run([sys.executable, str(HOOK)], input=payload, capture_output=True,
                          text=True, env=env, timeout=60, check=False)


def check_hook() -> None:
    clang_format = os.environ.get("CLANG_FORMAT", "clang-format")
    good = ("// SPDX-License-Identifier: Apache-2.0\n\nnamespace smply {\n\n"
            "int answer()\n{\n    return 42;\n}\n\n} // namespace smply\n")
    bad = good.replace("    return 42;", "  return   42 ;")
    fd, name = tempfile.mkstemp(prefix="claude_hook_probe_", suffix=".cpp", dir=REPO / "tests")
    os.close(fd)
    probe = Path(name)
    try:
        if shutil.which(clang_format) is None:
            record("hook", True, f"SKIPPED the formatting cases: {clang_format} not on PATH")
        else:
            probe.write_text(good, encoding="utf-8")
            r = run_hook(probe)
            record("hook", r.returncode == 0 and not r.stderr,
                   f"formatted file: exit {r.returncode} (want 0)")
            probe.write_text(bad, encoding="utf-8")
            r = run_hook(probe)
            record("hook", r.returncode == 2 and "clang-format-violations" in r.stderr
                   and probe.name in r.stderr,
                   f"misformatted file: exit {r.returncode} (want 2, with diagnostics)")
            record("hook", probe.read_text(encoding="utf-8") == bad,
                   "the hook left the misformatted file unchanged")
        r = run_hook(probe, {"CLANG_FORMAT": "clang-format-that-does-not-exist"})
        record("hook", r.returncode == 0 and "not on PATH" in r.stdout,
               f"missing clang-format: exit {r.returncode}, noted without blocking")
        r = run_hook(REPO / "docs" / "roadmap.md")
        record("hook", r.returncode == 0 and not r.stdout and not r.stderr,
               "non-source file ignored")
    finally:
        probe.unlink(missing_ok=True)


def check_docs() -> None:
    for rel in ("docs/agents/issue-tracker.md", "docs/agents/domain.md",
                "docs/agents/skills.md", ".claude/skills/THIRD-PARTY-NOTICES.md"):
        ok = (REPO / rel).is_file()
        record("docs", ok, rel + (" exists" if ok else " is missing"))
    claude_md = (REPO / "CLAUDE.md").read_text(encoding="utf-8")
    ok = all(f"docs/agents/{n}" in claude_md for n in ("issue-tracker.md", "domain.md", "skills.md"))
    record("docs", ok, "CLAUDE.md points the skills at docs/agents/")


def check_git() -> None:
    links = [p for p in CLAUDE.rglob("*") if p.is_symlink()]
    record("git", not links, "no symlinks under .claude/" if not links else f"symlinks: {links}")
    untracked = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard", ".claude", "skills-lock.json",
         "docs/agents"], cwd=REPO, capture_output=True, text=True, check=True).stdout.split()
    if untracked:
        print(f"WARN  git: {len(untracked)} file(s) not yet tracked, e.g. {untracked[0]}")
    else:
        record("git", True, "every file of the setup is tracked")


def main() -> int:
    meta = check_skills()
    check_links()
    check_deps(meta)
    check_lock()
    check_rules()
    check_settings()
    check_sources()
    check_hook()
    check_docs()
    check_git()
    failed = 0
    for check, ok, detail in results:
        print(f"{'PASS' if ok else 'FAIL'}  {check}: {detail}")
        failed += not ok
    print(f"\n{len(results) - failed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
