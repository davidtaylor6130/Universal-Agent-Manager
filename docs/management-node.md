# Headless management node

`--management-node` runs the existing application state, provider runtimes, polling, and authenticated companion API without constructing a CEF BrowserView. The desktop remains the default. The management process still uses CEF's application event loop and bundled resources; it is not the Linux processing runner.

Use a dedicated data directory and a private companion token file:

```sh
UAM_DATA_DIR=/path/to/manager-data \
UAM_COMPANION_TOKEN_FILE=/path/to/private-token-file \
UAM_COMPANION_PORT=58948 \
./Builds/universal_agent_manager.app/Contents/MacOS/universal_agent_manager --management-node
```

Create a new private token outside shared workspaces with 64 hexadecimal characters, using restrictive file permissions. The existing companion setup creates this file; retain it across manager restarts. Existing companion enrollment can reuse its existing private token file. Never put it in a command argument, discovery record, or shared project. The process refuses to run without a configured authenticated endpoint and exits if its loopback port cannot bind. Only one desktop or manager can own a data directory at a time.

The backend binds to `127.0.0.1`. Use the [existing companion HTTPS proxy and pairing flow](../Mobile/README.md) for another device. Configure the proxy to forward `/companion`, `/api`, and static assets to the loopback port; keep the proxy configuration and certificate storage in persistent private paths. The saved `companion.json` can select `proxy_config` and `proxy_executable` as described there. Discovery does not authenticate a peer. The API retains its action allowlist, bearer authorization, exact Origin checks, and request size limits, including bounded request admission and the existing chunked transfer route. WebSocket upgrades are rejected; the companion uses its existing HTTP polling transport.

All six provider implementations remain available. Local CLI processes run on the manager host; configured SSH execution hosts use their existing authenticated runner protocol. Workspaces refer to files on the execution host. No automatic file synchronization or session migration is introduced. See [processing-node deployment](../deploy/processing-node/README.md) for an opt-in Docker Compose SSH worker.

`SIGINT` and `SIGTERM` stop the manager through its event loop, save state, stop owned runtimes, and release the data lock. Restart uses the same durable history and provider resume behavior as the desktop.

## Validation

```sh
python3 tests/management_node_integration.py Builds/management/universal_agent_manager.app
```

The integration check runs the actual application with synthetic provider executables and isolated configuration. It covers authenticated state and assets, malformed and oversized requests, forbidden actions, chat dispatch, model selection, transcript updates, permissions, cancellation, the configured provider profiles, data-lock collisions, graceful termination, restart persistence, startup refusals, and WebSocket upgrade rejection. Interactive CEF worker requests use `TID_FILE_USER_BLOCKING`: CEF can defer background work for an arbitrarily long time while an application without a browser is idle.

The restored integration checks have not been run against this consolidated source. Historical branch checks do not establish current macOS or Windows operation; both runtime validation and user acceptance remain pending.

## Opt-in native discovery

After configuring and pairing the existing HTTPS companion proxy, a manager can announce its HTTPS address:

```sh
UAM_DATA_DIR=/path/to/manager-data \
UAM_COMPANION_TOKEN_FILE=/path/to/private-token-file \
UAM_COMPANION_ORIGIN=https://manager.local \
./Builds/universal_agent_manager.app/Contents/MacOS/universal_agent_manager \
  --management-node --advertise-management-node MyManager
```

Names use one DNS label of up to 63 letters, digits, or hyphens. This explicit flag advertises `_uam-manager._tcp.local` through the operating system responder. It publishes the service name, HTTPS hostname, and port. It publishes no TXT payload, credentials, chat metadata, or filesystem paths. It does not create a public listener or configure TLS. The configured hostname and port must already reach your companion HTTPS proxy. Stop the manager to remove the announcement.

```sh
./Builds/universal_agent_manager.app/Contents/MacOS/universal_agent_manager --discover-management-nodes
```

Discovery takes five seconds and prints JSON address hints with `trusted: false`. Copy an origin into the existing companion pairing flow. Verify the server certificate and pair with the private token through that flow. A discovered address is never automatically connected, trusted, or enrolled; provider execution hosts still require explicit SSH configuration and verified host keys. Results are capped at 32 nodes. Peer TXT data is ignored, hostnames are validated, and JSON encoding prevents a service name from injecting output commands.

macOS uses the native Bonjour DNS-SD API. Windows uses the [Windows 10 DNS-SD API](https://learn.microsoft.com/en-us/windows/win32/api/windns/nf-windns-dnsservicebrowse) with asynchronous cancellation; its implementation compiles and links in the historical branch, but the consolidated source has not been compiled or runtime-verified on Windows. `--discovery-local-only` restricts macOS announcement and browse to its local-only responder interface for integration checks; Windows refuses that mode without sending a query. Explicit discovery output preserves redirected file/pipe handles; when no output handle exists it attaches to the parent console. Normal GUI launches do not attach a console. No LAN discovery validation is claimed for this consolidated source.

```sh
python3 tests/management_node_integration.py Builds/management/universal_agent_manager.app --native-discovery-local-only
./Builds/management/uam_management_discovery_tests --native-local-only
python3 tests/management_discovery_windows_compile.py /path/to/x86_64-w64-mingw32-g++
```
