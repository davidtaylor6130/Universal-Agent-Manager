# Building UAM

Use the repository [agent commands](../AGENTS.md) for ordinary iteration and the [build configurator](../tools/build-configurator/README.md) for a selected feature profile. Reuse one isolated build folder inside Builds; build and test output is not user acceptance. [Build flags](build-flags.md) owns the exclusion agreement. The detailed commands below are developer reference, so they are intentionally longer than feature paragraphs.

## Build from source

Open **Build UAM.command** on macOS or **Build UAM.bat** on Windows for the browser configurator. It checks prerequisites, selects providers and optional features, builds into a separate folder inside `Builds/`, and offers Launch after success. [Configurator instructions and feature flags](../tools/build-configurator/README.md).

Requirements:

- CMake 3.20 or newer
- A C++20 compiler
- Node.js 20 and npm
- Internet access during the first configure so CMake can fetch CEF and `nlohmann/json`
- Xcode command-line tools on macOS, or MSVC Build Tools on Windows
- The provider CLIs you intend to run: `gemini`, `codex`, `claude`, `opencode`, `copilot`, or `agy`

CMake requires build directories inside `Builds/`. CLion's default `cmake-build-*` directories are also accepted.

### Frontend

```bash
npm --prefix UI-V2 ci
npm --prefix UI-V2 run test
npm --prefix UI-V2 run build
```

Do not open `UI-V2/dist/index.html` directly. The frontend expects the native CEF bridge.

For the isolated SVN composer service fixtures, configure the same root project with `-DUAM_BUILD_TESTS=ON -DUAM_SVN_BRANCH_TESTS_ONLY=ON -DUAM_PACKAGE_REMOTE_RUNNERS=OFF`, then build `uam_svn_branch_tests` and run CTest. This avoids CEF downloads and provider/runner processes; `tests/svn_branch_integration.py <test-binary>` additionally uses disposable `file://` repositories and a private SVN configuration when `svn` and `svnadmin` are installed. Set `UAM_SVN_BRANCH_TESTS_ONLY=OFF` before using that build folder for normal native targets.

### Local development build

Disable remote-helper packaging when you only need a local development build:

Keep this build folder between edits and rerun only `cmake --build`; native and frontend
inputs are tracked automatically. Run `npm ci` again only when the lockfile changes.
Make/Ninja builds use an installed `ccache` automatically. Agent verification commands,
including focused cases and the parallel full native inventory, are in [AGENTS.md](../AGENTS.md).

```bash
npm --prefix UI-V2 ci
cmake -S . -B Builds/dev \
  -DUAM_BUILD_TESTS=OFF \
  -DUAM_PACKAGE_REMOTE_RUNNERS=OFF
cmake --build Builds/dev --config Release --parallel 4
```

On Windows, initialize MSVC before running the same commands:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
npm --prefix UI-V2 ci
cmake -S . -B Builds\dev -A x64 -DUAM_BUILD_TESTS=OFF -DUAM_PACKAGE_REMOTE_RUNNERS=OFF
cmake --build Builds\dev --config Release --parallel 4
```

### Complete macOS package

`build.sh` builds the desktop app and cross-builds every remote helper. It requires [Zig](https://ziglang.org/) plus MinGW-w64:

```bash
brew install zig mingw-w64
./build.sh
```

The interactive script lets you exclude provider runtimes, but at least one provider must remain enabled.

Linux helper compilation, static archive creation and archive indexing all use Zig;
macOS `ar` and `ranlib` cannot supply the ELF archive index. The checked-in wrappers
under `cmake/` pass arguments directly to `zig ar` and `zig ranlib`. Set `UAM_ZIG`
to a specific Zig executable when needed. Windows helpers retain MinGW-w64's tools.

### Release-compatible direct build

Full app packaging fails closed unless these helper artifacts, checksums, and version files exist:

```text
Builds/remote-artifacts/
  linux-arm64/
  linux-x86_64/
  windows-x86_64/
```

After populating that directory, build with:

```bash
npm --prefix UI-V2 ci
cmake -S . -B Builds \
  -DUAM_REMOTE_RUNNER_ARTIFACT_DIR="$PWD/Builds/remote-artifacts"
cmake --build Builds --config Release --parallel 4
```

The GitHub release workflow builds these helpers independently, verifies their reported version, and injects all three into each desktop package.

On Windows, `./build.ps1` acquires the three matching `UAM-runner-<platform>-<architecture>.zip` release assets before configuring the app. Existing artifacts are reused only after their SHA-256, exact application version, protocol and source fingerprint pass validation. Releases publish these small archives separately from desktop packages. Local builds and CI use `scripts/acquire_remote_runners.ps1` and the same four-file contract.

For an offline or unreleased source version, download that revision's three runner artifacts from CI. Put the runner, `uam-runner.sha256`, `uam-runner.version` and `uam-runner.manifest.json` files in each directory above, or provide the standalone ZIPs with `./build.ps1 -RemoteRunnerArchiveDirectory C:\UAM-helpers -Offline`. Acquisition fails before CMake and names the missing platform or mismatched version. It never substitutes an older helper.


## Tests and release gates

Run the local test suite without requiring packaged remote helpers:

```bash
npm --prefix UI-V2 ci
npm --prefix UI-V2 run test
npm --prefix UI-V2 run build

cmake -S . -B Builds/tests \
  -DUAM_BUILD_TESTS=ON \
  -DUAM_PACKAGE_REMOTE_RUNNERS=OFF
cmake --build Builds/tests --config Debug --parallel 4
ctest --test-dir Builds/tests -C Debug --output-on-failure
```

Pull-request CI also runs:

- A locked CycloneDX frontend SBOM check.
- Frontend tests and the production frontend build.
- Linux x86-64, Linux ARM64, and Windows x86-64 helper builds with version smoke tests.
- Native Release builds and CTest on macOS ARM, macOS Intel, and Windows x64.
- Packaged-app smoke tests that validate the bundled UI, CEF resources, helper checksums, launch, and shutdown.
- Separate Computer Use and provider CLI contract checks.

Release tags must match the numeric version in `UI-V2/package.json`, optionally followed by a prerelease suffix (for example `v5.0.0-alpha.1` for version `5.0.0`); suffixed tags are published as GitHub prereleases so they never replace the latest release. The release job gates native core cases like CI, through one retry and `tests/ci_quarantine.txt`. The release workflow publishes the two macOS archives, the Windows archive, and the frontend SBOM.
