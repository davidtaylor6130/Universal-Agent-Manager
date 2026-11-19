# Security and Enterprise Notes

> Engineering reference retained from the trial base (`3a7c8477`, 2026-10-16). This records implementation detail and historical verification, not full feature acceptance. Start with the [feature guide](../README.md); its agreements and [documented corrections](../documentation-review.md) take precedence over stale claims here.

This document captures the current security posture for Universal Agent Manager’s six-provider release slice, including structured Chat and native CLI View for Antigravity. It is not a formal penetration test or compliance attestation; it is the repository-level review checklist for enterprise readiness work.

## Trust Model

- UAM is a local desktop application with optional explicitly enabled Phone Access, which publishes a restricted authenticated browser interface. Provider and SSH connections have their own network boundaries.
- The React UI is loaded from bundled `UI-V2/dist` resources through the private `uam://app/` CEF scheme.
- The native bridge is privileged and is restricted to the trusted bundled UI URL and the main frame.
- Provider CLI processes run with the current user's operating-system permissions.
- ACP, terminal, and text-worker launches blank conventional direct-provider API keys owned by a
  different selected provider. OpenCode remains multi-provider by design; operators can set
  `UAM_PRESERVE_PROVIDER_CHILD_SECRETS=1` when compatibility requires the legacy inherited behavior.
- Workspace folders are user-selected local directories and are used as provider CLI working directories.
- Chat metadata, settings, global memory, and optional backups are stored locally under the resolved data root.
- Workspace-local memory files are stored under `<workspace>/.UAM/`.

## Security Controls Present

- CEF bridge requests are rejected unless they originate from the trusted bundled UI and the main frame.
- Renderer-side `window.cefQuery` injection is restricted to the trusted bundled UI URL.
- On macOS, renderer, GPU and utility processes run inside the Chromium sandbox. The helper bundles
  hold a separate `uam_cef_helper` executable that enters the sandbox before anything else runs, then
  loads the CEF framework at runtime; it links no application or platform services. The main
  executable still links the framework directly and is not sandboxed. Windows and Linux are known gaps
  (see Known Gaps).
- External `http`, `https`, `mailto`, `ftp`, and `tel` navigation is blocked inside CEF and delegated to the operating system browser or handler.
- DevTools and view-source shortcuts are blocked in the embedded UI.
- The browser context menu is reduced to a Copy command when selected text is present.
- Native clipboard writes go through a bridge action with a 1 MiB text limit.
- Native clipboard writes run off the CEF UI thread; the macOS helper has a two-second process deadline and is terminated and reaped on timeout.
- macOS browsing, URL, Finder, editor, and terminal-open actions use AppKit instead of shell or AppleScript child processes.
- Terminal process launch uses argv-style execution for interactive provider sessions rather than concatenating untrusted prompt text into a shell command.
- Windows CLI child processes are attached to a kill-on-close job object.
- macOS terminal processes are launched in their own process group and are signaled on stop.
- Local chat writes use temporary files and backups for crash-tolerant persistence.
- Native Gemini history reads cap parsed session file size and message count through platform limits.
- Memory extraction rejects supported entries that appear to contain obvious secrets before writing `.md` memory files.
- Each UAM control capability rejects a reused request ID among its 256 most recently seen IDs; when the window is full, only the oldest ID is evicted.
- Agent-published [HTML views](../chat/html-views.md) load only after the user chooses Run on the card, remembered per view and exact page. The hidden runtime check stays off on every platform; with the macOS renderer sandboxed it can be reconsidered there later.
- Git commands UAM runs itself, locally or over SSH, are built in `src/common/vcs/git_command.h`, which turns off repository-configured hooks, file-system monitors, external diff programs, text conversion and pagers ([Git](../settings/version-control/git.md)).

## Enterprise Risks and Operating Assumptions

- Provider CLIs are part of the trusted computing base. Each provider CLI can access files allowed by its own sandbox and the current operating-system account.
- Structured permission handling combines native provider modes and UAM requests. AI Review can use native Claude auto or confirmed Codex auto-review; other eligible requests use UAM review, and residual native prompts remain user decisions. Plan and managed read-only ceilings remain in force. Do not claim universal UAM mediation.
- Terminal fallback is an opaque provider-controlled PTY. UAM does not claim structured permission mediation there and refuses legacy provider-native bypass flags.
- User-provided provider flags and command templates are powerful configuration. Restrict write access to the UAM data root and settings files in managed environments.
- Local chat history and memory files may contain sensitive prompts, terminal output, file paths, tool-call details, and model responses. Store the data root and workspaces on enterprise-managed encrypted storage.
- The desktop does not establish enterprise role-based access control, centralized auditing, DLP or remote policy management. Optional phone access has its own authentication boundary; it does not confer unrestricted desktop bridge access.
- CEF and npm dependencies must be kept patched as part of release management.

## Current Hardening Decisions

