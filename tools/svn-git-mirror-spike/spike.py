#!/usr/bin/env python3
"""Throwaway spike for issue #521: a Git mirror of an SVN checkout that UAM owns.

This is NOT product code. It exists to test the design in
docs/reference/svn-git-mirror-design.md against a disposable local file:// SVN
repository, and its printed results are recorded there. Nothing here is
imported by UAM, and it does not need git svn or the Perl SVN bindings.

Run:  python3 tools/svn-git-mirror-spike/spike.py [--keep]
Needs: svn, svnadmin (1.10 or newer) and git (2.25 or newer) on PATH.
Every repository, checkout and mirror is created under a new temporary folder,
which is removed at the end unless --keep is given.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse
import xml.etree.ElementTree as ET
from pathlib import Path

# A fixed client ignore list, so the run does not depend on the person's ~/.subversion/config.
CLIENT_GLOBAL_IGNORES = "*.o *.lo *.la *.al .libs *.so *.so.[0-9]* *.a *.pyc *.pyo __pycache__ *.rej *~ #*# .#* .*.swp .DS_Store [Tt]humbs.db"
REVPROP_COMMIT = "uam:git-commit"
REVPROP_BRANCH = "uam:git-branch"
OUT_OF_DATE_CODES = ("E155011", "E160028", "E170004", "E160024", "E155015", "E160020")


class SpikeError(Exception):
    """One plain line for the person, as UAM would show it."""


class Tools:
    def __init__(self, base: Path):
        self.config_dir = base / "svn-config"
        self.config_dir.mkdir()
        (self.config_dir / "config").write_text(f"[miscellany]\nglobal-ignores = {CLIENT_GLOBAL_IGNORES}\n", encoding="utf-8")
        self.git_env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT="0",
                            GIT_AUTHOR_NAME="Agent", GIT_AUTHOR_EMAIL="agent@example.invalid",
                            GIT_COMMITTER_NAME="Agent", GIT_COMMITTER_EMAIL="agent@example.invalid", LC_ALL="C")

    def svn(self, *args: str, cwd: Path | None = None, user: str = "david", check: bool = True) -> subprocess.CompletedProcess:
        cmd = ["svn", "--non-interactive", "--config-dir", str(self.config_dir), "--username", user, *args]
        res = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, env=dict(os.environ, LC_ALL="C"))
        if check and res.returncode != 0:
            raise RuntimeError(f"svn {' '.join(args)} failed: {res.stderr.strip()}")
        return res

    def git(self, *args: str, git_dir: Path | None = None, work_tree: Path | None = None, index: Path | None = None,
            cwd: Path | None = None, env: dict | None = None, check: bool = True, input: str | None = None) -> subprocess.CompletedProcess:
        e = dict(self.git_env)
        if index is not None:
            e["GIT_INDEX_FILE"] = str(index)
        if env:
            e.update(env)
        cmd = ["git"]
        if git_dir is not None:
            cmd += ["--git-dir", str(git_dir)]
        if work_tree is not None:
            cmd += ["--work-tree", str(work_tree)]
        res = subprocess.run(cmd + list(args), cwd=cwd, capture_output=True, text=True, env=e, input=input)
        if check and res.returncode != 0:
            raise RuntimeError(f"git {' '.join(args)} failed: {res.stderr.strip()}")
        return res


def glob_escape(text: str) -> str:
    """Escape Git glob characters in a literal folder name, so `a[1]` means that folder and not `a1`."""
    return re.sub(r"([*?\[\\])", r"\\\1", text)


def svn_pattern_to_git(directory: str, pattern: str, recursive: bool) -> str | None:
    """Translate one SVN ignore pattern to a Git exclude line.

    SVN matches a pattern against an item's name only, so a pattern containing '/'
    never matches anything in SVN and is dropped. svn:ignore covers the folder's
    direct children; svn:global-ignores and the client list cover every descendant.
    """
    pattern = pattern.strip()
    if not pattern or "/" in pattern:
        return None
    if pattern[0] in "#!":
        pattern = "\\" + pattern
    if pattern.endswith(" "):
        pattern = pattern[:-1] + "\\ "
    prefix = f"/{glob_escape(directory)}" if directory else ""
    if recursive:
        return f"{prefix}/**/{pattern}" if directory else pattern
    return f"{prefix}/{pattern}"


def svn_arg(path: str) -> str:
    """A trailing @ stops SVN reading an @ in a file name as a peg revision."""
    return path + "@"


def ref_component(segment: str) -> str:
    """Percent-encode one SVN path segment so any folder name is a valid Git ref component."""
    return urllib.parse.quote(segment, safe="-_").replace(".", "%2E").replace("~", "%7E")


KEYWORD_NAMES = {"Id": ["Id"], "Header": ["Header"], "Date": ["Date", "LastChangedDate"], "LastChangedDate": ["Date", "LastChangedDate"],
                 "Rev": ["Rev", "Revision", "LastChangedRevision"], "Revision": ["Rev", "Revision", "LastChangedRevision"],
                 "LastChangedRevision": ["Rev", "Revision", "LastChangedRevision"], "Author": ["Author", "LastChangedBy"],
                 "LastChangedBy": ["Author", "LastChangedBy"], "URL": ["URL", "HeadURL"], "HeadURL": ["URL", "HeadURL"]}


class Mirror:
    """One SVN repository's Git mirror: a bare repository, one bridge checkout per SVN path,
    an upstream ref per path, a revision map, a state file and a publish journal.

    Write order for every imported revision: the Git commit object, then the revision map line (the authority),
    then the upstream ref, then the state file. reconcile() repairs a stop between any two of them."""

    def __init__(self, tools: Tools, home: Path, repo_root_url: str):
        self.t = tools
        self.home = home
        self.repo_root_url = repo_root_url
        self.git_dir = home / "mirror.git"
        self.revmap_path = home / "revmap.jsonl"
        self.journal_path = home / "publish-journal.json"
        self.state_path = home / "state.json"
        self.log: list[str] = []  # Stands in for UAM's diagnostic log: raw SVN output goes here, never to the person.
        if not self.git_dir.exists():
            home.mkdir(parents=True, exist_ok=True)
            self.t.git("init", "-q", "--bare", str(self.git_dir))
            self.t.git("config", "core.autocrlf", "false", git_dir=self.git_dir)
            self.t.git("config", "core.symlinks", "true", git_dir=self.git_dir)
            self.state_path.write_text(json.dumps({"paths": {}}), encoding="utf-8")
        # Simulated fault injection for the spike scenarios.
        self.before_commit = None
        self.crash_after_commit = False
        self.crash_after_revmap = False
        self.crash_after_publish_pull = False
        self.publishing = False

    # ----- small helpers -----
    def state(self) -> dict:
        return json.loads(self.state_path.read_text(encoding="utf-8"))

    def save_state(self, state: dict) -> None:
        tmp = self.state_path.with_suffix(".tmp")
        tmp.write_text(json.dumps(state, indent=1), encoding="utf-8")
        os.replace(tmp, self.state_path)

    def slug(self, svn_path: str) -> str:
        # A hash keeps distinct SVN paths apart (`a/b` and `a__b` would collide if only separators were replaced).
        return hashlib.sha256(svn_path.encode("utf-8")).hexdigest()[:16]

    def bridge(self, svn_path: str) -> Path:
        return self.home / "bridges" / self.slug(svn_path)

    def index(self, svn_path: str) -> Path:
        return self.home / ("index-" + self.slug(svn_path))

    def ref(self, svn_path: str) -> str:
        return "refs/uam-svn/upstream/" + "/".join(ref_component(s) for s in svn_path.split("/"))

    def url(self, svn_path: str) -> str:
        return f"{self.repo_root_url}/{urllib.parse.quote(svn_path)}"

    def g(self, svn_path: str, *args: str, **kw) -> subprocess.CompletedProcess:
        return self.t.git(*args, git_dir=self.git_dir, work_tree=self.bridge(svn_path), index=self.index(svn_path), **kw)

    def svn_targets(self, svn_path: str, *args: str, paths: list[str]) -> subprocess.CompletedProcess:
        """Many paths through a --targets file: no command-line length limit, no option or peg-revision parsing of names."""
        targets = self.home / "targets.txt"
        targets.write_text("".join(svn_arg(p) + "\n" for p in paths), encoding="utf-8")
        return self.t.svn(*args, "--targets", str(targets), cwd=self.bridge(svn_path))

    def subject(self, commit: str) -> str:
        return self.t.git("log", "-1", "--format=%s", commit, git_dir=self.git_dir).stdout.strip()

    def tip(self, svn_path: str) -> str | None:
        res = self.t.git("rev-parse", "--verify", "-q", self.ref(svn_path), git_dir=self.git_dir, check=False)
        return res.stdout.strip() or None

    def map_revision(self, rev: int, svn_path: str, commit: str, published_from: str | None) -> None:
        with self.revmap_path.open("a", encoding="utf-8") as f:
            f.write(json.dumps({"rev": rev, "path": svn_path, "commit": commit, "published_from": published_from}) + "\n")
            f.flush()
            os.fsync(f.fileno())

    def revmap(self) -> list[dict]:
        if not self.revmap_path.exists():
            return []
        return [json.loads(line) for line in self.revmap_path.read_text(encoding="utf-8").splitlines() if line]

    def write_journal(self, entry: dict | None) -> None:
        if entry is None:
            self.journal_path.unlink(missing_ok=True)
            return
        tmp = self.journal_path.with_suffix(".tmp")
        with tmp.open("w", encoding="utf-8") as f:
            json.dump(entry, f)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, self.journal_path)

    def reconcile(self, svn_path: str) -> None:
        """The revision map is the authority. Repair a stop between its line and the ref or state file;
        refuse when the upstream ref was moved by anything else (an agent's `git update-ref`, for example)."""
        entries = [e for e in self.revmap() if e["path"] == svn_path]
        last = entries[-1]
        tip = self.tip(svn_path)
        if tip != last["commit"]:
            parent = self.t.git("rev-parse", "-q", "--verify", f"{last['commit']}^", git_dir=self.git_dir, check=False).stdout.strip()
            previous = entries[-2]["commit"] if len(entries) > 1 else None
            if tip is not None and tip == previous and parent == previous:
                self.t.git("update-ref", self.ref(svn_path), last["commit"], tip, git_dir=self.git_dir)
            elif tip is None and len(entries) == 1:
                # A stop during attach, between the first map line and the ref.
                self.t.git("update-ref", self.ref(svn_path), last["commit"], "", git_dir=self.git_dir)
            else:
                raise SpikeError(f"The mirror of {svn_path} was changed outside UAM, so nothing was pulled or published.")
        state = self.state()
        entry = state["paths"].setdefault(svn_path, {"last_rev": last["rev"]})
        if entry["last_rev"] < last["rev"]:
            entry["last_rev"] = last["rev"]
        self.save_state(state)

    # ----- import (SVN -> Git, one way) -----
    def versioned_files(self, svn_path: str) -> list[str]:
        """Every versioned file in the bridge checkout, from SVN's own metadata, not from ignore rules."""
        out = self.t.svn("info", "-R", "--xml", ".", cwd=self.bridge(svn_path)).stdout
        files = []
        for entry in ET.fromstring(out).iter("entry"):
            schedule = entry.findtext("wc-info/schedule")
            if entry.get("kind") == "file" and schedule != "delete":
                files.append(entry.get("path").replace(os.sep, "/"))
        return sorted(files)

    def props(self, svn_path: str, name: str) -> dict[str, str]:
        cwd = self.bridge(svn_path)
        out = self.t.svn("propget", "-R", "--xml", name, ".", cwd=cwd).stdout
        return {Path(os.path.relpath(t.get("path"), cwd)).as_posix(): (t.findtext("property") or "") for t in ET.fromstring(out).iter("target")}

    def contract_keywords(self, svn_path: str) -> list[str]:
        """Rewrite svn:keywords files in the bridge with their keywords unexpanded, as the repository stores them,
        so Git does not see $Id$ and friends change on every revision. SVN still treats these files as unmodified."""
        cwd = self.bridge(svn_path)
        contracted = []
        for path in self.props(svn_path, "svn:keywords"):
            pristine = subprocess.run(["svn", "--non-interactive", "--config-dir", str(self.t.config_dir), "cat", "--ignore-keywords",
                                       "-r", "BASE", "--", svn_arg(path)], cwd=cwd, capture_output=True, check=True).stdout
            if (cwd / path).read_bytes() != pristine:
                (cwd / path).write_bytes(pristine)
            contracted.append(path)
        return contracted

    def stage_exact_tree(self, svn_path: str) -> str:
        """Index exactly SVN's versioned files (a versioned file that matches an ignore pattern is included,
        unversioned and ignored files are not) and return the tree id."""
        self.contract_keywords(svn_path)
        self.g(svn_path, "read-tree", "--empty")
        files = self.versioned_files(svn_path)
        if files:
            self.g(svn_path, "update-index", "--add", "-z", "--stdin", input="\0".join(files) + "\0")
        return self.g(svn_path, "write-tree").stdout.strip()

    def write_excludes(self, svn_path: str) -> list[str]:
        """Map svn:ignore, svn:global-ignores (own and inherited) and the client list to info/exclude,
        which every worktree of the mirror shares and which is never published."""
        cwd = self.bridge(svn_path)
        lines = ["# Written by UAM from SVN ignore rules; edit them in SVN, not here.", ".svn/"]
        lines += [p for p in (svn_pattern_to_git("", x, True) for x in CLIENT_GLOBAL_IGNORES.split()) if p]
        for prop, recursive in (("svn:ignore", False), ("svn:global-ignores", True)):
            for directory, value in self.props(svn_path, prop).items():
                # The XML names targets by absolute path; props() makes them relative to the mirror root.
                directory = "" if directory == "." else directory
                for pattern in value.split():
                    line = svn_pattern_to_git(directory, pattern, recursive)
                    if line:
                        lines.append(line)
        out = self.t.svn("propget", "--show-inherited-props", "--xml", "svn:global-ignores", ".", cwd=cwd).stdout
        for value in ET.fromstring(out).iter("inherited_property"):
            for pattern in (value.text or "").split():
                line = svn_pattern_to_git("", pattern, True)
                if line:
                    lines.append(line)
        (self.git_dir / "info").mkdir(exist_ok=True)
        (self.git_dir / "info" / "exclude").write_text("\n".join(dict.fromkeys(lines)) + "\n", encoding="utf-8")
        return lines

    def attach(self, svn_path: str, rev: int, parent_commit: str | None = None, copy_rev: int | None = None) -> str:
        """Start mirroring svn_path at rev (the checkout's own revision, not HEAD)."""
        bridge = self.bridge(svn_path)
        bridge.parent.mkdir(parents=True, exist_ok=True)
        self.t.svn("checkout", "-q", "--ignore-externals", f"{self.url(svn_path)}@{rev}", str(bridge))
        tree = self.stage_exact_tree(svn_path)
        if parent_commit is not None and self.t.git("rev-parse", f"{parent_commit}^{{tree}}", git_dir=self.git_dir).stdout.strip() == tree:
            commit = parent_commit  # A server-side copy changes no content; the branch starts at its origin commit.
        else:
            commit = self.commit_revision(svn_path, rev, tree, parent_commit)
        mapped = copy_rev if copy_rev is not None else rev
        self.map_revision(mapped, svn_path, commit, None)
        self.t.git("update-ref", self.ref(svn_path), commit, "", git_dir=self.git_dir)
        state = self.state()
        state["paths"][svn_path] = {"last_rev": mapped}
        self.save_state(state)
        self.write_excludes(svn_path)
        return commit

    def revision_info(self, svn_path: str, rev: int) -> dict:
        out = self.t.svn("log", "--xml", "--with-all-revprops", "-r", str(rev), self.repo_root_url).stdout
        entry = ET.fromstring(out).find("logentry")
        props = {p.get("name"): p.text or "" for p in entry.iter("property")}
        return {"author": entry.findtext("author") or "unknown", "date": entry.findtext("date"),
                "message": props.get("svn:log", entry.findtext("msg") or ""), "props": props}

    def commit_revision(self, svn_path: str, rev: int, tree: str, parent: str | None) -> str:
        info = self.revision_info(svn_path, rev)
        uuid = self.t.svn("info", "--show-item", "repos-uuid", self.repo_root_url).stdout.strip()
        message = info["message"].rstrip("\n") + f"\n\nUAM-SVN-Revision: {rev}\nUAM-SVN-Path: {svn_path}\n"
        date = info["date"]
        env = {"GIT_AUTHOR_NAME": info["author"], "GIT_AUTHOR_EMAIL": f"{info['author']}@{uuid}",
               "GIT_AUTHOR_DATE": date, "GIT_COMMITTER_DATE": date}
        args = ["commit-tree", tree, "-F", "-"] + (["-p", parent] if parent else [])
        return self.t.git(*args, git_dir=self.git_dir, env=env, input=message).stdout.strip()

    def revisions_on_path(self, svn_path: str, first: int, last: int) -> list[int]:
        if first > last:
            return []
        out = self.t.svn("log", "-q", "--xml", "-r", f"{first}:{last}", f"{self.url(svn_path)}@{last}").stdout
        return [int(e.get("revision")) for e in ET.fromstring(out).iter("logentry")]

    def pull(self, svn_path: str, up_to: int | None = None) -> list[tuple[int, str, str]]:
        """Explicit, one-way: bring SVN revisions that changed svn_path into its upstream ref.
        Only the bridge checkout and the upstream ref move; agent worktrees and branches never do."""
        self.settle_interrupted()
        self.reconcile(svn_path)
        journal = json.loads(self.journal_path.read_text(encoding="utf-8")) if self.journal_path.exists() else None
        if journal and journal.get("state") != "awaiting-pull" and not self.publishing:
            raise SpikeError("A publish was interrupted; UAM settles it before pulling.")
        state = self.state()
        last = state["paths"][svn_path]["last_rev"]
        head = int(self.t.svn("info", "--show-item", "revision", self.repo_root_url).stdout.strip())
        target = min(head, up_to) if up_to else head
        if target <= last:
            return []
        out = self.t.svn("log", "-q", "--xml", "-r", f"{last + 1}:{target}", f"{self.url(svn_path)}@{target}").stdout
        revisions = [int(e.get("revision")) for e in ET.fromstring(out).iter("logentry")]
        imported = []
        for rev in revisions:
            self.t.svn("update", "-q", "--ignore-externals", "--accept", "postpone", "-r", str(rev), ".", cwd=self.bridge(svn_path))
            tree = self.stage_exact_tree(svn_path)
            parent = self.tip(svn_path)
            info = self.revision_info(svn_path, rev)
            published = info["props"].get(REVPROP_COMMIT)
            commit = None
            how = "imported"
            if published:
                published_ok = self.t.git("cat-file", "-e", f"{published}^{{commit}}", git_dir=self.git_dir, check=False).returncode == 0
                if published_ok:
                    p_parent = self.t.git("rev-parse", f"{published}^", git_dir=self.git_dir).stdout.strip()
                    p_tree = self.t.git("rev-parse", f"{published}^{{tree}}", git_dir=self.git_dir).stdout.strip()
                    if p_parent == parent and p_tree == tree:
                        commit, how = published, "fast-forward to published commit"
            if commit is None:
                commit = self.commit_revision(svn_path, rev, tree, parent)
                if published:
                    how = "imported (SVN's content differs from the published commit)"
            self.map_revision(rev, svn_path, commit, published)
            if self.crash_after_revmap:
                self.crash_after_revmap = False
                raise KeyboardInterrupt("simulated crash between the revision map and the ref")
            self.t.git("update-ref", self.ref(svn_path), commit, parent or "", git_dir=self.git_dir)
            state["paths"][svn_path]["last_rev"] = rev
            self.save_state(state)
            imported.append((rev, commit, how))
        state["paths"][svn_path]["last_rev"] = target
        self.save_state(state)
        self.write_excludes(svn_path)
        if journal and journal.get("state") == "awaiting-pull" and journal["path"] == svn_path and target >= journal["rev"]:
            self.write_journal(None)  # The published revision is now mapped through its revision property.
        return imported

    # ----- publish (Git -> SVN) -----
    def published_commits(self) -> set[str]:
        return {e["published_from"] for e in self.revmap() if e.get("published_from")}

    def changed_paths(self, parent: str, commit: str) -> list[tuple[str, str, str, str]]:
        raw = self.t.git("diff-tree", "-r", "--no-renames", "--raw", "-z", parent, commit, git_dir=self.git_dir).stdout.split("\0")
        changes = []
        for i in range(0, len(raw) - 1, 2):
            old_mode, new_mode, _, _, status = raw[i].lstrip(":").split()
            changes.append((status, raw[i + 1], old_mode, new_mode))
        return changes

    def normal_form_problem(self, svn_path: str, commit: str, path: str) -> str | None:
        """Content SVN would rewrite as it stores it. Refused up front, so it is never mistaken for someone else's change."""
        keywords = self.props(svn_path, "svn:keywords").get(path, "")
        eol = self.props(svn_path, "svn:eol-style").get(path, "")
        if not keywords and not eol:
            return None
        blob = subprocess.run(["git", "--git-dir", str(self.git_dir), "cat-file", "blob", f"{commit}:{path}"],
                              capture_output=True, check=True, env=self.t.git_env).stdout
        names = sorted({n for k in keywords.split() for n in KEYWORD_NAMES.get(k, [k])})
        if names:
            match = re.search(rb"\$(" + b"|".join(re.escape(n.encode()) for n in names) + rb")::? [^$\n]*\$", blob)
            if match:
                name = match.group(1).decode()
                return f"{path} contains an expanded ${name}$ keyword; change it back to ${name}$ before publishing."
        if eol in ("native", "LF") and b"\r" in blob:
            return f"{path} has Windows line endings, but SVN stores it with Unix ones; convert it before publishing."
        if eol == "CRLF" and re.search(rb"(?<!\r)\n", blob):
            return f"{path} has Unix line endings, but SVN stores it with Windows ones; convert it before publishing."
        return None

    def settle_interrupted(self) -> None:
        """Recovery runs before every pull, publish and branch copy while a publish is still in doubt, not only at start,
        so a publish whose svn was still running at start is settled as soon as it has finished."""
        if self.journal_path.exists() and not self.publishing:
            journal = json.loads(self.journal_path.read_text(encoding="utf-8"))
            if journal.get("state") != "awaiting-pull":
                self.recover()

    def require_no_journal(self, svn_path: str) -> None:
        """One journal per mirror: while it exists, nothing is written to SVN on any path."""
        if not self.journal_path.exists():
            return
        journal = json.loads(self.journal_path.read_text(encoding="utf-8"))
        if journal.get("state") == "awaiting-pull":
            if journal["path"] != svn_path:
                raise SpikeError(f"SVN has newer revisions on {journal['path']}; pull {journal['path']} first.")
            raise SpikeError(f"SVN has newer revisions on {svn_path}; pull {svn_path}, rebase and publish again.")
        raise SpikeError("A publish was interrupted; UAM settles it before anything else is published.")

    def preflight(self, svn_path: str, branch: str) -> list[str]:
        self.reconcile(svn_path)
        self.require_no_journal(svn_path)
        tip = self.tip(svn_path)
        base = self.t.git("merge-base", tip, branch, git_dir=self.git_dir, check=False).stdout.strip()
        if base != tip:
            raise SpikeError(f"{branch} does not contain the latest pulled SVN revision; rebase it onto the pulled revisions first.")
        commits = self.t.git("rev-list", "--reverse", "--topo-order", f"{tip}..{branch}", git_dir=self.git_dir).stdout.split()
        already = self.published_commits()
        for c in commits:
            if c in already:
                raise SpikeError(f"\"{self.subject(c)}\" is already in SVN; rebase {branch} onto the pulled revisions first.")
            parents = self.t.git("rev-list", "--parents", "-n1", c, git_dir=self.git_dir).stdout.split()[1:]
            if len(parents) != 1:
                raise SpikeError(f"\"{self.subject(c)}\" is a merge; publish a linear branch.")
            changes = self.changed_paths(parents[0], c)
            if not changes:
                raise SpikeError(f"\"{self.subject(c)}\" changes no files, and SVN cannot record an empty commit.")
            for status, path, old_mode, new_mode in changes:
                if "160000" in (old_mode, new_mode):
                    raise SpikeError(f"\"{self.subject(c)}\" adds a Git submodule at {path}, which SVN cannot hold.")
                if any(part == ".svn" for part in path.split("/")):
                    raise SpikeError(f"\"{self.subject(c)}\" writes inside a .svn folder ({path}).")
                if any(ord(ch) < 32 or ord(ch) == 127 for ch in path):
                    raise SpikeError(f"\"{self.subject(c)}\" has a file name with a control character, which SVN cannot store.")
                if status in ("A", "M"):
                    problem = self.normal_form_problem(svn_path, c, path)
                    if problem:
                        raise SpikeError(problem)
        # Git's view of the SVN ignore rules, for paths added with `git add -f`; SVN itself checks again before each commit.
        added = set()
        for c in commits:
            added.update(self.t.git("diff-tree", "-r", "--no-renames", "--diff-filter=A", "--name-only", "-z", f"{c}^", c,
                                    git_dir=self.git_dir).stdout.split("\0"))
        added.discard("")
        if added:
            res = self.t.git("check-ignore", "--no-index", "-z", "--stdin", git_dir=self.git_dir, work_tree=self.bridge(svn_path),
                             index=self.index(svn_path), check=False, input="\0".join(sorted(added)) + "\0")
            ignored = [p for p in res.stdout.split("\0") if p]
            if ignored:
                raise SpikeError(f"{ignored[0]} is ignored by SVN and would not be published; remove it from the branch.")
        # A cheap out-of-date check before anything is written; SVN repeats it per file at commit time.
        state = self.state()
        changed = int(self.t.svn("info", "--show-item", "last-changed-revision", self.url(svn_path)).stdout.strip())
        if changed > state["paths"][svn_path]["last_rev"]:
            raise SpikeError(f"SVN has newer changes on {svn_path}; pull them and rebase before publishing.")
        return commits

    def apply_commit(self, svn_path: str, parent: str, commit: str) -> None:
        """Make the bridge checkout hold the commit's tree and schedule the matching SVN operations."""
        bridge = self.bridge(svn_path)
        changes = self.changed_paths(parent, commit)
        self.g(svn_path, "read-tree", "-m", "-u", parent, commit)
        added = [p for s, p, _, _ in changes if s == "A"]
        new_dirs = sorted({str(Path(p).parents[i]).replace(os.sep, "/") for p in added for i in range(len(Path(p).parents) - 1)},
                          key=lambda d: (d.count("/"), d))
        for d in new_dirs:
            st = self.t.svn("status", "--no-ignore", "--depth", "empty", "--", svn_arg(d), cwd=bridge).stdout
            if st.startswith("I"):
                raise SpikeError(f"{d} is ignored by SVN and would not be published; remove it from the branch.")
            if st.startswith("?"):
                self.t.svn("add", "-q", "--depth", "empty", "--", svn_arg(d), cwd=bridge)
        if added:
            # `svn status` takes no --targets file, so long lists go in chunks to stay under Windows' command-line limit.
            st = "".join(self.t.svn("status", "--no-ignore", "--", *(svn_arg(p) for p in added[i:i + 200]), cwd=bridge).stdout
                         for i in range(0, len(added), 200))
            ignored = [line[8:] for line in st.splitlines() if line.startswith("I")]
            if ignored:
                raise SpikeError(f"{ignored[0]} is ignored by SVN and would not be published; remove it from the branch.")
            self.svn_targets(svn_path, "add", "-q", paths=added)
        deleted = [p for s, p, _, _ in changes if s == "D"]
        if deleted:
            self.svn_targets(svn_path, "rm", "-q", "--force", paths=deleted)
        for status, path, old_mode, new_mode in changes:
            if status == "T":
                raise SpikeError(f"{path} changes between a file and a link, which this spike does not publish.")
            if status in ("A", "M") and old_mode != new_mode and "120000" not in (old_mode, new_mode):
                if new_mode == "100755":
                    self.t.svn("propset", "-q", "svn:executable", "*", "--", svn_arg(path), cwd=bridge)
                elif old_mode == "100755":
                    self.t.svn("propdel", "-q", "svn:executable", "--", svn_arg(path), cwd=bridge)
        # Git removes a folder whose last file went; SVN keeps versioned folders, so delete the ones Git emptied.
        emptied = [line[8:] for line in self.t.svn("status", cwd=bridge).stdout.splitlines()
                   if line.startswith("!") and not (bridge / line[8:]).exists()]
        if emptied:
            self.svn_targets(svn_path, "rm", "-q", "--force", paths=emptied)

    def reset_bridge(self, svn_path: str, parent: str, base_rev: int | None = None) -> None:
        """Return the bridge to the parent's tree. cleanup comes first: a checkout left by an interrupted svn refuses revert."""
        bridge = self.bridge(svn_path)
        self.t.svn("cleanup", "-q", ".", cwd=bridge)
        self.t.svn("revert", "-q", "-R", ".", cwd=bridge)
        self.t.svn("cleanup", "-q", "--remove-unversioned", "--remove-ignored", ".", cwd=bridge)
        if base_rev is not None:
            # A stop after the commit leaves a mixed-revision checkout; return it to the last imported revision.
            self.t.svn("update", "-q", "-r", str(base_rev), ".", cwd=bridge)
        self.contract_keywords(svn_path)
        self.g(svn_path, "read-tree", parent)
        self.g(svn_path, "update-index", "-q", "--refresh", check=False)
        left = [l for l in self.t.svn("status", cwd=bridge).stdout.splitlines() if l.strip()]
        if left:
            raise RuntimeError(f"bridge not clean after reset: {left}")

    def publish(self, svn_path: str, branch: str) -> list[tuple[str, int]]:
        """Publish the branch's unpublished commits to svn_path as SVN revisions, oldest first."""
        self.settle_interrupted()
        self.publishing = True
        try:
            return self._publish(svn_path, branch)
        finally:
            self.publishing = False

    def _publish(self, svn_path: str, branch: str) -> list[tuple[str, int]]:
        commits = self.preflight(svn_path, branch)
        published = []
        for commit in commits:
            parent = self.tip(svn_path)
            message = self.t.git("log", "-1", "--format=%B", commit, git_dir=self.git_dir).stdout.rstrip("\n") + "\n"
            base_rev = self.state()["paths"][svn_path]["last_rev"]
            journal = {"path": svn_path, "branch": branch, "commit": commit, "parent": parent, "base_rev": base_rev, "svn_pid": None}
            self.write_journal(journal)
            try:
                self.apply_commit(svn_path, parent, commit)
            except SpikeError:
                self.reset_bridge(svn_path, parent)
                self.write_journal(None)
                raise
            except Exception as error:  # noqa: BLE001 - raw tool output goes to the log, never to the person
                self.log.append(str(error))
                self.reset_bridge(svn_path, parent)
                self.write_journal(None)
                raise SpikeError(f"Publishing \"{self.subject(commit)}\" failed; the diagnostic log has the reason.")
            if self.before_commit:
                hook, self.before_commit = self.before_commit, None
                hook()
            msg_file = self.home / "commit-message.txt"
            msg_file.write_text(message, encoding="utf-8")
            proc = subprocess.Popen(["svn", "--non-interactive", "--config-dir", str(self.t.config_dir), "--username", "david", "commit",
                                     "-F", str(msg_file), "--encoding", "UTF-8", "--with-revprop", f"{REVPROP_COMMIT}={commit}", "."],
                                    cwd=self.bridge(svn_path), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                    env=dict(os.environ, LC_ALL="C"))
            # Recovery must not touch the bridge while this svn may still be running after a UAM stop.
            journal["svn_pid"] = proc.pid
            self.write_journal(journal)
            stdout, stderr = proc.communicate()
            if proc.returncode != 0:
                self.log.append(stderr.strip())
                self.reset_bridge(svn_path, parent)
                self.write_journal(None)
                if any(code in stderr for code in OUT_OF_DATE_CODES):
                    raise SpikeError(f"SVN has a newer change to a file in \"{self.subject(commit)}\"; pull, rebase and publish again.")
                raise SpikeError(f"SVN refused \"{self.subject(commit)}\"; the diagnostic log has SVN's reason.")
            rev = int(re.search(r"Committed revision (\d+)\.", stdout).group(1))
            if self.crash_after_commit:
                raise KeyboardInterrupt("simulated crash after the SVN commit")
            # David, 2026-11-23: Publish never brings in anyone else's revision. If one landed before this commit,
            # stop and ask for a Pull; the journal keeps the commit's revision so that Pull settles it.
            others = [r for r in self.revisions_on_path(svn_path, base_rev + 1, rev) if r != rev]
            if others:
                journal.update({"state": "awaiting-pull", "rev": rev, "svn_pid": None})  # That svn has finished.
                self.write_journal(journal)
                raise SpikeError(f"\"{self.subject(commit)}\" is in SVN, but SVN also has newer revisions from someone else; "
                                 "pull them, rebase and publish the rest.")
            imported = self.pull(svn_path, up_to=rev)
            if self.crash_after_publish_pull:
                raise KeyboardInterrupt("simulated crash after the publish recorded its revision")
            self.write_journal(None)
            published.append((commit, rev))
            if self.tip(svn_path) != commit:
                raise SpikeError(f"SVN stored \"{self.subject(commit)}\" with different content; rebase onto the pulled revisions and check it.")
        return published

    def recover(self) -> str | None:
        """After a stop, settle an interrupted publish from SVN's own record (the revision property)."""
        if not self.journal_path.exists():
            self.reconcile_all()
            return None
        entry = json.loads(self.journal_path.read_text(encoding="utf-8"))
        pid = entry.get("svn_pid")
        if pid and pid_running(pid):
            raise SpikeError("A previous publish is still running; try again when it has finished.")
        svn_path = entry["path"]
        if entry.get("state") == "awaiting-pull":
            return "awaiting pull"  # The commit landed and nothing is in doubt; the person's Pull settles it.
        # Find the commit's own revision from SVN's record; recovery, like Publish, never imports anyone else's.
        head = int(self.t.svn("info", "--show-item", "revision", self.repo_root_url).stdout.strip())
        candidates = self.revisions_on_path(svn_path, entry["base_rev"] + 1, head)
        own = next((r for r in candidates if self.revision_info(svn_path, r)["props"].get(REVPROP_COMMIT) == entry["commit"]), None)
        self.reset_bridge(svn_path, entry["parent"], base_rev=entry["base_rev"])
        if own is None:
            self.write_journal(None)
            return "not committed"
        if any(r < own for r in candidates):
            entry.update({"state": "awaiting-pull", "rev": own, "svn_pid": None})
            self.write_journal(entry)
            return "awaiting pull"
        self.publishing = True  # Recovery's own pull settles the journal.
        try:
            self.pull(svn_path, up_to=own)
        finally:
            self.publishing = False
        # The stop may have come after the pull had already recorded the revision; the pull above then finds nothing
        # new and leaves the bridge behind, so bring it to what the mirror has recorded.
        last_rev = self.state()["paths"][svn_path]["last_rev"]
        if int(self.t.svn("info", "--show-item", "revision", ".", cwd=self.bridge(svn_path)).stdout.strip()) < last_rev:
            self.reset_bridge(svn_path, self.tip(svn_path), base_rev=last_rev)
        landed = entry["commit"] in self.published_commits()
        self.write_journal(None)
        return "landed" if landed else "not committed"

    def reconcile_all(self) -> None:
        for svn_path in self.state()["paths"]:
            self.reconcile(svn_path)

    def branch_base_revision(self, from_path: str, git_branch: str) -> tuple[int, str]:
        """The SVN revision a Git branch starts from: the mapped revision of its fork point from the upstream ref."""
        fork = self.t.git("merge-base", self.tip(from_path), git_branch, git_dir=self.git_dir).stdout.strip()
        entry = next((e for e in reversed(self.revmap()) if e["path"] == from_path and e["commit"] == fork), None)
        if entry is None:
            raise SpikeError(f"{git_branch} does not start from a pulled SVN revision; rebase it onto the pulled revisions first.")
        return entry["rev"], fork

    def publish_branch(self, from_path: str, new_path: str, git_branch: str, message: str) -> tuple[int, int]:
        """Server-side copy (#520 default: from the revision the branch starts from), then mirror the new path."""
        self.settle_interrupted()
        self.require_no_journal(new_path)
        from_rev, origin = self.branch_base_revision(from_path, git_branch)
        res = self.t.svn("copy", "-m", message, "--with-revprop", f"{REVPROP_BRANCH}={git_branch}",
                         f"{self.url(from_path)}@{from_rev}", self.url(new_path))
        copy_rev = int(re.search(r"Committed revision (\d+)\.", res.stdout).group(1))
        self.attach(new_path, copy_rev, parent_commit=origin, copy_rev=copy_rev)
        return from_rev, copy_rev


def pid_running(pid: int) -> bool:
    """POSIX only, and only a sketch: on Windows os.kill would terminate the process, and a reused id would read as
    still running. A product version records the process start time too and checks both."""
    if os.name == "nt":
        return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


# ---------------------------------------------------------------------------
# Scenarios
# ---------------------------------------------------------------------------

class Run:
    def __init__(self, base: Path):
        self.base = base
        self.t = Tools(base)
        self.results: list[tuple[str, bool, str]] = []
        svn_repo = base / "repo"
        subprocess.run(["svnadmin", "create", str(svn_repo)], check=True)
        self.root = svn_repo.resolve().as_uri()

    def check(self, name: str, ok: bool, detail: str = "") -> None:
        self.results.append((name, ok, detail))
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + (f"  -- {detail}" if detail else ""))

    def other_user_commit(self, rel: str, content: str, message: str, path: str = "trunk") -> int:
        wc = self.base / f"bob-{time.monotonic_ns()}"
        self.t.svn("checkout", "-q", f"{self.root}/{path}", str(wc), user="bob")
        target = wc / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        existed = target.exists()
        target.write_text(content, encoding="utf-8")
        if not existed:
            self.t.svn("add", "-q", "--parents", rel, cwd=wc, user="bob")
        out = self.t.svn("commit", "-m", message, cwd=wc, user="bob").stdout
        shutil.rmtree(wc)
        return int(re.search(r"Committed revision (\d+)\.", out).group(1))

    def export_digest(self, url: str, rev: int) -> dict[str, str]:
        dest = self.base / f"export-{rev}-{time.monotonic_ns()}"
        self.t.svn("export", "-q", "--ignore-keywords", f"{url}@{rev}", str(dest))
        digest = {}
        for p in sorted(dest.rglob("*")):
            if p.is_file():
                digest[p.relative_to(dest).as_posix()] = hashlib.sha256(p.read_bytes()).hexdigest()
        shutil.rmtree(dest)
        return digest

    def git_digest(self, git_dir: Path, commit: str) -> dict[str, str]:
        out = self.t.git("ls-tree", "-r", "-z", commit, git_dir=git_dir).stdout
        digest = {}
        for item in out.split("\0"):
            if not item:
                continue
            meta, path = item.split("\t", 1)
            blob = subprocess.run(["git", "--git-dir", str(git_dir), "cat-file", "blob", meta.split()[2]],
                                  capture_output=True, check=True, env=self.t.git_env).stdout
            digest[path] = hashlib.sha256(blob).hexdigest()
        return digest

    def tree_digest(self, folder: Path) -> str:
        h = hashlib.sha256()
        for p in sorted(folder.rglob("*")):
            if ".git" in p.parts:
                continue
            h.update(p.relative_to(folder).as_posix().encode())
            if p.is_file():
                h.update(p.read_bytes())
        return h.hexdigest()

    def seed(self) -> None:
        t = self.t
        t.svn("mkdir", "-q", "-m", "Standard layout", f"{self.root}/trunk", f"{self.root}/branches", f"{self.root}/tags")
        seed = self.base / "seed"
        t.svn("checkout", "-q", f"{self.root}/trunk", str(seed))
        (seed / "src").mkdir()
        (seed / "src" / "main.c").write_text("int main(void)\n{\n\treturn 0;\n}\n", encoding="utf-8")
        (seed / "src" / "util.c").write_text("int util(void) { return 1; }\n", encoding="utf-8")
        (seed / "README.txt").write_text("Example project\n", encoding="utf-8")
        (seed / "build").mkdir()
        (seed / "build" / "keep.o").write_text("versioned although it matches *.o\n", encoding="utf-8")
        (seed / "docs").mkdir()
        (seed / "docs" / "notes.md").write_text("notes\n", encoding="utf-8")
        (seed / "tools").mkdir()
        (seed / "tools" / "run.sh").write_text("#!/bin/sh\necho run\n", encoding="utf-8")
        (seed / "id.txt").write_text("$Id$\n", encoding="utf-8")
        (seed / "lib[1]").mkdir()
        (seed / "lib[1]" / "keep.txt").write_text("a folder name with glob characters\n", encoding="utf-8")
        t.svn("add", "-q", "--force", ".", cwd=seed)
        t.svn("add", "-q", "--no-ignore", "build/keep.o", cwd=seed, check=False)
        t.svn("propset", "-q", "svn:executable", "*", "tools/run.sh", cwd=seed)
        t.svn("propset", "-q", "svn:keywords", "Id", "id.txt", cwd=seed)
        t.svn("propset", "-q", "svn:ignore", "*.log\nlocal.cfg\n", "src", cwd=seed)
        t.svn("propset", "-q", "svn:global-ignores", "*.tmp\nout\n", ".", cwd=seed)
        t.svn("propset", "-q", "svn:ignore", "*.log\n", "lib[1]@", cwd=seed)
        t.svn("commit", "-q", "-m", "Initial import", cwd=seed)
        shutil.rmtree(seed)

    def go(self) -> int:
        t = self.t
        self.seed()
        # The person's own SVN checkout, with an uncommitted edit and some ignored junk.
        user_wc = self.base / "user-checkout"
        t.svn("checkout", "-q", f"{self.root}/trunk", str(user_wc))
        (user_wc / "README.txt").write_text("Example project\nlocal edit, never committed\n", encoding="utf-8")
        (user_wc / "src" / "debug.log").write_text("junk\n", encoding="utf-8")
        checkout_rev = int(t.svn("info", "--show-item", "revision", str(user_wc)).stdout.strip())
        user_wc_before = self.tree_digest(user_wc)

        mirror = Mirror(t, self.base / "uam-data" / "svn-mirrors" / "example", self.root)

        # S1: create the mirror from the checkout's revision; the Git tree is exactly SVN's versioned set.
        start = mirror.attach("trunk", checkout_rev)
        git_files = self.git_digest(mirror.git_dir, start)
        svn_files = self.export_digest(f"{self.root}/trunk", checkout_rev)
        self.check("S1 mirror tree equals the SVN revision (versioned *.o kept, keywords unexpanded)",
                   git_files.keys() == svn_files.keys() and "build/keep.o" in git_files and "src/debug.log" not in git_files,
                   f"{len(git_files)} files at r{checkout_rev}; Git id.txt = "
                   f"{t.git('cat-file', '-p', f'{start}:id.txt', git_dir=mirror.git_dir).stdout.strip()!r}")
        keyword_note = t.git("cat-file", "-p", f"{start}:id.txt", git_dir=mirror.git_dir).stdout.strip()

        # S2: the translated excludes agree with SVN on probe paths inside versioned folders.
        agent = self.base / "agent-worktree"
        t.git("worktree", "add", "-q", "-b", "chat/feature", str(agent), mirror.ref("trunk"), git_dir=mirror.git_dir)
        probes = ["a.o", "lib.so.1", "src/x.log", "src/local.cfg", "x.log", "local.cfg", "docs/t.tmp", "t.tmp", "src/deep.tmp",
                  "out", "docs/out", "notes.txt", "src/new.c", "#scratch#", "__pycache__", "docs/x.pyc", "Thumbs.db", "thumbs.db",
                  "lib[1]/x.log", "lib[1]/x.txt"]
        probe_wc = self.base / "probe-checkout"
        t.svn("checkout", "-q", f"{self.root}/trunk@{checkout_rev}", str(probe_wc))
        for root in (agent, probe_wc):
            for p in probes:
                (root / p).parent.mkdir(parents=True, exist_ok=True)
                (root / p).write_text("probe\n", encoding="utf-8")
        svn_ignored = {l[8:].replace(os.sep, "/") for l in t.svn("status", "--no-ignore", "--", *(p + "@" for p in probes), cwd=probe_wc).stdout.splitlines()
                       if l.startswith("I")}
        git_ignored = set(t.git("check-ignore", "--no-index", "--stdin", "-z", cwd=agent, check=False,
                                input="\0".join(probes) + "\0").stdout.split("\0")) - {""}
        self.check("S2 Git excludes match SVN ignore rules on probe paths", svn_ignored == git_ignored,
                   f"{len(svn_ignored)} of {len(probes)} ignored by both" if svn_ignored == git_ignored
                   else f"only SVN: {sorted(svn_ignored - git_ignored)}, only Git: {sorted(git_ignored - svn_ignored)}")
        nested = "newdir/deep.o"
        (probe_wc / "newdir").mkdir()
        (probe_wc / nested).write_text("x", encoding="utf-8")
        svn_nested = t.svn("status", "--no-ignore", cwd=probe_wc).stdout
        git_nested = t.git("check-ignore", "--no-index", nested, cwd=agent, check=False).returncode == 0
        self.check("S2b a new folder's ignored file: Git ignores it, SVN status only reports the folder",
                   git_nested and "?       newdir" in svn_nested and "deep.o" not in svn_nested)
        for p in probes:
            (agent / p).unlink()

        # S3: three agent commits published in order with their messages; the mirror keeps the same commit ids.
        def agent_commit(message: str, edit) -> str:
            edit()
            t.git("add", "-A", cwd=agent)
            t.git("commit", "-q", "-m", message, cwd=agent)
            return t.git("rev-parse", "HEAD", cwd=agent).stdout.strip()

        def c1():
            (agent / "src" / "main.c").write_text("int main(void)\n{\n\treturn util();\n}\n", encoding="utf-8")
        def c2():
            (agent / "src" / "lib").mkdir()
            (agent / "src" / "lib" / "helper.c").write_text("void helper(void) {}\n", encoding="utf-8")
            (agent / "src" / "lib" / "helper.sh").write_text("#!/bin/sh\n", encoding="utf-8")
            os.chmod(agent / "src" / "lib" / "helper.sh", 0o755)
            # Names SVN would otherwise read as a peg revision or an option.
            (agent / "src" / "lib" / "icon@2x.png").write_bytes(b"\x89PNG\r\n\x1a\n\x00")
            (agent / "src" / "lib" / "-dash.txt").write_text("dash\n", encoding="utf-8")
        def c3():
            (agent / "docs" / "notes.md").unlink()
            os.chmod(agent / "tools" / "run.sh", 0o644)
        commits = [agent_commit("Call util from main\n\nLonger body line.", c1), agent_commit("Add helper library", c2),
                   agent_commit("Remove notes; run.sh is no longer executable", c3)]
        published = mirror.publish("trunk", "chat/feature")
        revs = [r for _, r in published]
        log = ET.fromstring(t.svn("log", "--xml", "-r", f"{revs[0]}:{revs[-1]}", f"{self.root}/trunk").stdout)
        messages = [(e.findtext("msg") or "").rstrip("\n") for e in log.iter("logentry")]
        expected = [t.git("log", "-1", "--format=%B", c, cwd=agent).stdout.rstrip("\n") for c in commits]
        trees_match = all(self.git_digest(mirror.git_dir, c) == self.export_digest(f"{self.root}/trunk", r) for c, r in published)
        exec_props = t.svn("propget", "svn:executable", f"{self.root}/trunk/src/lib/helper.sh@{revs[1]}", check=False).stdout.strip()
        run_exec = t.svn("propget", "svn:executable", f"{self.root}/trunk/tools/run.sh@{revs[2]}", check=False).returncode
        docs_gone = t.svn("ls", f"{self.root}/trunk/docs@{revs[2]}", check=False).returncode != 0
        self.check("S3 three commits published as consecutive revisions, in order, with their messages",
                   [c for c, _ in published] == commits and revs == list(range(revs[0], revs[0] + 3)) and messages == expected,
                   f"r{revs[0]}-r{revs[-1]}")
        self.check("S3b each revision's tree equals its Git commit's tree", trees_match)
        self.check("S3c executable bit both ways, emptied folder removed", exec_props == "*" and run_exec != 0 and docs_gone,
                   f"helper.sh svn:executable={exec_props!r}, docs/ removed={docs_gone}")
        self.check("S3d upstream ref fast-forwarded to the agent's own commits", mirror.tip("trunk") == commits[-1])

        # S4: an ignored file forced into Git is refused before any SVN revision is written.
        head_before = t.svn("info", "--show-item", "revision", self.root).stdout.strip()
        (agent / "src" / "trace.log").write_text("x\n", encoding="utf-8")
        t.git("add", "-f", "src/trace.log", cwd=agent)
        t.git("commit", "-q", "-m", "Add a log by mistake", cwd=agent)
        try:
            mirror.publish("trunk", "chat/feature")
            self.check("S4 ignored file refused before publishing", False, "publish succeeded")
        except SpikeError as e:
            head_after = t.svn("info", "--show-item", "revision", self.root).stdout.strip()
            self.check("S4 ignored file refused before publishing", head_before == head_after, str(e))
        # The authoritative SVN check, with Git's own view bypassed.
        excludes = (mirror.git_dir / "info" / "exclude").read_text(encoding="utf-8")
        (mirror.git_dir / "info" / "exclude").write_text("", encoding="utf-8")
        try:
            mirror.publish("trunk", "chat/feature")
            self.check("S4b SVN's own ignore check refuses it when Git's excludes are missing", False, "publish succeeded")
        except SpikeError as e:
            head_after = t.svn("info", "--show-item", "revision", self.root).stdout.strip()
            clean = not t.svn("status", cwd=mirror.bridge("trunk")).stdout.strip()
            self.check("S4b SVN's own ignore check refuses it when Git's excludes are missing", head_before == head_after and clean, str(e))
        (mirror.git_dir / "info" / "exclude").write_text(excludes, encoding="utf-8")
        t.git("reset", "-q", "--hard", "HEAD~1", cwd=agent)

        # S5: out of date at commit time (someone changes the same file between the check and the commit).
        def c5a():
            (agent / "README.txt").write_text("Example project\nagent line\n", encoding="utf-8")
        def c5b():
            (agent / "src" / "util.c").write_text("int util(void) { return 2; }\n", encoding="utf-8")
        c5 = [agent_commit("Agent edits README", c5a), agent_commit("Agent changes util", c5b)]
        mirror.before_commit = None
        calls = {"n": 0}
        original_apply = mirror.apply_commit
        def apply_and_race(svn_path, parent, commit):
            original_apply(svn_path, parent, commit)
            calls["n"] += 1
            if calls["n"] == 2:
                self.other_user_commit("src/util.c", "int util(void) { return 3; }\n", "Bob changes util")
        mirror.apply_commit = apply_and_race
        error = ""
        try:
            mirror.publish("trunk", "chat/feature")
        except SpikeError as e:
            error = str(e)
        mirror.apply_commit = original_apply
        first_landed = c5[0] in mirror.published_commits()
        second_landed = c5[1] in mirror.published_commits()
        branch_tip = t.git("rev-parse", "chat/feature", cwd=agent).stdout.strip()
        bridge_clean = not t.svn("status", cwd=mirror.bridge("trunk")).stdout.strip()
        self.check("S5 out-of-date commit refused; earlier commit kept; branch and bridge intact",
                   bool(error) and first_landed and not second_landed and branch_tip == c5[1] and bridge_clean, error)
        pulled = mirror.pull("trunk")
        rebase = t.git("rebase", mirror.ref("trunk"), cwd=agent, check=False)
        conflicted = rebase.returncode != 0 and "CONFLICT" in (rebase.stdout + rebase.stderr)
        (agent / "src" / "util.c").write_text("int util(void) { return 2 + 3; }\n", encoding="utf-8")
        t.git("add", "src/util.c", cwd=agent)
        t.git("-c", "core.editor=true", "rebase", "--continue", cwd=agent)
        again = mirror.publish("trunk", "chat/feature")
        final = t.svn("cat", f"{self.root}/trunk/src/util.c").stdout
        self.check("S5b pull, Git conflict in the agent's worktree, resolve, publish: both changes kept",
                   conflicted and len(again) == 1 and "2 + 3" in final, f"pulled r{pulled[0][0]}, republished r{again[0][1]}")

        # S6: a race on a different file: SVN accepts it; publish stops and asks for a Pull instead of importing Bob's revision.
        def c6a():
            (agent / "src" / "main.c").write_text("int main(void)\n{\n\treturn util() + 1;\n}\n", encoding="utf-8")
        def c6b():
            (agent / "README.txt").write_text("Example project\nagent line\nsecond agent line\n", encoding="utf-8")
        c6 = [agent_commit("Agent changes main", c6a), agent_commit("Agent extends README", c6b)]
        mirror.before_commit = lambda: self.other_user_commit("src/other.c", "int other;\n", "Bob adds other.c")
        error = ""
        try:
            mirror.publish("trunk", "chat/feature")
        except SpikeError as e:
            error = str(e)
        tip_before_pull = mirror.tip("trunk")
        imported_nothing = not [e for e in mirror.revmap() if e.get("published_from") == c6[0]]
        try:
            mirror.publish("trunk", "chat/feature")
            refused = False
        except SpikeError:
            refused = True
        pulled = mirror.pull("trunk")
        merged = [e for e in mirror.revmap() if e.get("published_from") == c6[0]]
        tip_tree_has_both = "src/other.c" in self.git_digest(mirror.git_dir, mirror.tip("trunk"))
        self.check("S6 race on another file: SVN accepts; publish stops without importing anyone else's revision, and refuses until a Pull",
                   bool(error) and imported_nothing and refused and tip_before_pull != mirror.tip("trunk")
                   and len(pulled) == 2 and len(merged) == 1 and merged[0]["commit"] != c6[0] and tip_tree_has_both
                   and c6[1] not in mirror.published_commits() and not mirror.journal_path.exists(), error)
        t.git("rebase", mirror.ref("trunk"), cwd=agent)
        remaining = t.git("rev-list", "--count", f"{mirror.ref('trunk')}..chat/feature", cwd=agent).stdout.strip()
        again = mirror.publish("trunk", "chat/feature")
        self.check("S6b rebase drops the already-published commit; the rest publishes", remaining == "1" and len(again) == 1,
                   f"{remaining} commit left after rebase")

        # S7: crash after the SVN commit but before the mirror recorded it; recovery uses the revision property.
        def c7():
            (agent / "src" / "util.c").write_text("int util(void) { return 7; }\n", encoding="utf-8")
        c7c = agent_commit("Agent sets util to 7", c7)
        mirror.crash_after_commit = True
        try:
            mirror.publish("trunk", "chat/feature")
        except KeyboardInterrupt:
            pass
        mirror.crash_after_commit = False
        restarted = Mirror(t, mirror.home, self.root)
        outcome = restarted.recover()
        head_before = t.svn("info", "--show-item", "revision", self.root).stdout.strip()
        try:
            again = restarted.publish("trunk", "chat/feature")
        except SpikeError as e:
            again = str(e)
        head_after = t.svn("info", "--show-item", "revision", self.root).stdout.strip()
        self.check("S7 crash after commit: recovery maps the revision, nothing published twice",
                   outcome == "landed" and restarted.tip("trunk") == c7c and again == [] and head_before == head_after,
                   f"recovery: {outcome}")
        mirror = restarted

        # S7c: someone else's revision lands before the commit, and UAM stops right after it: recovery maps nothing of theirs.
        def c7r():
            (agent / "src" / "util.c").write_text("int util(void) { return 70; }\n", encoding="utf-8")
        agent_commit("Agent sets util to 70", c7r)
        mirror.before_commit = lambda: self.other_user_commit("src/race.c", "int race;\n", "Bob adds race.c")
        mirror.crash_after_commit = True
        try:
            mirror.publish("trunk", "chat/feature")
        except KeyboardInterrupt:
            pass
        mirror.crash_after_commit = False
        restarted = Mirror(t, mirror.home, self.root)
        tip_before = restarted.tip("trunk")
        outcome = restarted.recover()
        untouched = restarted.tip("trunk") == tip_before
        try:
            restarted.publish("trunk", "chat/feature")
            message = "published"
        except SpikeError as e:
            message = str(e)
        pulled = restarted.pull("trunk")
        self.check("S7c stop after a raced commit: recovery imports nothing, asks for a Pull of trunk, and the Pull settles it",
                   outcome == "awaiting pull" and untouched and "pull trunk" in message and len(pulled) == 2
                   and not restarted.journal_path.exists(), message)
        t.git("rebase", "-q", restarted.ref("trunk"), cwd=agent)
        mirror = restarted

        # S7d: at the next start the interrupted publish's svn is still running; once it ends, the next action settles it.
        def c7d():
            (agent / "src" / "util.c").write_text("int util(void) { return 71; }\n", encoding="utf-8")
        c7d_commit = agent_commit("Agent sets util to 71", c7d)
        mirror.crash_after_commit = True
        try:
            mirror.publish("trunk", "chat/feature")
        except KeyboardInterrupt:
            pass
        mirror.crash_after_commit = False
        stand_in = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])  # Plays the still-running svn.
        journal = json.loads(mirror.journal_path.read_text(encoding="utf-8"))
        journal["svn_pid"] = stand_in.pid
        mirror.write_journal(journal)
        restarted = Mirror(t, mirror.home, self.root)
        try:
            restarted.pull("trunk")
            while_running = "pulled"
        except SpikeError as e:
            while_running = str(e)
        stand_in.kill()
        stand_in.wait()
        settled = restarted.pull("trunk")
        self.check("S7d svn still running at start: actions wait, and the first one after it ends settles the publish",
                   "still running" in while_running and settled == [] and restarted.tip("trunk") == c7d_commit
                   and not restarted.journal_path.exists(), while_running)
        mirror = restarted

        # S7b: a stop after the publish recorded its revision but before the journal was cleared.
        def c7b():
            (agent / "src" / "util.c").write_text("int util(void) { return 77; }\n", encoding="utf-8")
        agent_commit("Agent sets util to 77", c7b)
        mirror.crash_after_publish_pull = True
        try:
            mirror.publish("trunk", "chat/feature")
        except KeyboardInterrupt:
            pass
        restarted = Mirror(t, mirror.home, self.root)
        outcome = restarted.recover()
        bridge_rev = int(t.svn("info", "--show-item", "revision", ".", cwd=restarted.bridge("trunk")).stdout.strip())
        recorded = restarted.state()["paths"]["trunk"]["last_rev"]
        def c7c():
            (agent / "src" / "util.c").write_text("int util(void) { return 78; }\n", encoding="utf-8")
        agent_commit("Agent sets util to 78", c7c)
        again = restarted.publish("trunk", "chat/feature")
        self.check("S7b stop after the revision was recorded: recovery brings the bridge up to date and the next commit publishes",
                   outcome == "landed" and bridge_rev == recorded and len(again) == 1,
                   f"bridge at r{bridge_rev} after recovery, next commit r{again[0][1]}")
        mirror = restarted

        # S8: an explicit pull while a chat has uncommitted work changes only the upstream ref.
        (agent / "src" / "main.c").write_text("/* uncommitted agent edit */\n", encoding="utf-8")
        (agent / "scratch.txt").write_text("untracked\n", encoding="utf-8")
        agent_before = self.tree_digest(agent)
        branch_before = t.git("rev-parse", "chat/feature", cwd=agent).stdout.strip()
        head_rev_before = self.other_user_commit("README.txt", "Example project\nbob was here\n", "Bob edits README")
        upstream_before = mirror.tip("trunk")
        pulled = mirror.pull("trunk")
        head_after_pull = t.svn("info", "--show-item", "revision", self.root).stdout.strip()
        self.check("S8 pull while a chat runs: only the upstream ref moves; worktree, branch and the person's checkout untouched",
                   len(pulled) == 1 and mirror.tip("trunk") != upstream_before and self.tree_digest(agent) == agent_before
                   and t.git("rev-parse", "chat/feature", cwd=agent).stdout.strip() == branch_before
                   and self.tree_digest(user_wc) == user_wc_before and int(head_after_pull) == head_rev_before,
                   f"imported r{pulled[0][0]}; person's checkout still at r{checkout_rev} with its local edit")
        t.git("checkout", "-q", "--", "src/main.c", cwd=agent)
        (agent / "scratch.txt").unlink()

        # S9: publish a new Git branch as an SVN branch (server-side copy from the revision the branch starts from), then its commit.
        # The SVN branch name has a space and dots, which are not valid in a Git ref name as they stand.
        t.git("switch", "-q", "-c", "chat/branchy", mirror.ref("trunk"), cwd=agent)
        def c9():
            (agent / "BRANCH.txt").write_text("branch work\n", encoding="utf-8")
        c9c = agent_commit("Start branch work", c9)
        branch_path = "branches/chat branchy 1.0"
        from_rev, copy_rev = mirror.publish_branch("trunk", branch_path, "chat/branchy", "Create branch chat branchy 1.0 from trunk")
        on_branch = mirror.publish(branch_path, "chat/branchy")
        info = ET.fromstring(t.svn("log", "--xml", "-v", "--stop-on-copy", mirror.url(branch_path)).stdout)
        copyfrom = [(p.get("copyfrom-path"), p.get("copyfrom-rev")) for p in info.iter("path") if p.get("copyfrom-path")]
        ref_ok = t.git("check-ref-format", mirror.ref(branch_path), check=False).returncode == 0
        self.check("S9 Git branch published with svn copy from /trunk at the revision the branch starts from, then its commit",
                   on_branch == [(c9c, copy_rev + 1)] and copyfrom == [("/trunk", str(from_rev))] and ref_ok,
                   f"copy r{copy_rev} from /trunk@{from_rev} (the person's checkout is still at r{checkout_rev}), commit r{copy_rev + 1}, "
                   f"ref {mirror.ref(branch_path)}")
        t.git("switch", "-q", "chat/feature", cwd=agent)

        # S10: rough import cost.
        for i in range(30):
            self.other_user_commit(f"bulk/f{i % 5}.txt", f"line {i}\n", f"Bulk change {i}")
        t0 = time.perf_counter()
        pulled = mirror.pull("trunk")
        elapsed = time.perf_counter() - t0
        self.check("S10 pull of 30 revisions", len(pulled) == 30, f"{elapsed:.2f}s, {elapsed / 30 * 1000:.0f} ms per revision (file://, small tree)")

        # S11: a stop between the revision map line and the ref move; the next pull repairs it without a duplicate commit.
        self.other_user_commit("README.txt", "Example project\ncrash test\n", "Bob edits README again")
        mirror.crash_after_revmap = True
        try:
            mirror.pull("trunk")
        except KeyboardInterrupt:
            pass
        restarted = Mirror(t, mirror.home, self.root)
        again = restarted.pull("trunk")
        trunk_entries = [e for e in restarted.revmap() if e["path"] == "trunk"]
        revs = [e["rev"] for e in trunk_entries]
        self.check("S11 stop during a pull: the ref is repaired from the revision map and nothing is imported twice",
                   again == [] and len(revs) == len(set(revs)) and restarted.tip("trunk") == trunk_entries[-1]["commit"],
                   f"r{revs[-1]} mapped once")
        mirror = restarted

        # S12: an agent moves the upstream ref by hand; publish refuses rather than reporting nothing to publish.
        t.git("rebase", "-q", mirror.ref("trunk"), cwd=agent)
        def c12():
            (agent / "src" / "util.c").write_text("int util(void) { return 12; }\n", encoding="utf-8")
        agent_commit("Agent sets util to 12", c12)
        good_tip = mirror.tip("trunk")
        t.git("update-ref", mirror.ref("trunk"), "HEAD", cwd=agent)
        head_before = t.svn("info", "--show-item", "revision", self.root).stdout.strip()
        try:
            result = mirror.publish("trunk", "chat/feature")
            error = f"published {result}"
        except SpikeError as e:
            error = str(e)
        head_after = t.svn("info", "--show-item", "revision", self.root).stdout.strip()
        self.check("S12 an upstream ref moved outside UAM is refused, not treated as published",
                   "changed outside UAM" in error and head_before == head_after, error)
        t.git("update-ref", mirror.ref("trunk"), good_tip, git_dir=mirror.git_dir)
        self.check("S12b with the ref restored, the commit publishes", len(mirror.publish("trunk", "chat/feature")) == 1)

        # S13: a commit with an expanded keyword is refused up front, so SVN's rewrite is never mistaken for a race.
        def c13():
            (agent / "id.txt").write_text("$Id: id.txt 99 2026-01-01 agent $\n", encoding="utf-8")
        agent_commit("Agent touches id.txt", c13)
        head_before = t.svn("info", "--show-item", "revision", self.root).stdout.strip()
        try:
            mirror.publish("trunk", "chat/feature")
            error = "published"
        except SpikeError as e:
            error = str(e)
        head_after = t.svn("info", "--show-item", "revision", self.root).stdout.strip()
        self.check("S13 an expanded svn:keywords value is refused before publishing", "expanded $Id$" in error and head_before == head_after, error)
        t.git("reset", "-q", "--hard", "HEAD~1", cwd=agent)

        # S14: a file name SVN cannot store is refused before publishing, in words, with the bridge left clean.
        (agent / "tab\tname.txt").write_text("x\n", encoding="utf-8")
        agent_commit("Agent adds a tab name", lambda: None)
        head_before = t.svn("info", "--show-item", "revision", self.root).stdout.strip()
        try:
            mirror.publish("trunk", "chat/feature")
            error = "published"
        except SpikeError as e:
            error = str(e)
        clean = not t.svn("status", cwd=mirror.bridge("trunk")).stdout.strip() and not mirror.journal_path.exists()
        head_after = t.svn("info", "--show-item", "revision", self.root).stdout.strip()
        self.check("S14 a file name with a control character is refused before publishing",
                   "control character" in error and head_before == head_after and clean, error)
        t.git("reset", "-q", "--hard", "HEAD~1", cwd=agent)

        failed = [n for n, ok, _ in self.results if not ok]
        print(f"\nObservation: Git sees svn:keywords unexpanded at import: {keyword_note!r}")
        print(f"\n{len(self.results) - len(failed)} of {len(self.results)} checks passed.")
        return 1 if failed else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", action="store_true", help="keep the temporary folder for inspection")
    args = parser.parse_args()
    for tool in ("svn", "svnadmin", "git"):
        if shutil.which(tool) is None:
            print(f"{tool} is not installed; this spike needs svn, svnadmin and git.")
            return 2
    print(subprocess.run(["svn", "--version", "--quiet"], capture_output=True, text=True).stdout.strip(), "/",
          subprocess.run(["git", "--version"], capture_output=True, text=True).stdout.strip())
    base = Path(tempfile.mkdtemp(prefix="uam-svn-mirror-spike-"))
    try:
        return Run(base).go()
    finally:
        if args.keep:
            print(f"Kept {base}")
        else:
            shutil.rmtree(base, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
