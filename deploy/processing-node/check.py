"""Offline deployment contract checks. Does not start Docker or contact a provider."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

directory = Path(__file__).resolve().parent
environment = os.environ.copy()
environment.update(UAM_AUTHORIZED_KEYS="/tmp/uam-test-public-keys", UAM_WORKSPACE_DIR="/tmp/uam-test-project")
command = ["docker", "compose", "-f", str(directory / "compose.yaml"), "--profile", "processing-node", "config", "--format", "json"]
configuration = json.loads(subprocess.check_output(command, env=environment, text=True))
service = configuration["services"]["processing-node"]
assert service["profiles"] == ["processing-node"]
assert service["ports"][0]["host_ip"] == "127.0.0.1"
assert service["ports"][0]["target"] == 22
mounts = {mount["target"]: mount for mount in service["volumes"]}
assert mounts["/run/uam-authorized-keys"]["read_only"]
assert not mounts["/run/uam-authorized-keys"]["bind"]["create_host_path"]
assert not mounts["/workspace"]["bind"]["create_host_path"]
for variable in ("UAM_AUTHORIZED_KEYS", "UAM_WORKSPACE_DIR"):
    missing = environment.copy()
    missing.pop(variable)
    result = subprocess.run(command, env=missing, capture_output=True, text=True)
    assert result.returncode != 0 and variable in result.stderr
subprocess.run(["sh", "-n", str(directory / "start.sh")], check=True)
with tempfile.TemporaryDirectory(prefix="uam-node-check-") as temporary:
    # Use an absent private test input so the host entrypoint cannot reach its writes.
    script = (directory / "start.sh").read_text().replace("/run/uam-authorized-keys", str(Path(temporary) / "absent-keys"))
    result = subprocess.run(["sh"], input=script, capture_output=True, text=True)
    assert result.returncode != 0 and "nonempty public authorized_keys" in result.stderr
print("Passed: loopback/profile/mount contract, both required inputs, shell syntax, missing-key refusal.")
