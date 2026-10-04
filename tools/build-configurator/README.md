# Build UAM

Open **Build UAM.command** on macOS or **Build UAM.bat** on Windows. Install Python 3 from python.org if the launcher requests it. A local browser page lets you select providers, optional features and a new output folder inside `Builds/`.

Click **Check prerequisites**. Install any missing tools named by the page: Node.js LTS, CMake, Apple command line tools on macOS or Visual Studio Build Tools with **Desktop development with C++** on Windows. The configurator finds installed Visual Studio tools and initializes their environment itself. An internet connection is needed to download locked frontend packages, CEF and zlib when the toolchain does not already provide it. CMake dependency sources and binaries remain inside the selected build folder. Frontend dependencies are installed in the selected source folder’s `UI-V2/node_modules`.

Click **Build UAM**. Progress and the latest compiler output appear on the page. The full log is `<build-folder>/configurator-build.log`. After success, use **Launch UAM** or **Open build folder**. Launch starts this artifact directly with its own data folder at `<build-folder>/data`, so it can run beside an installed UAM. Keep the launcher window open until the build finishes. Builds never install or update provider CLIs; selected providers still need their own CLI installation to run chats.

The configurator protects existing build directories. It reuses only output marked with `.uam-configurator`. Choose another folder to keep separate configurations. macOS builds target the current Mac's architecture. Windows builds use the installed Visual Studio desktop toolchain. Existing developer commands work unchanged.

## Optional feature flags

All feature flags default to **ON**. All five provider flags retain their existing **ON** defaults. Set at least one `UAM_ENABLE_RUNTIME_<GEMINI|CODEX|CLAUDE|OPENCODE|COPILOT>_CLI=ON`.

| CMake flag | ON includes | OFF excludes |
| --- | --- | --- |
| `UAM_ENABLE_COMPUTER_USE` | Capture/input platform code, MCP server, macOS Computer Use app, cursor/policy resources and UI controls | Native capture/input dependencies and companion artifacts; MCP invocation and controls are unavailable |
| `UAM_ENABLE_SSH` | Remote connection support and the current platform's SSH helper | SSH connection admission, remote host controls and bundled remote helper artifacts |
| `UAM_ENABLE_MOBILE_COMPANION` | Companion HTTP server, mobile frontend and optional Caddy HTTPS resources | Companion server source, mobile frontend chunk, phone access controls and HTTPS resources |

Core configuration/history compatibility code remains compiled so older settings and chats remain readable. A disabled feature cannot be enabled through saved settings or bridge requests. Rebuilding with a feature OFF removes its old generated bundle artifacts.

`UAM_PACKAGE_REMOTE_RUNNERS=ON` validates and packages helper artifacts for every supported remote platform. It requires SSH ON and matching files in `UAM_REMOTE_RUNNER_ARTIFACT_DIR`. The configurator uses OFF, which packages only the helper built for the current platform when SSH is enabled. Add cross-platform helper artifacts through the documented developer/release pipeline. `UAM_RUNNER_ONLY=ON` also requires SSH ON.

Example local build with only Codex and no optional features:

```sh
cmake -S . -B Builds/minimal -DUAM_PACKAGE_REMOTE_RUNNERS=OFF -DUAM_ENABLE_COMPUTER_USE=OFF -DUAM_ENABLE_SSH=OFF -DUAM_ENABLE_MOBILE_COMPANION=OFF -DUAM_ENABLE_RUNTIME_GEMINI_CLI=OFF -DUAM_ENABLE_RUNTIME_CLAUDE_CLI=OFF -DUAM_ENABLE_RUNTIME_OPENCODE_CLI=OFF -DUAM_ENABLE_RUNTIME_COPILOT_CLI=OFF
cmake --build Builds/minimal --config Release --target universal_agent_manager
```

The main CI workflow builds and tests the application with all optional features and all five providers enabled. The configurator workflow runs its focused unit tests without additional application builds. Configurator regressions run with `python3 tools/build-configurator/test_configurator.py`.

Local configurator, `build.sh` and `build.ps1` builds reserve one `X.Y.Z-alpha-Q` version per requested build. Linked worktrees share a locked counter in the main checkout’s ignored `Builds/local-versions/` folder. The release version and remote runner protocol version stay unchanged. To reserve a version for separately packaged local artifacts without starting a build:

```sh
cmake -DUAM_SOURCE_DIR="/absolute/path/to/source" -P cmake/local_build_version.cmake
```

Pass the returned version as `-DUAM_LOCAL_BUILD_VERSION=X.Y.Z-alpha-Q` when configuring to label the macOS bundle and frontend build identifier. A corrupt counter stops allocation instead of recycling a number. Allocator regressions run with `python3 tools/build-configurator/test_local_build_version.py`.
