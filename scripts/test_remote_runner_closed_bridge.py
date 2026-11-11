#!/usr/bin/env python3
"""The native runner service must survive bridges that close first, keep its descriptors out of the processes it starts, and never hang on a socket nobody serves."""
import argparse
import json
import os
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time


def listening(path):
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
            probe.connect(path)
        return True
    except OSError:
        return False


def check_sockets_close_on_exec(pid):
    """Provider processes must not inherit the listener: one that outlives the service keeps its socket looking alive."""
    sockets = 0
    for fd in pathlib.Path(f"/proc/{pid}/fd").iterdir():
        try:
            if not str(fd.readlink()).startswith("socket:"):
                continue
            flags = next(line for line in (pathlib.Path(f"/proc/{pid}/fdinfo") / fd.name).read_text().splitlines()
                         if line.startswith("flags:"))
        except OSError:
            continue
        sockets += 1
        assert int(flags.split()[1], 8) & 0o2000000, f"Runner service socket fd {fd.name} is inherited by child processes."
    assert sockets > 0, "Runner service has no open sockets to check."


def check_started_processes_hold_only_standard_streams(service, runner, path, workspace):
    """A process holding anything of the service's outlives it badly: its listener keeps accepting, and another session's input pipe never ends."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as bridge, bridge.makefile("rb") as replies:
        bridge.connect(path)

        def request(kind, session, **fields):
            body = json.dumps({"id": kind, "type": kind, "sessionId": session, "controlToken": "token", **fields}).encode()
            bridge.sendall(struct.pack(">I", len(body)) + body)
            reply = json.loads(replies.read(struct.unpack(">I", replies.read(4))[0]))
            assert reply.get("ok"), f"{kind} failed: {reply}"

        request("process.start", "holder", cwd=workspace, argv=["/bin/cat"])
        request("process.write", "holder", dataBase64="eA==")  # The service now holds its own writer on this input pipe.
        request("process.start", "later", cwd=workspace, argv=["/bin/sleep", "30"])
        children = []
        for entry in pathlib.Path("/proc").iterdir():
            try:
                if entry.name.isdigit() and (entry / "stat").read_text().rsplit(")", 1)[1].split()[1] == str(service.pid):
                    children.append(int(entry.name))
            except OSError:  # A process that ended while /proc was being read.
                continue
        assert len(children) == 2, f"Expected the two started processes, found {children}."
        deadline = time.monotonic() + 10
        for child in children:
            while os.readlink(f"/proc/{child}/exe") == str(runner):  # Not yet replaced by the started program.
                assert time.monotonic() < deadline, "A started process never ran its program."
                time.sleep(0.01)
            held = sorted(int(name) for name in os.listdir(f"/proc/{child}/fd"))
            assert held == [0, 1, 2], f"A started process inherited runner descriptors: {held}."
        for session in ("holder", "later"):
            request("process.stop", session)
            request("process.remove", session)


def check_unserved_socket_cannot_hang(runner, directory):
    """A listener nobody accepts on, as a dead service leaves in a process it started, must not hold stop, bridge or start, and start replaces it."""
    path = str(directory / "u.sock")

    def run(command):
        return subprocess.run([str(runner), command, "--socket", path], stdin=subprocess.DEVNULL, timeout=40).returncode

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
        listener.bind(path)
        listener.listen(4)
        assert run("stop") == 0, "Stop waited on a shutdown reply nobody will send."  # Connected, never answered, and no service lock.
        waiting = []
        try:
            while True:  # Fill the backlog: the next blocking connect would sleep until an accept.
                assert len(waiting) < 64, "The listen backlog never filled."
                waiting.append(socket.socket(socket.AF_UNIX, socket.SOCK_STREAM))
                waiting[-1].setblocking(False)
                try:
                    waiting[-1].connect(path)
                except BlockingIOError:
                    break
            assert run("bridge") == 2, "A bridge connected to a socket nobody serves."
            assert run("start") == 0, "Start did not replace a socket no service owns."  # The install would roll back otherwise.
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
                probe.connect(path)  # The replacement service answers where the old socket could not.
        finally:
            for client in waiting:
                client.close()
            assert run("stop") == 0, "The replacement service did not stop."  # Also on failure, so no detached service is left behind.


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
            if sys.platform == "linux":  # The three checks below read /proc and rely on Linux backlog behaviour.
                check_sockets_close_on_exec(service.pid)
                directory = pathlib.Path(temporary).resolve()
                check_started_processes_hold_only_standard_streams(service, runner, path, str(directory))
                check_unserved_socket_cannot_hang(runner, directory)
            assert subprocess.run([str(runner), "stop", "--socket", path], timeout=15).returncode == 0
            service.wait(timeout=15)
        finally:
            if service.poll() is None:
                service.kill()
                service.wait()
    print("PASS: runner service survives closed bridges, keeps its descriptors and does not hang on an unserved socket")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", required=True, type=pathlib.Path)
    verify(parser.parse_args().runner)
