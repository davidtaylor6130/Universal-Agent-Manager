#!/usr/bin/env python3
"""The native runner service must survive bridges that close before their replies (Linux SIGPIPE)."""
import argparse
import json
import pathlib
import socket
import struct
import subprocess
import tempfile
import time


def listening(path):
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
            probe.connect(path)
        return True
    except OSError:
        return False


def verify(runner):
    runner = runner.resolve()
    body = json.dumps({"id": "closed-bridge", "type": "hello"}).encode()
    frames = (struct.pack(">I", len(body)) + body) * 64
    with tempfile.TemporaryDirectory(prefix="uam-closed-bridge-") as temporary:
        path = str(pathlib.Path(temporary).resolve() / "r.sock")
        # Popen restores SIGPIPE to its default, as a helper started over SSH has it.
        service = subprocess.Popen([str(runner), "serve", "--socket", path], stdin=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 10
            while not listening(path):
                assert service.poll() is None and time.monotonic() < deadline, "Runner service did not start."
                time.sleep(0.01)
            for _ in range(20):
                while not listening(path):  # The listen backlog is small; a dead service fails below.
                    if service.poll() is not None or time.monotonic() > deadline + 20:
                        break
                    time.sleep(0.01)
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as bridge:
                    try:
                        bridge.connect(path)
                        bridge.sendall(frames)
                    except OSError:
                        break
            time.sleep(0.5)
            assert service.poll() is None, f"Runner service died after closed bridges (exit {service.returncode})."
            assert subprocess.run([str(runner), "stop", "--socket", path], timeout=15).returncode == 0
            service.wait(timeout=15)
        finally:
            if service.poll() is None:
                service.kill()
                service.wait()
    print("PASS: runner service survives bridges that close before their replies")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=pathlib.Path)
    verify(parser.parse_args().runner)
