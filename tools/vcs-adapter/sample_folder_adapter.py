#!/usr/bin/env python3
"""Sample read-only VCS adapter for UAM's adapter protocol, version 1 (issue #522).

It treats a plain folder as a repository: the folder holds a baseline copy of
itself in `.foldervcs/base/`, and the adapter reports how the folder differs
from that baseline. It declares `status` and `diff`, refuses everything else,
and offers two actions that only open files. It never writes anything.

Protocol: docs/reference/vcs-adapter-protocol.md
Check it: python3 tools/vcs-adapter/conformance.py --workspace DIR -- python3 tools/vcs-adapter/sample_folder_adapter.py
Make a baseline for a folder (outside the protocol): python3 sample_folder_adapter.py --snapshot DIR
"""
from __future__ import annotations

import difflib
import stat
import json
import os
import shutil
import sys
from pathlib import Path

PROTOCOL_VERSION = 1
MARKER = ".foldervcs"
MAX_FILES = 10000
DEFAULT_MAX_MESSAGE_BYTES = 4 * 1024 * 1024
# Room for the JSON envelope around a patch or file list.
REPLY_MARGIN = 4096

# JSON-RPC and protocol error codes.
PARSE_ERROR = -32700
INVALID_REQUEST = -32600
METHOD_NOT_FOUND = -32601
INVALID_PARAMS = -32602
INTERNAL_ERROR = -32603
NOT_A_REPOSITORY = -32001
PATH_NOT_CHANGED = -32002


class RpcError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code
        self.message = message


def find_root(path: Path) -> Path | None:
    for candidate in (path, *path.parents):
        if (candidate / MARKER / "base").is_dir():
            return candidate
    return None


def relative_path_problem(path) -> bool:
    """True unless path is a normalized repository-relative path with forward slashes."""
    if not isinstance(path, str) or not path or "\\" in path or path.startswith("/") or "\0" in path or (len(path) > 1 and path[1] == ":"):
        return True
    return any(part in ("", ".", "..") for part in path.split("/"))


def is_binary(data: bytes) -> bool:
    return b"\0" in data[:8192]


def read_item(path: Path) -> bytes:
    # A link is compared by its target text and never followed, so the adapter cannot be led outside the folder.
    if path.is_symlink():
        return os.readlink(path).encode("utf-8", "surrogateescape")
    return path.read_bytes()


def inventory(root: Path) -> dict[str, Path]:
    items: dict[str, Path] = {}
    for directory, dirs, files in os.walk(root, followlinks=False):
        here = Path(directory)
        # A link to a folder is listed as an item of its own and never walked into.
        links = [d for d in dirs if (here / d).is_symlink()]
        dirs[:] = sorted(d for d in dirs if d != MARKER and d not in links)
        for name in sorted(files + links):
            full = here / name
            # Only regular files and links: reading a named pipe or device would block or never end.
            if not full.is_symlink() and not stat.S_ISREG(full.lstat().st_mode):
                continue
            items[full.relative_to(root).as_posix()] = full
    return items


