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

For the isolated SVN composer service fixtures, configure the same root project with `-DUAM_BUILD_TESTS=ON -DUAM_SVN_BRANCH_TESTS_ONLY=ON -DUAM_PACKAGE_REMOTE_RUNNERS=OFF`, then build `uam_svn_branch_tests` and run CTest. This avoids CEF downloads and provider/runner processes; CTest also registers `uam_svn_branch_integration` (`tests/svn_branch_integration.py`), which uses disposable `file://` repositories and a private SVN configuration, when `svn` and `svnadmin` are installed. Set `UAM_SVN_BRANCH_TESTS_ONLY=OFF` before using that build folder for normal native targets.

### Local development build

Disable remote-helper packaging when you only need a local development build:

Keep this build folder between edits and rerun only `cmake --build`; native and frontend
inputs are tracked automatically. Run `npm ci` again only when the lockfile changes.
Make/Ninja builds use an installed `ccache` automatically; with ccache 4.8+ worktrees that use the same build-folder name share its cache. Agent verification commands,
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

CI runs in tiers so a pull request gets a fast answer and the slow platform checks still run before anything ships.

Every pull request runs:

- A locked CycloneDX frontend SBOM check.
- Frontend tests and the production frontend build.
- Linux x86-64, Linux ARM64, and Windows x86-64 helper builds with version smoke tests.
- The build-flag UI rows and gate self-test when they are relevant.
- A native Release build, CTest and packaged-app smoke test on macOS ARM, and the complete native core inventory on macOS ARM, split across four shard jobs that share one uploaded test binary.
- On Windows x64 only when `scripts/select_native_tests.py` finds a change that can behave differently there (Windows-named files, the platform layer, sources whose old or new text contains a Windows or Apple platform conditional, shared build or test-runner inputs): the native Release build, CTest and smoke test, then four Windows shard jobs that share the built test binary and run the core cases whose test files reach a changed source. A change it cannot map runs every case.
- The Windows Computer Use check for computer-use changes.

Pull requests that change only Markdown, `LICENSE` or diagram files skip the native jobs. The provider CLI contract checks probe the latest published CLIs rather than repository code, so they run after relevant pushes and weekly. Every push to `main` or the V5 integration branch additionally runs the full core suite on macOS Intel and Windows x64, and, when their files change, the 19 native build-flag rows (V5 branch) and the Windows Computer Use check, so a platform-only regression appears on the base branch within one push. A pull request can still request the native build-flag rows with the `build-flags` label.

Native build jobs compile the core test binary while the helper builds are still running, then wait for the helper artifacts (`scripts/wait_for_remote_runners.py`) before building and packaging the app. Native CI jobs restore a per-platform `ccache` saved by the latest push to the base branch; pull requests read it without saving. The cache may grow to 3 GB, so a full Windows build's objects fit without evicting entries mid-build, and entries unused for two days are dropped before a push saves it, and the native dependency caches (CEF and other fetched sources) are likewise saved only by branch pushes; only the small npm cache can still be saved by a pull request that changes the lockfile. After each CI push a cleanup job deletes superseded copies, keeping the newest of each cache family on that branch, so the repository stays under the 10 GB Actions cache limit. Windows CI uses Ninja Multi-Config inside the MSVC developer environment so the cache applies there too, keeping the `Release/` output layout. A newer push to the same pull request cancels its older CI run. CI runs native core cases in two processes, with the wall-clock-budget cases in `tests/ci_serial.txt` running alone afterwards; each shard keeps that order for its share.

Release tags must match the numeric version in `UI-V2/package.json`, optionally followed by a prerelease suffix (for example `v5.0.0-alpha.1` for version `5.0.0`); suffixed tags are published as GitHub prereleases so they never replace the latest release. A release candidate tag containing `-rc` (for example `v5.0.0-rc.1`) is the full release gate: it builds and tests every native case on macOS ARM, macOS Intel and Windows, runs all build-flag rows, and publishes nothing. Before tagging a final release, its commit must have a passed release candidate run. A final tag on that commit rebuilds and packages without repeating the native test runs; without one, the final tag runs them as before. The release job gates native core cases through one retry and `tests/ci_quarantine.txt` like CI, but keeps running every case serially. The release workflow publishes the two macOS archives, the Windows archive, and the frontend SBOM.
