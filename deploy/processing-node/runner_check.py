"""Check an explicitly prepared loopback test node, using only a synthetic cat process.

Usage: python3 runner_check.py IDENTITY_FILE VERIFIED_KNOWN_HOSTS_FILE [PORT]
The host-key file must come from the owned test container, not ssh-keyscan trust.
"""
import base64
import json
from pathlib import Path
import secrets
import struct
import subprocess
import sys
import time

identity, known_hosts = map(str, map(Path, sys.argv[1:3]))
port = sys.argv[3] if len(sys.argv) > 3 else "62222"
ssh = ["ssh", "-F", "/dev/null", "-p", port, "-i", identity,
       "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
       "-o", f"UserKnownHostsFile={known_hosts}", "-o", "ConnectTimeout=5", "uam@127.0.0.1"]
socket = "/home/uam/.node-check-" + secrets.token_hex(6) + ".sock"
assert subprocess.check_output(ssh + ["id -u"], timeout=10).strip() == b"1000"
version = subprocess.check_output(ssh + ["uam-runner --version"], timeout=10).strip()
protocol = int(subprocess.check_output(ssh + ["uam-runner --protocol-version"], timeout=10))
subprocess.run(ssh + [f"uam-runner start --socket {socket}"], check=True, timeout=10)

def request(kind, **fields):
    payload = json.dumps(dict(id=secrets.token_hex(8), type=kind, **fields)).encode()
    # Every request reconnects through SSH to prove state survives bridge disconnection.
    reply = subprocess.check_output(ssh + [f"uam-runner bridge --socket {socket}"],
                                    input=struct.pack("!I", len(payload)) + payload, timeout=10)
    assert len(reply) >= 4 and struct.unpack("!I", reply[:4])[0] == len(reply) - 4
    return json.loads(reply[4:])

session = dict(sessionId="compose-check-" + secrets.token_hex(8), controlToken=secrets.token_hex(32))
launch = dict(**session, cwd="/workspace", argv=["/bin/cat"])
try:
    hello = request("hello", protocolVersion=protocol, nonce="compose-check")
    assert hello["ok"] and hello["platform"] == "linux" and hello["runnerVersion"].encode() == version
    assert not request("hello", protocolVersion=-1, nonce="compose-check")["ok"]
    assert request("process.start", **launch)["ok"]
    assert request("process.start", **launch, attachIfExists=True)["result"]["attached"]
    unauthorized = request("process.stop", sessionId=session["sessionId"], controlToken="wrong")
    assert not unauthorized["ok"] and unauthorized["error"]["code"] == "unauthorized"
    message = b"isolated processing node check\n"
    assert request("process.write", **session, dataBase64=base64.b64encode(message).decode())["ok"]
    output = b""
    deadline = time.monotonic() + 10
    while message not in output and time.monotonic() < deadline:
        reply = request("process.poll", **session)
        assert reply["ok"] and reply["result"]["running"]
        output += base64.b64decode(reply["result"]["stdoutBase64"])
        time.sleep(0.05)
    assert message in output
    assert request("process.stop", **session)["result"]["running"] is False
    assert request("process.remove", **session)["ok"]
finally:
    subprocess.run(ssh + [f"uam-runner stop --socket {socket}"], check=True, timeout=10)
print("Passed: nonroot SSH, runner version/handshake, protocol refusal, start/reconnect/attach, token refusal, input/output, stop/remove.")
