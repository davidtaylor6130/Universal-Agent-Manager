#!/usr/bin/env python3
"""Self-test for the VCS adapter conformance runner (issue #522).

Builds a disposable folder workspace, then runs conformance.py against the
sample folder adapter (every check must pass) and against each deliberate fault
in fixtures/misbehaving_adapter.py (the named check must fail with the expected
reason, and the runner must still finish within its time bound). Short limits
keep the run quick; they do not change what is checked.

Run: python3 tools/vcs-adapter/selftest.py
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNNER = HERE / "conformance.py"
SAMPLE = HERE / "sample_folder_adapter.py"
FAULTY = HERE / "fixtures" / "misbehaving_adapter.py"
# Generous enough that a healthy adapter on a loaded CI runner is never cut off; the faults sleep for 30 s.
LIMITS = ["--timeout-ms", "4000", "--handshake-timeout-ms", "6000", "--max-message-bytes", str(1024 * 1024)]
# Fault, the check that must fail, and text its reason must contain. None: every check must pass.
CASES = [
    ("with-history", None, ""),
    ("crash-on-status", "capability:status", "exited (code 3)"),
    ("hang-on-status", "capability:status", "did not answer vcs/status within 4000 ms"),
    ("oversize-status", "capability:status", "more than 1048576 bytes"),
    ("noise-on-status", "capability:status", "other than a JSON-RPC message"),
    ("wrong-id", "capability:status", "answered with id 999"),
    ("bad-status-path", "capability:status", "not a normalized workspace-relative path"),
    ("status-wrong-type", "capability:status", "a result the runner could not read"),
    ("empty-status", "capability:status", "listed no changed files"),
    ("history-bad-paths", "capability:history", "not a normalized workspace-relative path"),
    ("slow-handshake", "handshake", "did not answer initialize"),
    ("declares-reserved", "handshake", "declares commit"),
    ("declares-history-but-fails", "capability:history", "vcs/history failed"),
    ("answers-undeclared", "refuses:branchList", "answered vcs/branchList although it does not declare it"),
    ("bad-url-action", "actions", "not an http or https URL"),
    ("escaping-file-action", "actions", "not a normalized workspace-relative path"),
    ("root-slash", "actions", "outside the workspace"),
    ("action-never-resolves", "actions", "did not resolve"),
    ("resolves-hostile-path", "actions", "instead of refusing it"),
    ("run-command-action", "actions", "is not openUrl or openFile"),
    ("multiline-error", "error-messages", "one plain line"),
    ("url-in-error", "error-messages", "must not contain an address"),
    ("writes-workspace", "read-only", "the workspace changed"),
    ("ignores-shutdown", "shutdown", "did not exit"),
    ("orphan-on-status", "capability:status", "did not answer vcs/status"),
    ("deep-nesting", "capability:status", "other than a JSON-RPC message"),
    ("non-ascii-error", None, ""),
    ("stderr-flood", None, ""),
]
if os.name != "nt":
    # taskkill /T cannot find a child whose parent has already exited; the runner states this limit (UAM needs a Job Object).
    CASES.append(("orphan-then-crash", "capability:status", "exited (code 3)"))
ORPHAN_CASES = {"orphan-on-status", "orphan-then-crash"}
# Printed through a pipe in a code page that cannot show the adapter's message, as on Windows.
CASE_ENVIRONMENT = {"non-ascii-error": {"PYTHONIOENCODING": "cp1252"}}
# The slowest fault costs a 6 s handshake deadline or two 2 s shutdown waits; a hung runner would take far longer.
MAX_RUN_SECONDS = 60


def make_workspace(base: Path) -> Path:
    workspace = base / "workspace"
    (workspace / "src").mkdir(parents=True)
    (workspace / "sub").mkdir()
    (workspace / "a.txt").write_text("alpha\n", encoding="utf-8")
    (workspace / "src" / "b.txt").write_text("bravo\n", encoding="utf-8")
    (workspace / "sub" / "c.txt").write_text("charlie\n", encoding="utf-8")
    (workspace / "data.bin").write_bytes(b"\x00\x01\x02")
    # Rewritten below: a patch of both versions is larger than the 1 MiB message limit, so the adapter must say tooLarge.
    (workspace / "big.txt").write_text("old line\n" * 70000, encoding="utf-8")
    subprocess.run([sys.executable, str(SAMPLE), "--snapshot", str(workspace)], check=True, capture_output=True)
    (workspace / "a.txt").write_text("alpha\nalpha two\n", encoding="utf-8")
    (workspace / "sub" / "c.txt").unlink()
    (workspace / "new.txt").write_text("new\n", encoding="utf-8")
    (workspace / "data.bin").write_bytes(b"\x00\x01\x03")
    (workspace / "big.txt").write_text("new line\n" * 70000, encoding="utf-8")
    return workspace


def run(workspace: Path, report: Path, adapter: list[str], env: dict | None = None) -> tuple[int, dict, float, str]:
    started = time.monotonic()
    proc = subprocess.run([sys.executable, str(RUNNER), "--workspace", str(workspace), *LIMITS, "--json", str(report), "--", *adapter],
                          capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=MAX_RUN_SECONDS * 2,
                          env=dict(os.environ, **(env or {})))
    elapsed = time.monotonic() - started
    data = json.loads(report.read_text(encoding="utf-8")) if report.exists() else {"checks": []}
    return proc.returncode, data, elapsed, proc.stdout + proc.stderr


def main() -> int:
    failures = []
    base = Path(tempfile.mkdtemp(prefix="uam-adapter-selftest-"))
    try:
        workspace = make_workspace(base)
        code, data, elapsed, output = run(workspace, base / "sample.json", [sys.executable, str(SAMPLE)])
        bad = [c for c in data["checks"] if c["result"] == "fail"]
        if code != 0 or bad or len(data["checks"]) < 10:
            failures.append(f"sample adapter: exit {code}, failed {bad}\n{output}")
        print(f"{'ok  ' if code == 0 and not bad else 'FAIL'} sample adapter passes ({len(data['checks'])} checks, {elapsed:.1f} s)")
        for mode, check, reason in CASES:
            # Each fault gets a fresh copy, so a fault that writes cannot affect the next case.
            case_workspace = base / f"ws-{mode}"
            shutil.copytree(workspace, case_workspace, symlinks=True)
            marker = base / f"{mode}-orphan-marker"
            code, data, elapsed, output = run(case_workspace, base / f"{mode}.json", [sys.executable, str(FAULTY), mode, str(marker)],
                                              CASE_ENVIRONMENT.get(mode))
            if check is None:
                ok = code == 0 and not [c for c in data["checks"] if c["result"] == "fail"]
            else:
                hit = [c for c in data["checks"] if c["check"] == check and c["result"] == "fail" and reason in c["detail"]]
                ok = code == 1 and bool(hit)
            ok = ok and elapsed < MAX_RUN_SECONDS
            if mode in ORPHAN_CASES:
                # The grandchild marks its start, then writes the marker 8 s later unless the runner killed the whole tree.
                started_marker = Path(str(marker) + ".started")
                if not started_marker.exists():
                    ok = False
                    output += "\nThe adapter's child process never started."
                else:
                    time.sleep(max(0.0, started_marker.stat().st_mtime + 9 - time.time()))
                    if marker.exists():
                        ok = False
                        output += "\nThe adapter's child process outlived the adapter."
            print(f"{'ok  ' if ok else 'FAIL'} {mode}: {'passes' if check is None else check + ' fails'} ({elapsed:.1f} s)")
            if not ok:
                failures.append(f"{mode}: exit {code}, {elapsed:.1f} s\n{output}")
    finally:
        shutil.rmtree(base, ignore_errors=True)
    for failure in failures:
        print("\n" + failure)
    print(f"\n{len(CASES) + 1 - len(failures)} of {len(CASES) + 1} self-test cases passed.")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
