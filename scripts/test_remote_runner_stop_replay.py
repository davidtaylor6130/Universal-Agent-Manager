#!/usr/bin/env python3
"""A stop must not write output the app already received again.

The proxy writes each provider batch with a delivery marker and keeps it until the app acknowledges it. Model
discovery stops as soon as it reads its answer, before acknowledging that batch, and a chat can be stopped while a
streamed batch waits for its acknowledgement. The stop's final drain must start after the delivered batch, not at the
runner's last acknowledged cursor, or the app reads the same JSON-RPC reply or notification twice.
"""
import argparse
import base64
import json
import os
import pathlib
import select
import socket
import subprocess
import sys
import tempfile
import time

MARKER = b"\x1eUAM_REMOTE_OUTPUT "
STOP = b"\x1eUAM_REMOTE_STOP\n"
TOKEN = "stop-replay-token"
UNAVAILABLE = b"Remote process proxy is not yet available on this platform."


class ProxyUnavailable(Exception):
    """The desktop-side proxy is built only for macOS and Windows; a Linux runner refuses it."""


def wait_for_socket(path, deadline):
    while time.monotonic() < deadline:
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
                probe.connect(path)
            return
        except OSError:
            time.sleep(0.05)
    raise AssertionError("The runner service did not start.")


def read_until(process, output, predicate, deadline):
    stream = process.stdout
    while not predicate(output) and time.monotonic() < deadline:
        ready, _, _ = select.select([stream], [], [], 0.05)
        if not ready:
            if process.poll() is not None:
                break
            continue
        chunk = os.read(stream.fileno(), 65536)
        if not chunk:
            break
        output.extend(chunk)
    return predicate(output)


def run_case(runner, version, root, name, provider_script, lines_before_stop, acknowledge):
    """Starts one proxied provider, optionally acknowledges its first batch, then stops it and returns all stdout."""
    socket_path = str(root / f"{name}.sock")
    workspace = root / name
    workspace.mkdir()
    fake_ssh = root / "bin" / "ssh"
    fake_ssh.parent.mkdir(exist_ok=True)
    fake_ssh.write_text(f'#!/bin/sh\nexec "{runner}" bridge --socket "{socket_path}"\n')
    fake_ssh.chmod(0o700)
    service = subprocess.Popen([runner, "serve", "--socket", socket_path], cwd=workspace,
                               stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for_socket(socket_path, time.monotonic() + 30)
        spec = base64.b64encode(json.dumps({
            "sessionId": name, "cwd": str(workspace), "argv": ["/bin/sh", "-c", provider_script],
            "environment": {}, "attachOnly": False, "transientProcess": False, "deliveryToken": TOKEN,
            "deliveredStdoutCursor": 0, "deliveredStderrCursor": 0, "sessionMcpServers": [],
            "configurationProvider": "", "configurationDirectory": "", "contextProviderId": "", "contextDirectory": "",
        }).encode()).decode()
        environment = dict(os.environ, PATH=f"{fake_ssh.parent}:{os.environ.get('PATH', '')}", UAM_REMOTE_PROCESS_SPEC=spec)
        proxy = subprocess.Popen([runner, "proxy", "--alias", "test-host", "--platform", "linux", "--version", version],
                                 cwd=workspace, env=environment, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
        output = bytearray()
        try:
            def first_batch_marked(data):
                marker = data.find(MARKER)
                return all(line in data for line in lines_before_stop) and marker >= 0 and data.find(b"\n", marker) >= 0

            if not read_until(proxy, output, first_batch_marked, time.monotonic() + 30) and proxy.poll() is not None:
                if UNAVAILABLE in proxy.stderr.read():
                    raise ProxyUnavailable()
            assert first_batch_marked(output), \
                f"{name}: the first batch was not delivered: {bytes(output)!r}"
            if acknowledge:
                start = output.find(MARKER)
                end = output.find(b"\n", start)
                ack = b"\x1eUAM_REMOTE_OUTPUT_ACK " + bytes(output[start + len(MARKER):end]) + b"\n"
                proxy.stdin.write(ack)
                proxy.stdin.flush()
                time.sleep(0.3)  # Let the proxy forward the acknowledgement before the stop arrives.
            proxy.stdin.write(STOP)
            proxy.stdin.flush()
            read_until(proxy, output, lambda _: False, time.monotonic() + 30)
            proxy.wait(timeout=30)
            stderr = proxy.stderr.read()
        finally:
            if proxy.poll() is None:
                proxy.kill()
                proxy.wait()
        assert proxy.returncode == 0, f"{name}: proxy exit {proxy.returncode}: {stderr!r}"
        assert f"UAM_REMOTE_STOP_OUTCOME {TOKEN} ".encode() in stderr, f"{name}: no stop outcome: {stderr!r}"
        return bytes(output)
    finally:
        service.terminate()
        try:
            service.wait(timeout=10)
        except subprocess.TimeoutExpired:
            service.kill()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    args = parser.parse_args()
    runner = str(pathlib.Path(args.runner).resolve())
    version = subprocess.run([runner, "--version"], check=True, capture_output=True, text=True).stdout.strip()
    try:
        run_cases(runner, version)
    except ProxyUnavailable:
        print("SKIPPED: this runner has no desktop-side proxy (it is built only for macOS and Windows).")
        return 0
    print("Remote runner stop does not replay delivered output.")
    return 0


def run_cases(runner, version):
    with tempfile.TemporaryDirectory(prefix="uam-stop-replay-") as directory:
        root = pathlib.Path(directory)
        reply = b'{"jsonrpc":"2.0","id":2,"result":{"sessionId":"discovered"}}'
        # Discovery: the reply is delivered, the app stops before acknowledging it, and the provider is idle.
        output = run_case(runner, version, root, "unacknowledged", f"printf '%s\\n' '{reply.decode()}'; exec sleep 30",
                          [reply], acknowledge=False)
        assert output.count(reply) == 1, f"The stop replayed an unacknowledged delivered reply: {output!r}"

        # A streaming chat: one notification is delivered but unacknowledged, a later one is still being written.
        first = b'{"jsonrpc":"2.0","method":"session/update","params":{"n":1}}'
        later = b'{"jsonrpc":"2.0","method":"session/update","params":{"n":2}}'
        output = run_case(runner, version, root, "streaming",
                          f"printf '%s\\n' '{first.decode()}'; sleep 1; printf '%s\\n' '{later.decode()}'; exec sleep 30",
                          [first], acknowledge=False)
        assert output.count(first) == 1, f"The stop replayed a streamed notification: {output!r}"
        assert output.count(later) <= 1, f"The stop wrote later output twice: {output!r}"

        # An acknowledged batch was never replayed; this keeps that path covered beside the fix.
        output = run_case(runner, version, root, "acknowledged", f"printf '%s\\n' '{reply.decode()}'; exec sleep 30",
                          [reply], acknowledge=True)
        assert output.count(reply) == 1, f"The stop replayed an acknowledged reply: {output!r}"


if __name__ == "__main__":
    sys.exit(main())
