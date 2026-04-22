# Security and Enterprise Notes

This document captures the current security posture for the Gemini CLI and Codex CLI release slice of Universal Agent Manager. It is not a formal penetration test or compliance attestation; it is the repository-level review checklist for enterprise readiness work.

## Trust Model

- UAM is a local desktop application. It does not expose a network service.
- The React UI is loaded from the bundled `UI-V2/dist/index.html` file inside CEF.
- The native bridge is privileged and is restricted to the trusted bundled UI URL and the main frame.
- Provider CLI processes run with the current user's operating-system permissions.
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
- Terminal process launch uses argv-style execution for interactive provider sessions rather than concatenating untrusted prompt text into a shell command.
- Windows CLI child processes are attached to a kill-on-close job object.
- macOS terminal processes are launched in their own process group and are signaled on stop.
- Local chat writes use temporary files and backups for crash-tolerant persistence.
- Native Gemini history reads cap parsed session file size and message count through platform limits.
- Memory extraction rejects supported entries that appear to contain obvious secrets before writing `.md` memory files.

## Enterprise Risks and Operating Assumptions

- Provider CLIs are part of the trusted computing base. UAM can display and send input to Gemini CLI and Codex CLI, and each provider CLI can access files that the current user can access from the selected workspace.
- Approval and sandbox behavior is controlled by the provider CLI. Enterprise deployments should manage provider CLI versions, authentication, and policy outside UAM.
- User-provided provider flags and command templates are powerful configuration. Restrict write access to the UAM data root and settings files in managed environments.
- Local chat history and memory files may contain sensitive prompts, terminal output, file paths, tool-call details, and model responses. Store the data root and workspaces on enterprise-managed encrypted storage.
- UAM currently has no built-in authentication, role-based access control, centralized audit log, DLP enforcement, or remote policy management.
- CEF and npm dependencies must be kept patched as part of release management.

## Current Hardening Decisions

- Chromium web security is currently disabled with `disable-web-security`, and `allow-file-access-from-files` is enabled, because the bundled React app is loaded from `file://` and may need same-file-origin resource access. This should be revisited if the UI moves to a custom CEF scheme.
- JavaScript clipboard access is enabled in CEF. App copy flows prefer explicit app logic and the native clipboard bridge where possible.
- The privileged bridge validates the source URL again in the browser process before dispatching actions.

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

3. Run dependency checks:

   ```bash
   npm --prefix UI-V2 audit
   ```

4. Confirm the packaged CEF version is current for the release date and supported on both Windows and macOS.
5. Confirm the packaged app does not include `UI-V2/node_modules`, `UI-V2/dist` as checked-in source, local data roots, or build directories.
6. Validate that new bridge actions preserve the trusted-main-frame gate and do not accept arbitrary file paths for destructive operations without a user-mediated control.
7. Validate provider CLI policy with enterprise security owners, including workspace access, command approval behavior, authentication, sandboxing, and logging requirements.
8. Validate memory settings and storage paths for the deployment profile, including whether workspace-local memory is permitted.

## Known Gaps

- No formal SBOM generation is configured.
- No automated C++ static analysis or secret scanning is configured in this repository.
- CMake `FetchContent` downloads third-party source/binaries without pinned URL hashes.
- Local macOS packaging uses ad hoc signing before the final bundle verification; enterprise distribution should use the organization's signing, notarization, and deployment process.
- Windows packaging/signing policy is not documented here yet.
- `disable-web-security` and `allow-file-access-from-files` are active CEF command-line switches.
