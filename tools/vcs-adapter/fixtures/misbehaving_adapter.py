#!/usr/bin/env python3
"""The sample folder adapter with one deliberate fault, chosen by the first argument.

Used only by selftest.py to prove the conformance runner catches each fault
and is not crashed or hung by it. Not an example to copy. The `with-history`
mode is the exception: a correct history implementation, so the runner's
positive history checks are exercised too.
"""
from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import sample_folder_adapter as sample  # noqa: E402

MODE = sys.argv[1] if len(sys.argv) > 1 else ""
ARGUMENT = sys.argv[2] if len(sys.argv) > 2 else ""
FILESYSTEM_ROOT = os.path.abspath(os.sep)
ORPHAN = "import sys, time; open(sys.argv[1] + '.started', 'w').close(); time.sleep(8); open(sys.argv[1], 'w').write('alive')"


class Faulty(sample.FolderAdapter):
    capabilities = {"declares-history-but-fails": ["status", "diff", "history"],
                    "history-bad-paths": ["status", "diff", "history"],
                    "with-history": ["status", "diff", "history"],
                    "declares-reserved": ["status", "diff", "commit"]}.get(MODE, ["status", "diff"])

    def methods(self) -> dict:
        table = super().methods()
        if MODE == "answers-undeclared":
            table["vcs/branchList"] = lambda params: {"branches": []}
        if MODE in ("history-bad-paths", "with-history"):
            table["vcs/history"] = self.history
            table["vcs/revisionDiff"] = self.revision_diff
        return table

    def initialize(self, params: dict) -> dict:
        if MODE == "slow-handshake":
            time.sleep(30)
        result = super().initialize(params)
        if MODE == "bad-url-action":
            result["actions"].append({"id": "web", "label": "Open in browser", "kind": "openUrl", "target": "javascript:alert(1)"})
        if MODE == "escaping-file-action":
            result["actions"].append({"id": "outside", "label": "Show outside", "kind": "openFile", "target": "../outside.txt"})
        if MODE == "root-slash":
            result["actions"].append({"id": "system", "label": "Show system root", "kind": "openFile", "target": FILESYSTEM_ROOT})
        if MODE == "run-command-action":
            result["actions"].append({"id": "run", "label": "Run tool", "kind": "runCommand", "target": "tool --open"})
        return result

    def detect(self, params: dict) -> dict:
        if MODE == "root-slash" and params.get("path") == str(self.workspace):
            # Claims the whole file system as its repository, so a check against its own root would allow anything.
            return {"repository": True, "root": FILESYSTEM_ROOT}
        return super().detect(params)

    def status(self, params: dict) -> dict:
        if MODE == "crash-on-status":
            os._exit(3)
        if MODE == "hang-on-status":
            time.sleep(30)
        if MODE in ("orphan-on-status", "orphan-then-crash"):
            # A grandchild that marks its start, then writes a second marker after 8 s unless it is killed with the adapter.
            child = subprocess.Popen([sys.executable, "-c", ORPHAN, ARGUMENT],
                                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            while child.poll() is None and not os.path.exists(ARGUMENT + ".started"):
                time.sleep(0.05)
            if MODE == "orphan-then-crash":
                os._exit(3)  # The child is now an orphan whose parent has gone.
            time.sleep(30)
        if MODE == "deep-nesting":
            sys.stdout.buffer.write(b'{"jsonrpc":"2.0","id":2,"result":' + b"[" * 100000 + b"]" * 100000 + b"}\n")
            sys.stdout.buffer.flush()
        if MODE == "oversize-status":
            sys.stdout.buffer.write(b'{"jsonrpc":"2.0","id":0,"result":"' + b"x" * (2 * 1024 * 1024) + b'"}\n')
            sys.stdout.buffer.flush()
        if MODE == "noise-on-status":
            sys.stdout.write("computing status...\n")
            sys.stdout.flush()
        if MODE == "writes-workspace":
            (self.require_root() / "written-by-adapter.txt").write_text("x\n", encoding="utf-8")
        if MODE == "empty-status":
            return {"branchOrRevision": "baseline", "files": []}
        result = super().status(params)
        if MODE == "bad-status-path" and result["files"]:
            result["files"][0]["path"] = "../escape.txt"
        if MODE == "status-wrong-type" and result["files"]:
            result["files"][0]["status"] = ["modified"]
        if MODE == "wrong-id":
            sys.stdout.buffer.write(b'{"jsonrpc":"2.0","id":999,"result":{"branchOrRevision":"x","files":[]}}\n')
            sys.stdout.buffer.flush()
        return result

    def diff(self, params: dict) -> dict:
        unchanged = params.get("path") not in {f["path"] for f in self.changes()}
        if MODE == "multiline-error" and unchanged:
            raise sample.RpcError(sample.PATH_NOT_CHANGED, "first line\nsecond line")
        if MODE == "non-ascii-error" and unchanged:
            raise sample.RpcError(sample.PATH_NOT_CHANGED, "変更はありません")
        if MODE == "url-in-error" and unchanged:
            raise sample.RpcError(sample.PATH_NOT_CHANGED, "No changes; see https://example.invalid/help")
        return super().diff(params)

    def resolve_action(self, params: dict) -> dict:
        if MODE == "action-never-resolves":
            raise sample.RpcError(sample.PATH_NOT_CHANGED, "Not available.")
        if MODE == "resolves-hostile-path":
            return {"kind": "openFile", "target": "."}
        return super().resolve_action(params)

    def history(self, params: dict) -> dict:
        base = sorted(sample.inventory(self.require_root() / sample.MARKER / "base"))[:3]
        paths = [{"action": "A", "path": p} for p in base]
        if MODE == "history-bad-paths":
            paths.append({"action": "M", "path": "../../outside.txt"})
        return {"entries": [{"revision": "1", "author": "sample", "date": "2026-11-23T00:00:00Z", "message": "Baseline", "paths": paths}],
                "next": ""}

    def revision_diff(self, params: dict) -> dict:
        path = params.get("path")
        base = sample.inventory(self.require_root() / sample.MARKER / "base")
        if params.get("revision") != "1" or path not in base:
            raise sample.RpcError(sample.PATH_NOT_CHANGED, "That revision did not change that file.")
        data = sample.read_item(base[path])
        if sample.is_binary(data):
            return {"patch": "", "binary": True}
        lines = self.lines(data)
        return {"patch": f"--- /dev/null\n+++ b/{path}\n@@ -0,0 +1,{len(lines)} @@\n" + "".join(f"+{l}\n" for l in lines), "binary": False}


if __name__ == "__main__":
    if MODE == "stderr-flood":
        # Far more than a pipe buffer: a runner that does not drain standard error would hang here.
        for _ in range(64):
            sys.stderr.write("diagnostic noise " * 4096 + "\n")
        sys.stderr.flush()
    code = sample.serve(Faulty())
    if MODE == "ignores-shutdown":
        time.sleep(30)
    sys.exit(code)
