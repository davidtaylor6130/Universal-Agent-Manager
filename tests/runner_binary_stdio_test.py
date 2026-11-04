"""Check genuine direct-bridge frames at the Windows CRT LF and CTRL-Z boundaries."""
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


def encode(value):
    return json.dumps(value, separators=(",", ":"), sort_keys=True).encode("utf-8")


def hello(identifier, nonce):
    return {"id": identifier, "nonce": nonce, "protocolVersion": 3, "type": "hello"}


def exchange(binary, request, inventory):
    body = encode(request)
    header = struct.pack(">I", len(body))
    child = subprocess.Popen(
        [str(binary), "bridge-direct"], cwd=binary.parent,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    inventory.append(child)
    try:
        output, error = child.communicate(header + body, timeout=10)
    finally:
        if child.poll() is None:
            child.kill()
            child.communicate(timeout=5)
    assert child.returncode == 0, f"Direct bridge exit code {child.returncode}"
    assert not error, f"Unexpected bridge stderr ({len(error)} bytes)"
    assert 4 < len(output) <= 8192, "Response size outside hello bounds"
    size = struct.unpack(">I", output[:4])[0]
    assert size == len(output) - 4, "Response header does not match the exact body length"
    response = json.loads(output[4:].decode("utf-8"))
    assert response["id"] == request["id"] and response["nonce"] == request["nonce"]
    assert response["type"] == "hello" and response["ok"] is True
    assert response["protocolVersion"] == 3
    assert response["capabilities"]["processExecution"] is True
    assert output[4:] == encode(response), "Unexpected response serialization"
    return response, output[:4], header


def main():
    source = Path(sys.argv[1]).resolve(strict=True)
    inventory = []
    # Windows RunnerState creates its private spool next to the executable.
    # A disposable copy keeps every child and its spool inside this test root.
    with tempfile.TemporaryDirectory(prefix="uam-binary-stdio-") as directory:
        binary = Path(directory) / source.name
        shutil.copy2(source, binary)
        try:
            ordinary, _, _ = exchange(binary, hello("ordinary", "n" * 32), inventory)

            # Choose legal bounded identity fields so a response length byte is LF.
            # Derive the size from the real hello, allowing platform/version changes.
            lf_request = None
            for identifier_size in range(1, 129):
                for nonce_size in range(1, 129):
                    candidate = hello("i" * identifier_size, "n" * nonce_size)
                    expected = {**ordinary, "id": candidate["id"], "nonce": candidate["nonce"]}
                    if 0x0A in struct.pack(">I", len(encode(expected))):
                        lf_request = candidate
                        break
                if lf_request is not None:
                    break
            assert lf_request is not None, "No legal hello identity reached the LF boundary"
            eof_request = hello("i" * 99, "n" * 128)
            assert struct.pack(">I", len(encode(eof_request))) == b"\x00\x00\x01\x1a"
            failures = []
            for name, request in [("LF output", lf_request), ("CTRL-Z input", eof_request)]:
                try:
                    _, response_header, request_header = exchange(binary, request, inventory)
                    if name == "LF output":
                        assert 0x0A in response_header, "LF response boundary was not exercised"
                    else:
                        assert 0x1A in request_header, "CTRL-Z input boundary was not exercised"
                except (AssertionError, subprocess.TimeoutExpired) as error:
                    failures.append(f"{name}: {error}")
            assert len(inventory) == 3, "Expected exactly three genuine direct children"
            assert list(binary.parent.iterdir()) == [binary], "Runner left temporary spool files"
            assert not failures, "; ".join(failures)
            print("PASS ordinary, LF output header, CTRL-Z input header: 3 children reaped")
        finally:
            for child in inventory:
                if child.poll() is None:
                    child.kill()
                    child.wait(timeout=5)
            assert all(child.poll() is not None for child in inventory), "Direct child survived"


if __name__ == "__main__":
    main()
