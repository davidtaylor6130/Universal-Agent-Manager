# Build, test and release

The repository builds a C++ native app and a React frontend. CMake configures the native target, invokes the UI build and packages the resulting assets. The source-tree `UI-V2/dist` is not the packaged output.

## Flow and invariants

Use `npm --prefix UI-V2 ci`, then the requested UI checks. Configure CMake only inside `Builds/` (or an accepted CLion `cmake-build-*` directory); package outputs live under the build tree. Native tests use the custom framework in `tests/core_tests.cpp` plus focused test sources registered by CMake. UI tests use Vitest. Keep build outputs and generated frontend files out of source changes.

Reuse the task's build folder and dependencies between edits. `cmake/ui_build.cmake` tracks frontend inputs and emits `index.html` only when they change; native-only builds reuse the existing frontend. Its diagnostic timestamp is generated during an actual UI build, so unrelated CMake regeneration does not invalidate it. `cmake/package_inputs.cmake` keeps resource-only and runner-only changes connected to desktop copying and signing. UI, dependency-lock, feature and presentation-version changes still invalidate the frontend. A failed UI build removes its entry point so the next build retries.

Make/Ninja builds automatically use an installed `ccache` unless a compiler launcher was explicitly set. The changing remote-runner source fingerprint lives in a generated header included by `runner_main.cpp` alone; other helper objects retain reusable compile commands. Release artifact validation and checksums remain enabled on packaging builds.

For focused native checks, build `uam_core_tests` and pass exact names to `scripts/run_native_tests.py`; `--list-tests` on the binary reports the compiled inventory. The `uam_check_core` target runs that complete inventory in batches across two separate processes, validating that every requested case passes exactly once. Processes isolate environment variables and singleton state; individual timing budgets and assertions remain unchanged. `UAM_TEST_TIMINGS=1` reports per-case durations on direct binary runs. The original serial `ctest` registration remains available. When using `uam_check_core` as the final core gate, exclude `uam_core_tests` from the subsequent CTest run to avoid executing the same inventory twice. Build all required test targets before running the remaining gates. Full commands are in [AGENTS.md](../../AGENTS.md).

The supported release slice has six provider implementations and the React/CEF, ACP, terminal, workspace and memory paths. Do not add excluded engines or remove a provider without explicit direction. Update the root V5 log in the same implementation commit and retain review/user acceptance flags until they are actually complete.

## Ownership and checks

- CMake and packaging: `CMakeLists.txt`, `cmake/`, `Builds/`.
- UI source and tests: `UI-V2/src/`, `UI-V2/package.json`.
- Native tests: `tests/`, `CMakeLists.txt`.
- Repository gates and change status: `AGENTS.md`, `V5 Improvement Change log.md`.

Commands and build-directory restriction are listed in [AGENTS.md](../../AGENTS.md). Budgets are test-specific; do not treat an individual interaction assertion as a general startup or release performance target.

## Local macOS signing

Set `-DUAM_MACOS_SIGNING_IDENTITY=<40-character identity SHA-1>` to sign the app and its owned helpers with an available certificate from `security find-identity -v -p codesigning`. CMake rejects a missing identity and stops on signing failures. The default `-` keeps ad-hoc signing available for machines without a certificate. A stable certificate requirement lets macOS retain privacy grants across rebuilds after the signed app receives its initial grant; changing from ad-hoc signing can require consent again. Existing entitlements are preserved and a valid vendor-signed CEF framework is verified without being resigned.

## Separate manual test bundles on macOS

A separately named copy must also have a distinct bundle identifier and a distinct `UAM_DATA_DIR` when it needs to run alongside the normal application. The existing data-root lock remains enforced: a second process targeting the regular application's live database exits before creating a window. A renamed bundle alone does not isolate that database.

For private manual-test packaging, set a test-only `CFBundleIdentifier` and `CFBundleDisplayName`, and put the absolute test data-root path in the bundle's `LSEnvironment` dictionary. [Apple documents this environment setting for Launch Services launches](https://developer.apple.com/documentation/bundleresources/information-property-list/lsenvironment); direct command-line runs need the environment set explicitly. Re-sign the outer test bundle while preserving entitlements and verify its nested signatures. Keep the production build and normal application bundle unchanged.

An import-test profile can start with copies of `settings.txt`, `folders.txt` and `providers.txt`; keep its chat database separate and do not clone active queue journals, agent-run state or runtime ownership. Verify a real Launch Services launch, the actual private data-root log entry, a visible app window and the connected backend before calling the test bundle ready. Static signature and resource checks alone do not establish launch acceptance. Re-signing with the test bundle identity changes the executable signature bytes; compare unsigned scratch copies to establish unchanged executable code.