class FolderAdapter:
    capabilities = ["status", "diff"]

    def __init__(self) -> None:
        self.workspace: Path | None = None
        self.root: Path | None = None
        self.initialized = False
        self.max_bytes = DEFAULT_MAX_MESSAGE_BYTES

    # ----- dispatch -----
    def methods(self) -> dict:
        table = {"initialize": self.initialize, "shutdown": self.shutdown, "vcs/detect": self.detect,
                 "actions/resolve": self.resolve_action}
        if "status" in self.capabilities:
            table["vcs/status"] = self.status
        if "diff" in self.capabilities:
            table["vcs/diff"] = self.diff
        return table

    def handle(self, message: dict):
        method = message.get("method")
        handler = self.methods().get(method)
        if handler is None:
            raise RpcError(METHOD_NOT_FOUND, f"{method} is not supported by this adapter.")
        if method != "initialize" and not self.initialized:
            raise RpcError(INVALID_REQUEST, "initialize must be the first request.")
        params = message.get("params", {})
        if not isinstance(params, dict):
            raise RpcError(INVALID_PARAMS, "params must be an object.")
        return handler(params)

    # ----- lifecycle -----
    def initialize(self, params: dict) -> dict:
        if params.get("protocolVersion") != PROTOCOL_VERSION:
            raise RpcError(INVALID_PARAMS, "Only protocol version 1 is supported.")
        workspace = params.get("workspace", {}).get("path") if isinstance(params.get("workspace"), dict) else None
        if not isinstance(workspace, str) or not os.path.isabs(workspace):
            raise RpcError(INVALID_PARAMS, "workspace.path must be an absolute path.")
        self.workspace = Path(workspace)
        self.root = find_root(self.workspace)
        limits = params.get("limits") if isinstance(params.get("limits"), dict) else {}
        if type(limits.get("maxMessageBytes")) is int and limits["maxMessageBytes"] > REPLY_MARGIN * 4:
            self.max_bytes = limits["maxMessageBytes"]
        self.initialized = True
        return {
            "protocolVersion": PROTOCOL_VERSION,
            "adapter": {"id": "folder-sample", "name": "Folder (sample)", "version": "1.0.0"},
            "capabilities": list(self.capabilities),
            "actions": [
                {"id": "show-folder", "label": "Show folder", "kind": "openFile", "target": "."},
                {"id": "show-baseline", "label": "Show baseline copy", "kind": "openFile", "context": ["path"]},
            ],
        }

    def shutdown(self, params: dict):
        return None

    # ----- operations -----
    def detect(self, params: dict) -> dict:
        path = params.get("path")
        if not isinstance(path, str) or not os.path.isabs(path):
            raise RpcError(INVALID_PARAMS, "path must be an absolute path.")
        root = find_root(Path(path))
        return {"repository": root is not None, **({"root": str(root)} if root else {})}

    def require_root(self) -> Path:
        if self.root is None:
            raise RpcError(NOT_A_REPOSITORY, "This folder has no baseline copy.")
        return self.root

    def changes(self) -> list[dict]:
        root = self.require_root()
        current = inventory(root)
        base = inventory(root / MARKER / "base")
        files = []
        for rel in sorted(set(current) | set(base)):
            if rel not in base:
                files.append({"path": rel, "status": "added"})
            elif rel not in current:
                files.append({"path": rel, "status": "deleted"})
            elif current[rel].is_symlink() != base[rel].is_symlink() or read_item(current[rel]) != read_item(base[rel]):
                files.append({"path": rel, "status": "modified"})
        return files

    def status(self, params: dict) -> dict:
        include_stats = params.get("includeLineStats", False)
        if not isinstance(include_stats, bool):
            raise RpcError(INVALID_PARAMS, "includeLineStats must be true or false.")
        files = self.changes()
        truncated = len(files) > MAX_FILES
        files = files[:MAX_FILES]
        # Keep the reply within the message limit UAM sent; a longer list is cut and marked truncated.
        while files and len(json.dumps(files)) > self.max_bytes - REPLY_MARGIN:
            files, truncated = files[: len(files) // 2], True
        if include_stats:
            for entry in files:
                old, new = self.contents(entry)
                if old is None and new is None:
                    continue
                if is_binary(old or b"") or is_binary(new or b""):
                    entry["binary"] = True
                    continue
                added = deleted = 0
                for line in difflib.unified_diff(self.lines(old), self.lines(new), lineterm="", n=0):
                    if line.startswith("+") and not line.startswith("+++"):
                        added += 1
                    elif line.startswith("-") and not line.startswith("---"):
                        deleted += 1
                entry["additions"], entry["deletions"] = added, deleted
        return {"branchOrRevision": "baseline", "files": files, "truncated": truncated}

    def contents(self, entry: dict) -> tuple[bytes | None, bytes | None]:
        root = self.require_root()
        current, base = root / entry["path"], root / MARKER / "base" / entry["path"]
        too_big = any(p.exists() and not p.is_symlink() and p.stat().st_size > self.max_bytes for p in (current, base))
        if too_big:
            return None, None
        old = read_item(base) if entry["status"] != "added" else b""
        new = read_item(current) if entry["status"] != "deleted" else b""
        return old, new

    @staticmethod
    def lines(data: bytes | None) -> list[str]:
        return (data or b"").decode("utf-8", "replace").splitlines()

    def diff(self, params: dict) -> dict:
        path = params.get("path")
        if not isinstance(path, str) or not path:
            raise RpcError(INVALID_PARAMS, "path must be a workspace-relative path.")
        entry = next((f for f in self.changes() if f["path"] == path), None)
        if entry is None:
            raise RpcError(PATH_NOT_CHANGED, "That file has no changes.")
        old, new = self.contents(entry)
        if old is None and new is None:
            return {"patch": "", "binary": False, "tooLarge": True}
        if is_binary(old) or is_binary(new):
            return {"patch": "", "binary": True}
        a = "/dev/null" if entry["status"] == "added" else f"a/{path}"
        b = "/dev/null" if entry["status"] == "deleted" else f"b/{path}"
        patch = "\n".join(difflib.unified_diff(self.lines(old), self.lines(new), fromfile=a, tofile=b, lineterm=""))
        patch += "\n" if patch else ""
        # tooLarge is decided by the encoded reply, not the file size: a patch holds both versions, and JSON escapes grow it.
        if len(json.dumps(patch).encode("utf-8")) > self.max_bytes - REPLY_MARGIN:
            return {"patch": "", "binary": False, "tooLarge": True}
        return {"patch": patch, "binary": False}

    def resolve_action(self, params: dict) -> dict:
        if params.get("actionId") != "show-baseline":
            raise RpcError(INVALID_PARAMS, "Unknown action.")
        context = params.get("context") or {}
        path = context.get("path") if isinstance(context, dict) else None
        # Only a path this adapter itself reported as changed; anything else could point outside the folder.
        if relative_path_problem(path) or path not in {f["path"] for f in self.changes()}:
            raise RpcError(INVALID_PARAMS, "This action needs a changed file.")
        target = self.require_root() / MARKER / "base" / path
        if not target.exists():
            raise RpcError(PATH_NOT_CHANGED, "That file has no baseline copy.")
        try:
            relative = target.relative_to(self.workspace)
        except ValueError:
            raise RpcError(PATH_NOT_CHANGED, "The baseline copy is outside the workspace.")
        return {"kind": "openFile", "target": relative.as_posix()}


def serve(adapter: FolderAdapter) -> int:
    stdin = sys.stdin.buffer
    stdout = sys.stdout.buffer
    for raw in stdin:
        try:
            message = json.loads(raw)
        except (ValueError, RecursionError):
            message = None
            reply = {"jsonrpc": "2.0", "id": None, "error": {"code": PARSE_ERROR, "message": "The message is not valid JSON."}}
        else:
            if not isinstance(message, dict) or message.get("jsonrpc") != "2.0" or "method" not in message:
                reply = {"jsonrpc": "2.0", "id": message.get("id") if isinstance(message, dict) else None,
                         "error": {"code": INVALID_REQUEST, "message": "The message is not a JSON-RPC 2.0 request."}}
            elif "id" not in message:
                continue  # Notifications need no reply; version 1 defines none.
            else:
                try:
                    reply = {"jsonrpc": "2.0", "id": message["id"], "result": adapter.handle(message)}
                except RpcError as error:
                    reply = {"jsonrpc": "2.0", "id": message["id"], "error": {"code": error.code, "message": error.message}}
                except Exception:  # noqa: BLE001 - the adapter must answer, not die
                    reply = {"jsonrpc": "2.0", "id": message["id"], "error": {"code": INTERNAL_ERROR, "message": "The adapter failed unexpectedly."}}
        stdout.write(json.dumps(reply, separators=(",", ":")).encode("utf-8") + b"\n")
        stdout.flush()
        if isinstance(message, dict) and message.get("method") == "shutdown" and "result" in reply:
            break
    return 0


def snapshot(folder: str) -> int:
    root = Path(folder).resolve()
    base = root / MARKER / "base"
    if base.exists():
        shutil.rmtree(base)
    shutil.copytree(root, base, symlinks=True, ignore=shutil.ignore_patterns(MARKER))
    print(f"Baseline written to {base}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--snapshot":
        sys.exit(snapshot(sys.argv[2]))
    sys.exit(serve(FolderAdapter()))
