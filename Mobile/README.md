# UAM companion

The companion reuses `UI-V2`: the desktop theme, chat transcript, composer and workspace tree. Activity opens first, with Chats available in the bottom navigation.

Configured installations start phone access with the normal desktop app. Settings → Defaults → Phone Access contains the enable switch, connection URL and Copy token button. Restart UAM after changing the switch. The phone uses the same data root and chats as the desktop.

Startup reads `<data-root>/companion.json` with `enabled`, `token_file`, `port`, `origin`, and optional `proxy_config`. Token files must contain 64 hexadecimal characters. Invalid configuration disables the listener. The listener binds to loopback; the bundled Caddy process provides LAN HTTPS and is stopped with UAM. Its JSON configuration and certificate storage must use persistent paths outside the app bundle so alpha replacements preserve pairing. The Mac build bundles Caddy when available at configure time, including its license. A configured proxy requires that bundled executable or an explicit `proxy_executable` path.

Phone transport cannot read the connection token or change phone settings. Those actions are restricted to the local desktop bridge. Initial LAN address and certificate trust setup are still machine-specific; the app does not automatically grant iPhone certificate trust.

For isolated development previews, environment configuration still overrides the saved configuration. Build the React frontend and desktop app using the root build instructions. Quit the preview, then run:

```sh
python3 Mobile/start-desktop.py --desktop Builds/audit-gui/universal_agent_manager.app
```

Open http://127.0.0.1:58948/companion on the Mac. Paste the token from `~/Library/Application Support/UAM Companion/token`. The token stays in browser local storage until Logout. The launcher refuses to start alongside another desktop instance and does not replace the installed app.

The server currently listens on loopback only. iPhone access still requires a secure connection to the Mac; this is not an installed iPhone app. No Apple membership is needed for this Mac browser preview.

For a LAN HTTPS reverse proxy, launch with `--origin https://YOUR-LAN-HOST:PORT`. The origin is matched exactly; arbitrary forwarded headers cannot authorize browser origins. The backend stays loopback-only. Your proxy must forward both `/companion` and `/api`, plus static assets, to port 58948.

On iPhone, open the HTTPS companion URL in Safari and choose Share → Add to Home Screen. This reuses the React app in a standalone window. Pair inside the installed app. Do not expose the HTTP backend directly on the LAN. A trusted HTTPS endpoint is still required before phone use.

To run the LAN endpoint on this Mac (with Caddy installed):

```sh
python3 Mobile/start-desktop.py --desktop Builds/audit-gui/universal_agent_manager.app --lan-host 192.168.0.167
```

Use the Mac's current private IPv4 address. This starts Caddy on HTTPS port 58950 alongside UAM and stops the proxy when UAM exits. A proxy failure does not terminate UAM. It does not change Tailscale, enable an exit node, create a public tunnel, or register a login service. The iPhone must have a route to this LAN address, either locally or through an advertised subnet route.

The private CA certificate is printed by the launcher. Install and trust that certificate on the iPhone before pairing; do not transfer its private key. Direct LAN reachability and device trust must be verified on the phone.
