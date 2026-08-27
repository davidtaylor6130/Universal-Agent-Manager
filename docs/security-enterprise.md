# Security and Enterprise Notes

This document captures the current security posture for the Gemini CLI, Codex CLI, Claude Code CLI, OpenCode CLI, and GitHub Copilot CLI release slice of Universal Agent Manager. It is not a formal penetration test or compliance attestation; it is the repository-level review checklist for enterprise readiness work.

## Trust Model

- UAM is a local desktop application. It does not expose a network service.
- The React UI is loaded from bundled `UI-V2/dist` resources through the private `uam://app/` CEF scheme.
- The native bridge is privileged and is restricted to the trusted bundled UI URL and the main frame.
- Provider CLI processes run with the current user's operating-system permissions.
- ACP, terminal, and text-worker launches blank conventional direct-provider API keys owned by a
  different selected provider. OpenCode remains multi-provider by design; operators can set
  `UAM_PRESERVE_PROVIDER_CHILD_SECRETS=1` when compatibility requires the legacy inherited behavior.
- Workspace folders are user-selected local directories and are used as provider CLI working directories.
- Chat metadata, settings, global memory, and optional backups are stored locally under the resolved data root.
- Workspace-local memory files are stored under `<workspace>/.codex/memories/`.

## Security Controls Present

- CEF bridge requests are rejected unless they originate from the trusted bundled UI and the main frame.
- Renderer-side `window.cefQuery` injection is restricted to the trusted bundled UI URL.
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

## Enterprise Risks and Operating Assumptions

- Provider CLIs are part of the trusted computing base. Each provider CLI can access files allowed by its own sandbox and the current operating-system account.
- Structured sessions start providers in their restrictive permission mode and route permission requests through UAM. UAM modes decide whether to leave a request for the user, auto-approve it, or run bounded AI review. Plan remains a hard read-only ceiling.
- Terminal fallback is an opaque provider-controlled PTY. UAM does not claim structured permission mediation there and refuses legacy provider-native bypass flags.
- User-provided provider flags and command templates are powerful configuration. Restrict write access to the UAM data root and settings files in managed environments.
- Local chat history and memory files may contain sensitive prompts, terminal output, file paths, tool-call details, and model responses. Store the data root and workspaces on enterprise-managed encrypted storage.
- UAM currently has no built-in authentication, role-based access control, centralized audit log, DLP enforcement, or remote policy management.
- CEF and npm dependencies must be kept patched as part of release management.

## Current Hardening Decisions

- Chromium web security remains enabled. The UI is served only from the private standard/secure `uam://app/` origin, and resource resolution rejects traversal, links/reparse points, missing files, and paths outside the bundled UI root.
- JavaScript clipboard access is enabled in CEF. App copy flows prefer explicit app logic and the bounded native clipboard bridge where needed.
- The privileged bridge validates the exact trusted origin and main frame again in the browser process before dispatching actions.
- Downloaded nlohmann/json and CEF archives are verified against repository-pinned SHA-256 hashes during configuration.
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
   cmake -S . -B Builds/tests -DUAM_BUILD_TESTS=ON
   cmake --build Builds/tests --config Debug
   ctest --test-dir Builds/tests -C Debug --output-on-failure
   ```

3. Generate locked dependency evidence and, where policy permits contacting npm's advisory service,
   run the current advisory check:

   ```bash
   npm --prefix UI-V2 sbom --package-lock-only --sbom-format cyclonedx > UAM-frontend.cdx.json
   npm --prefix UI-V2 audit
   ```

4. Confirm the packaged CEF version is current for the release date and supported on both Windows and macOS.
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
- CEF sandboxing is currently disabled. The macOS build directly links the framework and reuses the
  main executable in helper bundles; CEF requires dynamic framework loading plus helper-side sandbox
  initialization. Windows additionally requires its platform sandbox library and sandbox-info
  plumbing. Toggling `no_sandbox` alone would create a false control and can break startup, so this
  remains a packaging-architecture change that must be proven by both native CI matrices before it is
  enabled.
