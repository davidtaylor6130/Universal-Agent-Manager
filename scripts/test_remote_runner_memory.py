#!/usr/bin/env python3
"""Exercise project-memory persistence on the actual native runner, including Linux."""
import argparse
import pathlib
import subprocess
import tempfile

from test_remote_runner_compatibility import Bridge


def verify(runner):
    runner = runner.resolve()
    protocol = int(subprocess.check_output([str(runner), "--protocol-version"], text=True))
    with tempfile.TemporaryDirectory(prefix="uam-runner-memory-") as temporary:
        root = pathlib.Path(temporary).resolve()
        workspace, other = root / "workspace", root / "other"
        workspace.mkdir()
        other.mkdir()
        bridge = Bridge([str(runner), "bridge-direct"])
        try:
            hello = bridge.request("hello", protocolVersion=protocol, nonce="memory-fixture")
            assert hello["capabilities"]["projectMemory"]
            entry = bridge.request("memory.create", workspace=str(workspace), draft=dict(
                category="Lessons/User_Lessons", title="Private runner fixture",
                memory="Remember synthetic cedar", evidence="Synthetic fixture",
                confidence="high", sourceChatId="qa"))["result"]["entry"]
            entries = bridge.request("memory.list", workspace=str(workspace))["result"]["entries"]
            assert len(entries) == 1 and entries[0]["id"] == entry["id"]
            recall = bridge.request("context.memory", workspace=str(workspace), budget=512)["result"]["text"]
            assert "Remember synthetic cedar" in recall
            assert bridge.request("memory.list", workspace=str(other))["result"]["entries"] == []
            try:
                bridge.request("memory.delete", workspace=str(workspace), entryId="../outside.md")
            except RuntimeError:
                pass
            else:
                raise AssertionError("Memory deletion accepted a path outside its workspace")
            bridge.request("memory.delete", workspace=str(workspace), entryId=entry["id"])
            assert bridge.request("memory.list", workspace=str(workspace))["result"]["entries"] == []
        finally:
            bridge.close()
        assert bridge.process.returncode == 0
    print("PASS: native runner memory persistence, recall, deletion, workspace isolation and path validation")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=pathlib.Path)
    verify(parser.parse_args().runner)
