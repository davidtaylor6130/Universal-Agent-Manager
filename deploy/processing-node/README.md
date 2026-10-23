# Linux processing node

Validation status: the original branch recorded Linux ARM64 image and isolated Docker checks; those results are historical. The restored deployment and runner checks have not been rerun against the consolidated source. Linux x86-64, Windows-container operation and user acceptance remain unverified. Run `python3 deploy/processing-node/check.py` for offline checks. `runner_check.py` exercises a disposable loopback node using an explicit test identity and verified host-key file.

This optional Compose deployment wraps the existing standalone runner in an OpenSSH server. UAM remains the management application and uses its normal remote-host setup. No new RPC transport, discovery service, or headless management server is included.

The default published SSH endpoint is `127.0.0.1:2222`. The service requires the `processing-node` profile, a public authorized-keys file, and a project directory. It never mounts your private SSH key, SSH agent, Docker socket, or desktop data directory. Provider processes run as `uam`, with project files at `/workspace`. Container home data and host keys persist in named volumes.

From the repository root:

```sh
export UAM_AUTHORIZED_KEYS=/absolute/path/to/authorized_keys
export UAM_WORKSPACE_DIR=/absolute/path/to/project
docker compose -f deploy/processing-node/compose.yaml --profile processing-node up --build -d
docker compose -f deploy/processing-node/compose.yaml exec processing-node \
    ssh-keygen -lf /var/lib/uam-host-keys/ssh_host_ed25519_key.pub
```

Add an alias to your desktop `~/.ssh/config`:

```sshconfig
Host uam-container
    HostName 127.0.0.1
    Port 2222
    User uam
    IdentityFile ~/.ssh/your-key
    IdentitiesOnly yes
```

Connect with `ssh uam-container`, compare the host fingerprint with the command above, and accept it only when it matches. Install the provider CLIs you use under this container account using their documented installation commands. Node.js, npm, Git, and the runner are included; provider versions and credentials are not baked into the image. npm global installs use `/home/uam/.local`, which persists across container recreation. Authenticate each provider inside the container. Container credentials belong to this execution account.

Use the alias in UAM remote-host setup, select Linux, then validate and install the matching packaged helper. That installer retains version/checksum checks and installs the controller's helper in the container's persistent home. Create a remote workspace using `/workspace` or its subdirectory. Ensure the project mount permits writes by container UID 1000; do not recursively change your project ownership merely to make this work.

To use a node on another machine without exposing the container listener to the LAN, keep its loopback binding and tunnel it through that machine's existing SSH service:

```sh
ssh -N -L 2222:127.0.0.1:2222 your-node-host
```

Then use the same `uam-container` alias from the primary device. Change the local forwarded port and alias together if port 2222 is already occupied. OpenSSH handles machine authentication and host verification on both connections.

Stopping the container stops its provider processes. Desktop disconnection alone uses the runner's existing reconnect behavior; it does not migrate work. Run `docker compose -f deploy/processing-node/compose.yaml --profile processing-node down` to stop this deployment. Do not add `--volumes` unless you intend to erase provider credentials, helper state, and host keys. Project files in the bind mount survive either operation.

This is a Linux deployment on ARM64 or x86-64 Docker hosts, including Linux containers hosted from macOS or Windows. It is not a Windows-container image. No LAN auto-discovery, scheduler, file synchronization, or desktop computer use is added.