- Chromium web security remains enabled. The UI is served only from the private standard/secure `uam://app/` origin, and resource resolution rejects traversal, links/reparse points, missing files, and paths outside the bundled UI root.
- JavaScript clipboard access is enabled in CEF. App copy flows prefer explicit app logic and the bounded native clipboard bridge where needed.
- The privileged bridge validates the exact trusted origin and main frame again in the browser process before dispatching actions.
- Downloaded nlohmann/json, CEF and Caddy archives are verified against repository-pinned SHA-256 hashes during configuration.
- Released remote-helper archives carry GitHub build provenance, and `scripts/acquire_remote_runners.ps1` refuses a downloaded or supplied archive that does not verify against the release workflow and its version tag.
- The remote OpenCode server password stays in memory and is never written to chat files, summaries, backups or exported bundles; loading chats removes one saved by an older build.
- CI and release workflows generate and validate a CycloneDX SBOM from the locked frontend dependency graph.
- Exact-artifact smoke jobs compare the packaged React tree with the production build, validate required CEF resources, and launch the isolated macOS and Windows packages.

## Release Checklist

Before an enterprise release:

1. Run frontend tests and build:

   ```bash
   npm --prefix UI-V2 ci
   npm --prefix UI-V2 run test
   npm --prefix UI-V2 run build
   ```

2. Run native tests:

   ```bash
   cmake -S . -B Builds/tests -DCMAKE_BUILD_TYPE=Debug -DUAM_BUILD_TESTS=ON
   cmake --build Builds/tests --config Debug
   ctest --test-dir Builds/tests -C Debug --output-on-failure
   ```

3. Generate locked dependency evidence and, where policy permits contacting npm's advisory service,
   run the current advisory check:

   ```bash
   npm --prefix UI-V2 sbom --package-lock-only --sbom-format cyclonedx > UAM-frontend.cdx.json
   npm --prefix UI-V2 audit
   ```

4. Confirm the packaged CEF version is current for the release date and supported on both Windows and macOS. CEF is pinned per platform in `CMakeLists.txt` with the SHA-256 of each `minimal` archive; read the CEF branch notes and diff `include/` and `cmake/` between the old and new archives when updating.
5. Run the exact-artifact packaged smoke script on macOS and the packaged Windows smoke job; verify the bundled UI matches the production build and signatures/resources are present.
6. Confirm the packaged app does not include `UI-V2/node_modules`, checked-in generated UI output, local data roots, or unrelated build directories.
7. Validate that new bridge actions preserve the trusted-main-frame gate and do not accept arbitrary file paths for destructive operations without a user-mediated control.
8. Validate provider CLI policy with enterprise security owners, including workspace access, command approval behavior, authentication, sandboxing, and logging requirements.
9. Validate memory settings and storage paths for the deployment profile, including whether workspace-local memory is permitted.

## Known Gaps

- The generated CycloneDX document covers the npm lockfile. CEF and nlohmann/json remain represented
  by their repository-pinned versions and SHA-256 hashes rather than a combined native SBOM.
- No automated C++ static analysis or secret scanning is configured in this repository.
- Local macOS packaging uses ad hoc signing before the final bundle verification; enterprise distribution should use the organization's signing, notarization, and deployment process.
- Windows CI packages are unsigned review artifacts. Enterprise/public distribution must apply and
  verify the organization's Authenticode signature after packaging; the repository does not contain
  or request a signing certificate.
- The Chromium process sandbox is enabled only in macOS app bundles. Windows and Linux still run CEF
  with `no_sandbox`. The CEF 156 Windows distribution ships no `cef_sandbox.lib`; its sandbox requires
  the `bootstrap.exe` layout, where the app is rebuilt as a DLL exporting `RunWinMain` and the shipped
  `.exe` is CEF's renamed bootstrap. That changes every Windows entry point (crash supervisor, stdio MCP
  and control servers, discovery CLI) and can only be verified on Windows, so it is a separate change.
  The Windows build sets CEF's `USE_SANDBOX` option off until then, because that option compiles the
  CEF wrapper for the bootstrap layout. Linux desktop builds are not shipped or built in CI, so the
  CEF 156 Linux pin is unverified; their sandbox (SUID `chrome-sandbox` or user namespaces) is out of
  scope until a Linux desktop package is.

## Repository publication

Before publishing a branch, scan every outgoing commit as well as the final tree: deleting a credential in a later commit does not remove earlier exposure. Keep agent handoffs, scratchpads, private evidence archives, generated dependency caches and runtime data outside the tracked publication tree. Retain the feature agreements and V5 acceptance records with generic paths and synthetic connection examples. A clean source snapshot must start from the approved public base without merging private recovery ancestry; keep that history and original evidence in local backups. Repository checks do not authorize a push or establish runtime acceptance.

The frontend lockfile includes patched development tooling; the `postcss-selector-parser` override keeps Tailwind 3 on a corrected parser without changing the design-system major version. Revalidate the frontend tests and generated CSS when updating or removing that override, and audit both development and production dependency scopes.
