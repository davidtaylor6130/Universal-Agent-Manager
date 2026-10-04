#!/usr/bin/env python3
"""Verify current disk helpers attach to an older compatible live daemon without stopping owned work."""
import argparse
import base64
import hashlib
import json
import pathlib
import queue
import shlex
import struct
import subprocess
import tempfile
import threading
import time
import uuid


class Bridge:
    def __init__(self, argv):
        self.process = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.responses = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        try:
            while True:
                header = self.process.stdout.read(4)
                if len(header) != 4:
                    raise RuntimeError("Runner bridge closed before its reply.")
                length = struct.unpack(">I", header)[0]
                if not 0 < length <= 4 * 1024 * 1024:
                    raise RuntimeError("Invalid runner frame size.")
                body = self.process.stdout.read(length)
                if len(body) != length:
                    raise RuntimeError("Truncated runner frame.")
                self.responses.put(json.loads(body))
        except Exception as error:
            self.responses.put(error)

    def request(self, operation, **fields):
        identifier = uuid.uuid4().hex
        body = json.dumps(dict(id=identifier, type=operation, **fields), separators=(",", ":")).encode()
        self.process.stdin.write(struct.pack(">I", len(body)) + body)
        self.process.stdin.flush()
        reply = self.responses.get(timeout=15)
        if isinstance(reply, Exception):
            raise reply
        if reply.get("id") != identifier:
            raise RuntimeError("Runner response identity mismatch.")
        if not reply.get("ok"):
            raise RuntimeError(str(reply.get("error")))
        return reply

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)


def verify(current, previous, ssh_config=None, ssh_alias="loopback"):
    if __import__("os").name == "nt":
        raise RuntimeError("This fixture uses a private Unix socket. Windows named-pipe lifecycle has separate native tests.")
    current, previous = current.resolve(), previous.resolve()
    version = subprocess.check_output([str(current), "--version"], text=True).strip()
    protocol = int(subprocess.check_output([str(current), "--protocol-version"], text=True))
    fingerprint = subprocess.check_output([str(current), "--source-fingerprint"], text=True).strip()
    manifest = json.loads((current.parent / "uam-runner.manifest.json").read_text())
    assert fingerprint == manifest["sourceFingerprint"] and len(fingerprint) == 64
    assert hashlib.sha256(current.read_bytes()).hexdigest() == (current.parent / "uam-runner.sha256").read_text().strip()
    assert subprocess.check_output([str(previous), "--version"], text=True).strip() == version
    assert int(subprocess.check_output([str(previous), "--protocol-version"], text=True)) == protocol
    with tempfile.TemporaryDirectory(prefix="uam-old-service-", dir="/private/tmp" if pathlib.Path("/private/tmp").exists() else None) as temporary:
        root = pathlib.Path(temporary).resolve()
        endpoint = root / "runner.sock"
        start = subprocess.run([str(previous), "start", "--socket", str(endpoint)], capture_output=True, text=True, timeout=15)
        if start.returncode:
            raise RuntimeError("Private fixture daemon start failed: " + start.stderr.strip())
        session, token = uuid.uuid4().hex, uuid.uuid4().hex
        argv = [str(current), "bridge", "--socket", str(endpoint)]
        if ssh_config:
            command = "exec " + shlex.join(argv)
            argv = ["/usr/bin/ssh", "-F", str(ssh_config.resolve()), "-o", "BatchMode=yes", ssh_alias, command]
        bridges = []
        started = False
        try:
            first = Bridge(argv)
            bridges.append(first)
            hello = first.request("hello", protocolVersion=protocol, nonce=uuid.uuid4().hex)
            assert not hello["capabilities"].get("contextRead", False)
            assert not hello["capabilities"].get("projectMemory", False)
            first.request("process.start", sessionId=session, controlToken=token, cwd=str(root),
                          argv=["/bin/sh", "-c", "printf 'before\\n'; while IFS= read -r line; do printf '%s\\n' \"$line\"; done"],
                          environment={}, attachIfExists=False)
            started = True
            first.close()
            bridges.remove(first)
            # This is the new on-disk bridge talking to the still-running old daemon.
            stopped = subprocess.run([str(current), "stop", "--socket", str(endpoint)], capture_output=True, text=True, timeout=15)
            assert stopped.returncode == 2, "A busy daemon was replaced or stopped."
            assert "own" in stopped.stderr.lower() or "managed" in stopped.stderr.lower(), stopped.stderr
            second = Bridge(argv)
            bridges.append(second)
            second.request("hello", protocolVersion=protocol, nonce=uuid.uuid4().hex)
            second.request("process.write", sessionId=session, controlToken=token, inputSequence=1,
                           deliveryId="compatibility-once", dataBase64=base64.b64encode(b"after-reconnect\n").decode())
            duplicate = second.request("process.write", sessionId=session, controlToken=token, inputSequence=1,
                                       deliveryId="compatibility-once", dataBase64=base64.b64encode(b"after-reconnect\n").decode())
            assert duplicate["result"].get("duplicate"), "Input was not acknowledged idempotently."
            second.request("process.closeInput", sessionId=session, controlToken=token)
            output = bytearray()
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                poll = second.request("process.poll", sessionId=session, controlToken=token)["result"]
                output.extend(base64.b64decode(poll["stdoutBase64"]))
                second.request("process.ack", sessionId=session, controlToken=token,
                               stdoutCursor=poll["stdoutCursor"], stderrCursor=poll["stderrCursor"])
                if not poll["running"]:
                    break
                time.sleep(0.02)
            else:
                raise RuntimeError("Owned fixture process did not exit.")
            assert bytes(output) == b"before\nafter-reconnect\n", output
            second.request("process.remove", sessionId=session, controlToken=token)
            started = False
        finally:
            if started:
                try:
                    cleanup = Bridge(argv)
                    bridges.append(cleanup)
                    cleanup.request("hello", protocolVersion=protocol, nonce=uuid.uuid4().hex)
                    bridges[-1].request("process.stop", sessionId=session, controlToken=token)
                    bridges[-1].request("process.remove", sessionId=session, controlToken=token)
                except Exception:
                    pass
            for bridge in bridges:
                bridge.close()
            result = subprocess.run([str(previous), "stop", "--socket", str(endpoint)], capture_output=True, timeout=15)
            if result.returncode:
                raise RuntimeError("Private fixture daemon could not be stopped after owned job cleanup.")
    print("PASS: new disk helper / old live daemon, busy-stop preservation, reconnect, acknowledged single delivery, owned cleanup"
          + (" over actual same-machine SSH loopback" if ssh_config else " over private local Unix transport"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--current-runner", type=pathlib.Path, required=True)
    parser.add_argument("--previous-runner", type=pathlib.Path, required=True)
    parser.add_argument("--ssh-config", type=pathlib.Path)
    parser.add_argument("--ssh-alias", default="loopback")
    args = parser.parse_args()
    verify(args.current_runner, args.previous_runner, args.ssh_config, args.ssh_alias)
