#!/usr/bin/env python3
"""Exercise the native runner's directory listing used by remote directory attachments and workspace moves."""
import argparse
import os
import pathlib
import subprocess
import tempfile

from test_remote_runner_compatibility import Bridge


def refused(bridge, path):
    try:
        bridge.request("directory.list", path=path)
    except RuntimeError as error:
        return str(error)
    raise AssertionError("Directory listing accepted an invalid path")


def verify(runner):
    if os.name == "nt":
        raise RuntimeError("This fixture uses POSIX paths. Windows directory listing has separate native tests.")
    runner = runner.resolve()
    protocol = int(subprocess.check_output([str(runner), "--protocol-version"], text=True))
    with tempfile.TemporaryDirectory(prefix="uam-runner-directories-") as temporary:
        root = pathlib.Path(temporary).resolve()
        workspace = root / "workspace"
        (workspace / "beta").mkdir(parents=True)
        (workspace / "alpha").mkdir()
        (workspace / "notes.txt").write_text("synthetic fixture\n")
        bridge = Bridge([str(runner), "bridge-direct"])
        try:
            bridge.request("hello", protocolVersion=protocol, nonce="directory-fixture")
            listing = bridge.request("directory.list", path=str(workspace))["result"]
            assert listing["directory"] == str(workspace)
            assert listing["parentDirectory"] == str(root)
            assert [entry["name"] for entry in listing["directories"]] == ["alpha", "beta"]
            assert listing["directories"][0]["path"] == str(workspace / "alpha")
            assert listing["truncated"] is False

            # A trailing separator or dot names the same directory and keeps a distinct parent.
            for spelling in (str(workspace) + "/", str(workspace) + "/.", str(workspace / "alpha") + "/.."):
                same = bridge.request("directory.list", path=spelling)["result"]
                assert same["directory"] == str(workspace), (spelling, same["directory"])
                assert same["parentDirectory"] == str(root), (spelling, same["parentDirectory"])

            top = bridge.request("directory.list", path="/")["result"]
            assert top["directory"] == "/" and top["parentDirectory"] == ""

            assert "does not exist" in refused(bridge, str(workspace / "missing"))
            assert "not a directory" in refused(bridge, str(workspace / "notes.txt"))
            assert "absolute" in refused(bridge, "workspace/alpha")
        finally:
            bridge.close()
        assert bridge.process.returncode == 0
    print("PASS: native runner directory listing, trailing-separator normalization and refusals")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=pathlib.Path)
    verify(parser.parse_args().runner)
