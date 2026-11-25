#!/usr/bin/env python3
"""Conformance checks for a UAM version-control adapter (protocol version 1, issue #522).

Usage:
  python3 conformance.py --workspace DIR [options] -- COMMAND [ARGS...]

COMMAND is the adapter as the person would register it in UAM, with absolute
paths. DIR is a repository of the adapter's kind with at least one changed
file; the adapter must not change it. The runner launches the adapter the way
UAM will (a minimal environment, an empty folder of its own as the working
folder, standard input and output as the only channel), enforces the same time
and size limits, and checks the handshake, every declared capability, that
undeclared ones and all writes are refused, error handling, actions, shutdown
and that nothing in the workspace was written.

It contains a crashing, hanging or flooding adapter itself: every request has
a deadline, every message a size cap, standard error is drained and capped, a
stopped adapter's whole process tree is killed, and the adapter is restarted
for the remaining checks. A malformed result fails the check it arrived in.

Exit code: 0 when no check fails, 1 when any fails, 2 for a usage error.
Protocol: docs/reference/vcs-adapter-protocol.md
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import queue
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
from datetime import datetime
from pathlib import Path
from urllib.parse import urlsplit

PROTOCOL_VERSION = 1
V1_CAPABILITIES = ("status", "diff", "history", "branchList")
# VcsCapabilityId names that exist in UAM but that a version 1 adapter must not declare.
RESERVED_CAPABILITIES = ("commit", "update", "branchCreate", "branchSwitch", "chatComparison", "revisionComparison",
                         "workspaceIsolation", "originComparison")
WRITE_METHODS = {"vcs/commit": {"message": "UAM conformance check", "paths": []}, "vcs/update": {},
                 "vcs/branchCreate": {"name": "uam-conformance-check"}, "vcs/branchSwitch": {"name": "uam-conformance-check"}}
# The methods each version 1 capability adds; an adapter that does not declare it must refuse all of them.
READ_METHODS = {"status": [("vcs/status", {})], "diff": [("vcs/diff", {"path": "uam-conformance/not-changed.txt"})],
                "history": [("vcs/history", {"limit": 5}), ("vcs/revisionDiff", {"revision": "1", "path": "uam-conformance/x.txt"})],
                "branchList": [("vcs/branchList", {})]}
STATUSES = {"added", "modified", "deleted", "renamed", "untracked", "conflicted"}
ACTION_CONTEXT = {"path", "revision"}
ID_PATTERN = re.compile(r"[a-z][a-z0-9-]{1,31}")
METHOD_NOT_FOUND, INVALID_PARAMS, PARSE_ERROR, PATH_NOT_CHANGED = -32601, -32602, -32700, -32002

DEFAULT_TIMEOUT_MS = 10000
DEFAULT_HANDSHAKE_TIMEOUT_MS = 5000
SHUTDOWN_TIMEOUT_S = 2.0
DEFAULT_MAX_MESSAGE_BYTES = 4 * 1024 * 1024
STDERR_KEEP_BYTES = 64 * 1024
MAX_ERROR_MESSAGE = 200
MAX_STATUS_FILES = 10000
MAX_ACTIONS = 8
MAX_RESTARTS = 8
# Only what a program needs to start; no tokens, proxies or provider settings reach an adapter.
ENVIRONMENT_KEEP = ("PATH", "HOME", "USERPROFILE", "LANG", "LC_ALL", "LC_CTYPE", "TMPDIR", "TEMP", "TMP",
                    "SYSTEMROOT", "SystemRoot", "WINDIR", "COMSPEC", "PATHEXT")


class AdapterStopped(Exception):
    """The adapter broke a containment rule and was stopped. `stderr` is the last line it wrote there, for the author."""

    def __init__(self, reason: str, stderr: str = ""):
        super().__init__(reason)
        self.stderr = stderr


def minimal_environment() -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if k in ENVIRONMENT_KEEP}
    if os.name == "nt":
        # Without this, Windows looks for a program the adapter runs by bare name in its current folder first.
        env["NoDefaultCurrentDirectoryInExePath"] = "1"
    return env


def one_line(text, limit: int = 160) -> str:
    text = " ".join(str(text).split())
    return text if len(text) <= limit else text[: limit - 1] + "…"


class Session:
    """One adapter process, used as UAM will: one request at a time, each with a deadline."""

    def __init__(self, command: list[str], start_folder: Path, max_bytes: int):
        self.max_bytes = max_bytes
        self.events: queue.Queue = queue.Queue()
        self.stderr_tail = bytearray()
        self.next_id = 1
        self.error_messages: list[str] = []
        self.dead = False
        # Its own process group or session, so stopping it also stops everything it started.
        group = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {"start_new_session": True}
        try:
            self.proc = subprocess.Popen(command, cwd=start_folder, env=minimal_environment(), stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0, **group)
        except OSError as error:
            self.dead = True
            raise AdapterStopped(f"could not be started ({error.strerror or error}); "
                                 "use absolute paths, because the adapter starts in an empty folder")
        self.stdout_thread = threading.Thread(target=self._read_stdout, daemon=True)
        self.stdout_thread.start()
        self.stderr_thread = threading.Thread(target=self._drain_stderr, daemon=True)
        self.stderr_thread.start()

    def _read_stdout(self) -> None:
        buffer = bytearray()
        while True:
            try:
                chunk = self.proc.stdout.read(65536)
            except (OSError, ValueError):
                chunk = b""
            if not chunk:
                self.events.put(("eof", None))
                return
            buffer += chunk
            while (newline := buffer.find(b"\n")) >= 0:
                line = bytes(buffer[:newline])
                del buffer[: newline + 1]
                if len(line) > self.max_bytes:
                    self.events.put(("oversize", len(line)))
                    return
                self.events.put(("line", line))
            if len(buffer) > self.max_bytes:
                self.events.put(("oversize", len(buffer)))
                return

    def _drain_stderr(self) -> None:
        while True:
            try:
                chunk = self.proc.stderr.read(65536)
            except (OSError, ValueError):
                return
            if not chunk:
                return
            self.stderr_tail += chunk
            del self.stderr_tail[:-STDERR_KEEP_BYTES]

    def kill_tree(self) -> None:
        if os.name == "nt":
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(self.proc.pid)], capture_output=True)
        else:
            try:
                os.killpg(self.proc.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
        if self.proc.poll() is None:
            self.proc.kill()

    def stop(self) -> None:
        self.dead = True
        # The group is killed even after a clean exit, in case the adapter left children behind.
        self.kill_tree()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        # Close the output pipes only after their reader threads have seen the end: closing them earlier frees the
        # handle numbers, the next session's pipes reuse them, and an old thread would read and drop its replies.
        for thread, stream in ((self.stdout_thread, self.proc.stdout), (self.stderr_thread, self.proc.stderr)):
            thread.join(timeout=2)
            if not thread.is_alive():
                try:
                    stream.close()
                except OSError:
                    pass

    def fail(self, reason: str) -> AdapterStopped:
        self.stop()
        lines = [line for line in self.stderr_tail.decode("utf-8", "replace").splitlines() if line.strip()]
        return AdapterStopped(reason, one_line(lines[-1]) if lines else "")

    def send(self, data: bytes, method: str) -> None:
        try:
            self.proc.stdin.write(data)
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError, ValueError):
            code = self.proc.poll()
            raise self.fail(f"exited (code {code}) before {method} could be sent")

    def request(self, method: str, params, timeout_ms: int) -> dict:
        request_id = self.next_id
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            message["params"] = params
        self.send(json.dumps(message, separators=(",", ":")).encode("utf-8") + b"\n", method)
        return self.response(request_id, method, timeout_ms)

    def response(self, request_id, method: str, timeout_ms: int) -> dict:
        deadline = time.monotonic() + timeout_ms / 1000
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise self.fail(f"did not answer {method} within {timeout_ms} ms")
            try:
                kind, value = self.events.get(timeout=remaining)
            except queue.Empty:
                continue
            if kind == "eof":
                try:
                    code = self.proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    code = "unknown"
                raise self.fail(f"exited (code {code}) while answering {method}")
            if kind == "oversize":
                raise self.fail(f"sent a message of more than {self.max_bytes} bytes while answering {method}")
            try:
                reply = json.loads(value.decode("utf-8"))
            except (UnicodeDecodeError, ValueError, RecursionError):
                raise self.fail(f"wrote something other than a JSON-RPC message to standard output while answering {method}: "
                                f"{one_line(value.decode('utf-8', 'replace'), 60)!r}")
            problem = self.reply_problem(reply, request_id)
            if problem:
                raise self.fail(f"{problem} while answering {method}")
            if "error" in reply:
                self.error_messages.append(reply["error"]["message"])
            return reply

    @staticmethod
    def reply_problem(reply, request_id) -> str | None:
        if not isinstance(reply, dict) or reply.get("jsonrpc") != "2.0":
            return "sent a message that is not JSON-RPC 2.0"
        if "method" in reply:
            return "sent a request or notification, which version 1 does not allow from an adapter"
        reply_id = reply.get("id", "missing")
        # `true == 1` in Python, so compare the type as well as the value.
        if type(reply_id) is not type(request_id) or reply_id != request_id:
            return f"answered with id {reply_id!r} instead of {request_id!r}"
        if ("result" in reply) == ("error" in reply):
            return "sent a response without exactly one of result and error"
        if "error" in reply:
            error = reply["error"]
            if not isinstance(error, dict) or type(error.get("code")) is not int or not isinstance(error.get("message"), str):
                return "sent an error without an integer code and a message"
        return None

    def close_input(self) -> None:
        try:
            self.proc.stdin.close()
        except OSError:
            pass


class Report:
    def __init__(self) -> None:
        self.checks: list[dict] = []
        self.notes: list[str] = []

    def add(self, check: str, ok: bool | None, detail: str = "") -> None:
        self.checks.append({"check": check, "result": {True: "pass", False: "fail", None: "skip"}[ok], "detail": detail})
        label = {True: "PASS", False: "FAIL", None: "SKIP"}[ok]
        print(f"{label}  {check}" + (f"  -- {detail}" if detail else ""), flush=True)

    def note(self, text: str) -> None:
        self.notes.append(text)
        print(f"NOTE  {text}", flush=True)

    @property
    def failed(self) -> list[dict]:
        return [c for c in self.checks if c["result"] == "fail"]


def workspace_digest(root: Path, excluded: set[str]) -> str:
    """Names, kinds and contents of everything under the workspace, links not followed, excluded top-level names left out."""
    digest = hashlib.sha256()
    for directory, dirs, files in os.walk(root, followlinks=False):
        if Path(directory) == root:
            dirs[:] = [d for d in dirs if d not in excluded]
            files = [f for f in files if f not in excluded]
        dirs.sort()
        for name in sorted(dirs + files):
            path = Path(directory) / name
            digest.update(path.relative_to(root).as_posix().encode("utf-8", "surrogateescape") + b"\0")
            if path.is_symlink():
                digest.update(b"L" + os.readlink(path).encode("utf-8", "surrogateescape"))
            elif path.is_file():
                digest.update(b"F" + hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def relative_path_problem(path) -> str | None:
    if not isinstance(path, str) or not path:
        return "a path is not a non-empty string"
    if "\\" in path or path.startswith("/") or re.match(r"^[A-Za-z]:", path) or "\0" in path:
        return f"{one_line(path, 60)!r} is not a workspace-relative path with forward slashes"
    if any(part in ("", ".", "..") for part in path.split("/")):
        return f"{one_line(path, 60)!r} is not a normalized workspace-relative path"
    return None


def url_problem(url) -> str | None:
    if not isinstance(url, str) or len(url) > 2048 or any(c.isspace() or ord(c) < 32 for c in url):
        return "the URL is not a single absolute URL"
    parts = urlsplit(url)
    if parts.scheme not in ("http", "https") or not parts.hostname:
        return f"{one_line(url, 60)!r} is not an http or https URL"
    if parts.username or parts.password:
        return "the URL contains a user name or password"
    return None


def file_problem(target, workspace: Path) -> str | None:
    """An openFile target must name an existing item inside the workspace: first by its text alone, then after links."""
    if not isinstance(target, str) or not target or "\0" in target:
        return "the file target is not a path"
    if target.startswith(("\\\\", "//")):
        return f"{one_line(target, 60)!r} is a network path"
    if target != "." and relative_path_problem(target) and not os.path.isabs(target):
        return f"{one_line(target, 60)!r} is not a normalized workspace-relative path"
    candidate = os.path.normpath(target if os.path.isabs(target) else os.path.join(workspace, target))
    # normcase: Windows compares paths without regard to case, so `c:\ws` is inside `C:\ws`.
    inside = lambda path, base: os.path.commonpath([os.path.normcase(path), os.path.normcase(base)]) == os.path.normcase(base)
    try:
        if not inside(candidate, str(workspace)):
            return f"{one_line(target, 60)!r} is outside the workspace"
        if not inside(os.path.realpath(candidate), os.path.realpath(workspace)):
            return f"{one_line(target, 60)!r} leads outside the workspace through a link"
    except ValueError:  # Different drives on Windows.
        return f"{one_line(target, 60)!r} is outside the workspace"
    if not os.path.lexists(candidate):
        return f"{one_line(target, 60)!r} does not exist"
    return None


class Runner:
    def __init__(self, command: list[str], workspace: Path, timeout_ms: int, handshake_timeout_ms: int, max_bytes: int,
                 allow_clean: bool, excluded: set[str]):
        self.command = command
        self.workspace = workspace
        self.timeout_ms = timeout_ms
        self.handshake_timeout_ms = handshake_timeout_ms
        self.max_bytes = max_bytes
        self.allow_clean = allow_clean
        self.excluded = excluded
        self.report = Report()
        self.session: Session | None = None
        self.restarts = 0
        self.init: dict | None = None
        self.status_paths: list[str] = []
        self.history_samples: list[tuple[str, str]] = []
        self.revisions: list[str] = []
        self.error_messages: list[str] = []
        self.start_folder = Path(tempfile.mkdtemp(prefix="uam-adapter-start-"))

    # ----- sessions -----
    def initialize_params(self) -> dict:
        return {"protocolVersion": PROTOCOL_VERSION, "client": {"name": "UAM conformance", "version": "1"},
                "workspace": {"path": str(self.workspace)},
                "limits": {"maxMessageBytes": self.max_bytes, "requestTimeoutMs": self.timeout_ms}}

    def start(self) -> Session:
        session = Session(self.command, self.start_folder, self.max_bytes)
        reply = session.request("initialize", self.initialize_params(), self.handshake_timeout_ms)
        if "error" in reply:
            session.stop()
            raise AdapterStopped(f"refused initialize: {one_line(reply['error']['message'])}")
        return session

    def live(self) -> Session:
        if self.session is not None and not self.session.dead:
            return self.session
        if self.session is not None:
            self.error_messages += self.session.error_messages
        if self.restarts >= MAX_RESTARTS:
            raise AdapterStopped("stopped too often; remaining checks were not run")
        self.restarts += 1
        self.session = self.start()
        return self.session

    def call(self, method: str, params, timeout_ms: int | None = None) -> dict:
        return self.live().request(method, params, timeout_ms or self.timeout_ms)

    def finish_session(self) -> None:
        if self.session is not None:
            self.error_messages += self.session.error_messages
            self.session.stop()
            self.session = None

    def run_check(self, name: str, body) -> None:
        try:
            problems = body()
        except AdapterStopped as stopped:
            self.report.add(name, False, f"the adapter {stopped}")
            if stopped.stderr:
                self.report.note(f"its last standard error line was: {stopped.stderr}")
            return
        except (TypeError, KeyError, AttributeError, ValueError, IndexError, RecursionError) as error:
            # A result of the wrong shape must fail the check, not stop the runner.
            self.report.add(name, False, f"the adapter sent a result the runner could not read ({type(error).__name__})")
            return
        if problems is None:
            self.report.add(name, None, "nothing to check")
        else:
            self.report.add(name, not problems, "; ".join(problems[:3]))

    # ----- checks -----
    def check_handshake(self) -> list[str]:
        session = Session(self.command, self.start_folder, self.max_bytes)
        self.session = session
        started = time.monotonic()
        reply = session.request("initialize", self.initialize_params(), self.handshake_timeout_ms)
        elapsed = (time.monotonic() - started) * 1000
        if "error" in reply:
            session.stop()
            return [f"initialize was refused: {one_line(reply['error']['message'])}"]
        result = reply["result"]
        problems = []
        if not isinstance(result, dict):
            session.stop()
            return ["initialize result is not an object"]
        if result.get("protocolVersion") != PROTOCOL_VERSION or type(result.get("protocolVersion")) is not int:
            problems.append(f"protocolVersion is {result.get('protocolVersion')!r}, expected 1")
        adapter = result.get("adapter")
        if not isinstance(adapter, dict):
            problems.append("adapter is missing")
            adapter = {}
        else:
            if not isinstance(adapter.get("id"), str) or not ID_PATTERN.fullmatch(adapter["id"]) or adapter["id"] in ("git", "svn"):
                problems.append(f"adapter.id {adapter.get('id')!r} must be 2-32 lowercase letters, digits or hyphens and not git or svn")
            name = adapter.get("name")
            if not isinstance(name, str) or not 1 <= len(name) <= 40 or any(c in name for c in "\r\n"):
                problems.append("adapter.name must be one line of 1-40 characters")
            if not isinstance(adapter.get("version"), str) or not adapter["version"]:
                problems.append("adapter.version must be a string")
        capabilities = result.get("capabilities")
        if not isinstance(capabilities, list) or not all(isinstance(c, str) for c in capabilities):
            problems.append("capabilities must be a list of names")
            capabilities = []
        if len(set(capabilities)) != len(capabilities):
            problems.append("capabilities lists a name twice")
        for capability in capabilities:
            if capability in RESERVED_CAPABILITIES:
                problems.append(f"declares {capability}, which version 1 does not allow")
            elif capability not in V1_CAPABILITIES:
                problems.append(f"declares unknown capability {capability!r}")
        actions = result.get("actions", [])
        if not isinstance(actions, list):
            problems.append("actions must be a list")
        elif len(actions) > MAX_ACTIONS:
            problems.append(f"declares {len(actions)} actions; at most {MAX_ACTIONS} are allowed")
        self.init = result if not problems else None
        if self.init is None:
            session.stop()
        else:
            self.report.note(f"{adapter.get('name')} {adapter.get('version')} declares {', '.join(capabilities) or 'no capabilities'}; "
                             f"initialize took {elapsed:.0f} ms")
        return problems

    def declared(self) -> list[str]:
        return list(self.init.get("capabilities", [])) if self.init else []

    def check_detect(self) -> list[str]:
        problems = []
        reply = self.call("vcs/detect", {"path": str(self.workspace)})
        result = reply.get("result")
        if "error" in reply or not isinstance(result, dict) or result.get("repository") is not True:
            problems.append("does not detect the workspace as a repository")
        else:
            root = result.get("root")
            if not isinstance(root, str) or not os.path.isabs(root):
                problems.append("root must be an absolute path")
            else:
                try:
                    contains = os.path.commonpath([os.path.realpath(root), os.path.realpath(self.workspace)]) == os.path.realpath(root)
                except ValueError:
                    contains = False
                if not contains:
                    problems.append("root does not contain the workspace")
        with tempfile.TemporaryDirectory(prefix="uam-adapter-empty-") as empty:
            reply = self.call("vcs/detect", {"path": empty})
            result = reply.get("result")
            if "error" in reply or not isinstance(result, dict) or result.get("repository") is not False:
                problems.append("claims an empty folder outside the workspace is a repository, or fails on it")
        return problems

    def check_status(self) -> list[str]:
        problems = []
        for include_stats in (False, True):
            reply = self.call("vcs/status", {"includeLineStats": include_stats})
            if "error" in reply:
                return [f"vcs/status failed: {one_line(reply['error']['message'])}"]
            result = reply["result"]
            if not isinstance(result, dict) or not isinstance(result.get("files"), list):
                return ["vcs/status result has no files list"]
            if not isinstance(result.get("branchOrRevision"), str):
                problems.append("branchOrRevision must be a string")
            if not isinstance(result.get("truncated", False), bool):
                problems.append("truncated must be true or false")
            if len(result["files"]) > MAX_STATUS_FILES:
                problems.append(f"lists {len(result['files'])} files; at most {MAX_STATUS_FILES}, then truncated")
            seen = set()
            for entry in result["files"]:
                if not isinstance(entry, dict):
                    problems.append("a files entry is not an object")
                    continue
                problem = relative_path_problem(entry.get("path"))
                if problem:
                    problems.append(problem)
                    continue
                if entry["path"] in seen:
                    problems.append(f"{entry['path']} is listed twice")
                seen.add(entry["path"])
                if entry.get("status") not in STATUSES:
                    problems.append(f"{entry['path']} has unknown status {one_line(entry.get('status'), 30)!r}")
                if (entry.get("status") == "renamed") != ("previousPath" in entry):
                    problems.append(f"{entry['path']}: previousPath is required for, and only for, renamed files")
                elif "previousPath" in entry and relative_path_problem(entry["previousPath"]):
                    problems.append(f"{entry['path']}: previousPath {relative_path_problem(entry['previousPath'])}")
                for key in ("additions", "deletions"):
                    if key in entry and (type(entry[key]) is not int or entry[key] < 0):
                        problems.append(f"{entry['path']}: {key} must be a non-negative integer")
                if "binary" in entry and not isinstance(entry["binary"], bool):
                    problems.append(f"{entry['path']}: binary must be true or false")
            if include_stats and sorted(seen) != sorted(self.status_paths):
                problems.append("the file list changes when line counts are requested")
            if not include_stats:
                self.status_paths = sorted(seen)
        if not self.status_paths:
            message = "vcs/status listed no changed files; run against a workspace with changes"
            if self.allow_clean:
                self.report.note(message + " for a stronger check")
            else:
                problems.append(message + ", or pass --allow-clean")
        return problems

    def check_diff(self) -> list[str]:
        problems = []
        if "status" not in self.declared():
            self.report.note("diff is declared without status, so only the refusal for an unchanged file was checked")
        for path in self.status_paths[:10]:
            reply = self.call("vcs/diff", {"path": path})
            if "error" in reply:
                problems.append(f"vcs/diff failed for {path}: {one_line(reply['error']['message'])}")
                continue
            problem = self.patch_problem(reply["result"], f"vcs/diff for {path}")
            if problem:
                problems.append(problem)
        reply = self.call("vcs/diff", {"path": "uam-conformance/not-changed.txt"})
        if "error" not in reply or reply["error"]["code"] != PATH_NOT_CHANGED:
            problems.append(f"vcs/diff for a file with no changes must fail with code {PATH_NOT_CHANGED}")
        return problems

    @staticmethod
    def patch_problem(result, label: str) -> str | None:
        if not isinstance(result, dict) or not isinstance(result.get("patch"), str):
            return f"{label} has no patch text"
        for key in ("binary", "tooLarge"):
            if not isinstance(result.get(key, False), bool):
                return f"{label}: {key} must be true or false"
        if not result["patch"] and not result.get("binary") and not result.get("tooLarge"):
            return f"{label} is empty"
        return None

    def check_history(self) -> list[str]:
        problems = []
        reply = self.call("vcs/history", {"limit": 5})
        if "error" in reply:
            return [f"vcs/history failed: {one_line(reply['error']['message'])}"]
        pages = [reply["result"]]
        if isinstance(pages[0], dict) and isinstance(pages[0].get("next"), str) and pages[0]["next"]:
            reply = self.call("vcs/history", {"limit": 5, "before": pages[0]["next"]})
            if "error" in reply:
                problems.append(f"the next history page failed: {one_line(reply['error']['message'])}")
            else:
                pages.append(reply["result"])
        seen = set()
        for page in pages:
            if not isinstance(page, dict) or not isinstance(page.get("entries"), list) or not isinstance(page.get("next", ""), str):
                problems.append("a history page needs an entries list and a next cursor string")
                continue
            if len(page["entries"]) > 5:
                problems.append("a history page returned more entries than the limit")
            for entry in page["entries"]:
                if not isinstance(entry, dict) or not isinstance(entry.get("revision"), str) or not entry["revision"]:
                    problems.append("a history entry has no revision")
                    continue
                revision = entry["revision"]
                if revision in seen:
                    problems.append(f"revision {revision} appears on two pages")
                seen.add(revision)
                for key in ("author", "message"):
                    if not isinstance(entry.get(key), str):
                        problems.append(f"revision {revision}: {key} must be a string")
                try:
                    datetime.fromisoformat(str(entry.get("date", "")).replace("Z", "+00:00"))
                except ValueError:
                    problems.append(f"revision {revision}: date must be ISO 8601")
                changed = entry.get("paths", [])
                if not isinstance(changed, list):
                    problems.append(f"revision {revision}: paths must be a list")
                    continue
                for item in changed:
                    if not isinstance(item, dict) or not isinstance(item.get("action"), str):
                        problems.append(f"revision {revision}: each changed path needs action and path")
                        continue
                    problem = relative_path_problem(item.get("path"))
                    if problem:
                        problems.append(f"revision {revision}: {problem}")
                    elif item["action"] != "D" and len(self.history_samples) < 3:
                        self.history_samples.append((revision, item["path"]))
        self.revisions = sorted(seen)
        # The patch one revision applied to one path, which UAM's History capability includes.
        for revision, path in self.history_samples:
            reply = self.call("vcs/revisionDiff", {"revision": revision, "path": path})
            if "error" in reply:
                problems.append(f"vcs/revisionDiff failed for {path} at {revision}: {one_line(reply['error']['message'])}")
                continue
            problem = self.patch_problem(reply["result"], f"vcs/revisionDiff for {path} at {revision}")
            if problem:
                problems.append(problem)
        if not self.history_samples:
            self.report.note("no history entry listed a changed file, so vcs/revisionDiff was not checked")
        return problems

    def check_branch_list(self) -> list[str]:
        reply = self.call("vcs/branchList", {})
        if "error" in reply:
            return [f"vcs/branchList failed: {one_line(reply['error']['message'])}"]
        result = reply["result"]
        if not isinstance(result, dict) or not isinstance(result.get("branches"), list):
            return ["vcs/branchList result has no branches list"]
        problems = []
        branches = result["branches"]
        names = [b.get("name") if isinstance(b, dict) else None for b in branches]
        if not all(isinstance(n, str) and n for n in names):
            return ["every branch needs a name"]
        if len(set(names)) != len(names):
            problems.append("a branch name is listed twice")
        if sum(1 for b in branches if b.get("current") is True) > 1:
            problems.append("more than one branch is current")
        return problems

    def check_refused(self, methods: list[tuple[str, dict]]) -> list[str]:
        problems = []
        for method, params in methods:
            reply = self.call(method, params)
            if "error" not in reply:
                problems.append(f"answered {method} although it does not declare it")
            elif reply["error"]["code"] != METHOD_NOT_FOUND:
                problems.append(f"refused {method} with code {reply['error']['code']} instead of {METHOD_NOT_FOUND}")
        return problems

    def check_actions(self) -> list[str] | None:
        actions = self.init.get("actions", []) if self.init else []
        if not actions:
            return None
        problems = []
        ids = set()
        for action in actions:
            if not isinstance(action, dict):
                problems.append("an action is not an object")
                continue
            action_id = action.get("id")
            if not isinstance(action_id, str) or not ID_PATTERN.fullmatch(action_id) or action_id in ids:
                problems.append(f"action id {action_id!r} is missing, invalid or repeated")
                continue
            ids.add(action_id)
            label = action.get("label")
            if not isinstance(label, str) or not 1 <= len(label) <= 32 or any(c in label for c in "\r\n"):
                problems.append(f"{action_id}: label must be one line of 1-32 characters")
            kind = action.get("kind")
            if kind not in ("openUrl", "openFile"):
                problems.append(f"{action_id}: kind {one_line(kind, 30)!r} is not openUrl or openFile")
                continue
            has_target, context = "target" in action, action.get("context")
            if has_target == (context is not None):
                problems.append(f"{action_id}: give either a target or a context, not both or neither")
                continue
            if has_target:
                problem = url_problem(action["target"]) if kind == "openUrl" else file_problem(action["target"], self.workspace)
                if problem:
                    problems.append(f"{action_id}: {problem}")
                continue
            if not isinstance(context, list) or not context or not all(isinstance(c, str) for c in context) or not set(context) <= ACTION_CONTEXT:
                problems.append(f"{action_id}: context must list path and/or revision")
                continue
            if "path" in context and not self.status_paths:
                message = f"{action_id} could not be checked: there is no changed file to pass as its path"
                if self.allow_clean:
                    self.report.note(message)
                else:
                    problems.append(message)
                continue
            if "revision" in context and not self.revisions:
                problems.append(f"{action_id} could not be checked: there is no revision to pass")
                continue
            # An action may not apply to every file (a baseline copy of an added file does not exist), so try a few.
            resolved = False
            for path in (self.status_paths[:5] if "path" in context else [None]):
                values = {"path": path} if path is not None else {}
                if "revision" in context:
                    values["revision"] = self.revisions[0]
                reply = self.call("actions/resolve", {"actionId": action_id, "context": values})
                if "error" in reply:
                    continue
                resolved = True
                result = reply["result"]
                if not isinstance(result, dict) or result.get("kind") != kind:
                    problems.append(f"{action_id}: resolved to a different kind than it declared")
                    break
                problem = url_problem(result.get("target")) if kind == "openUrl" else file_problem(result.get("target"), self.workspace)
                if problem:
                    problems.append(f"{action_id}: {problem}")
                break
            if not resolved:
                problems.append(f"{action_id} did not resolve for any of the files or revisions tried")
            # Paths UAM passes come from the adapter's own status, but an adapter must still refuse ones that leave the workspace.
            if "path" in context:
                outside = str(Path(tempfile.gettempdir()) / "uam-conformance-outside.txt")
                for hostile in ("../uam-conformance-outside.txt", outside):
                    values = {"path": hostile}
                    if "revision" in context:
                        values["revision"] = self.revisions[0]
                    reply = self.call("actions/resolve", {"actionId": action_id, "context": values})
                    if "error" not in reply:
                        problems.append(f"{action_id} resolved the path {one_line(hostile, 60)!r} instead of refusing it")
        reply = self.call("actions/resolve", {"actionId": "uam-conformance-unknown", "context": {}})
        if "error" not in reply:
            problems.append("resolved an action it never declared")
        return problems

    def check_errors(self) -> list[str]:
        problems = []
        reply = self.call("uam/conformanceUnknown", {})
        if "error" not in reply or reply["error"]["code"] != METHOD_NOT_FOUND:
            problems.append(f"an unknown method must fail with code {METHOD_NOT_FOUND}")
        reply = self.call("vcs/detect", {"path": 42})
        if "error" not in reply or reply["error"]["code"] != INVALID_PARAMS:
            problems.append(f"invalid params must fail with code {INVALID_PARAMS}")
        session = self.live()
        session.send(b"{this is not json\n", "a malformed line")
        reply = session.response(None, "a malformed line", self.timeout_ms)
        if "error" not in reply or reply["error"]["code"] != PARSE_ERROR:
            problems.append(f"a malformed line must be answered with code {PARSE_ERROR} and id null")
        reply = self.call("vcs/detect", {"path": str(self.workspace)})
        if "error" in reply:
            problems.append("stopped working after a malformed line")
        return problems

    def check_error_messages(self) -> list[str]:
        self.finish_session()
        problems = []
        for message in self.error_messages:
            if not message.strip() or "\n" in message or "\r" in message or len(message) > MAX_ERROR_MESSAGE:
                problems.append(f"error message {one_line(message, 60)!r} must be one plain line of at most {MAX_ERROR_MESSAGE} characters")
            elif "://" in message or re.search(r"\b(?:\d{1,3}\.){3}\d{1,3}\b", message):
                # URLs and IP addresses are caught here; a bare host name cannot be told from other dotted text.
                problems.append(f"error message {one_line(message, 60)!r} must not contain an address")
        return problems[:3]

    def check_shutdown(self) -> list[str]:
        problems = []
        session = self.start()
        reply = session.request("shutdown", None, self.timeout_ms)
        if "error" in reply:
            problems.append("refused shutdown")
        session.close_input()
        try:
            code = session.proc.wait(timeout=SHUTDOWN_TIMEOUT_S)
            if code != 0:
                problems.append(f"exited with code {code} after shutdown")
        except subprocess.TimeoutExpired:
            problems.append(f"did not exit within {SHUTDOWN_TIMEOUT_S:.0f} s of shutdown")
        session.stop()
        session = self.start()
        session.close_input()
        try:
            session.proc.wait(timeout=SHUTDOWN_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            problems.append(f"did not exit within {SHUTDOWN_TIMEOUT_S:.0f} s of its input closing")
        session.stop()
        return problems

    def run(self) -> Report:
        before = workspace_digest(self.workspace, self.excluded)
        try:
            self.run_checks()
        finally:
            self.finish_session()
            try:
                self.start_folder.rmdir()
            except OSError:
                self.report.note("the adapter left files in its start folder")
        after = workspace_digest(self.workspace, self.excluded)
        self.report.add("read-only", after == before, "" if after == before else "the workspace changed during the run")
        return self.report

    def run_checks(self) -> None:
        self.run_check("handshake", self.check_handshake)
        if self.init is None:
            return
        self.run_check("detect", self.check_detect)
        declared = self.declared()
        bodies = {"status": self.check_status, "diff": self.check_diff, "history": self.check_history, "branchList": self.check_branch_list}
        for capability in V1_CAPABILITIES:
            if capability in declared:
                self.run_check(f"capability:{capability}", bodies[capability])
        for capability in V1_CAPABILITIES:
            if capability not in declared:
                self.run_check(f"refuses:{capability}", lambda m=READ_METHODS[capability]: self.check_refused(m))
        for method, params in WRITE_METHODS.items():
            self.run_check(f"refuses:{method}", lambda m=method, p=params: self.check_refused([(m, p)]))
        self.run_check("actions", self.check_actions)
        self.run_check("errors", self.check_errors)
        self.run_check("error-messages", self.check_error_messages)
        self.run_check("shutdown", self.check_shutdown)


def main(argv: list[str]) -> int:
    # Adapter text can hold any character; a Windows pipe would otherwise use its code page and fail to print it.
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    if "--" not in argv:
        print(__doc__)
        return 2
    split = argv.index("--")
    parser = argparse.ArgumentParser(description="Check a UAM VCS adapter against protocol version 1.")
    parser.add_argument("--workspace", required=True, type=Path, help="a repository of the adapter's kind with changes")
    parser.add_argument("--timeout-ms", type=int, default=DEFAULT_TIMEOUT_MS)
    parser.add_argument("--handshake-timeout-ms", type=int, default=DEFAULT_HANDSHAKE_TIMEOUT_MS)
    parser.add_argument("--max-message-bytes", type=int, default=DEFAULT_MAX_MESSAGE_BYTES)
    parser.add_argument("--allow-clean", action="store_true", help="accept a workspace with no changed files (weaker checks)")
    parser.add_argument("--exclude", action="append", default=[], metavar="NAME",
                        help="a top-level metadata folder the system itself may rewrite during reads (for example .git); repeatable")
    parser.add_argument("--json", type=Path, help="also write the report as JSON")
    args = parser.parse_args(argv[:split])
    command = argv[split + 1:]
    if not command:
        parser.error("give the adapter command after --")
    workspace = args.workspace.resolve()
    if not workspace.is_dir():
        parser.error(f"{workspace} is not a folder")
    started = time.monotonic()
    report = Runner(command, workspace, args.timeout_ms, args.handshake_timeout_ms, args.max_message_bytes,
                    args.allow_clean, set(args.exclude)).run()
    passed = sum(1 for c in report.checks if c["result"] == "pass")
    print(f"\n{passed} of {len(report.checks)} checks passed, {len(report.failed)} failed in {time.monotonic() - started:.1f} s.")
    if args.json:
        args.json.write_text(json.dumps({"checks": report.checks, "notes": report.notes}, indent=1), encoding="utf-8")
    return 0 if not report.failed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
