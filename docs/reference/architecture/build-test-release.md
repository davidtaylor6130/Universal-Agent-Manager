# Build, test and release

> Engineering reference updated from `uam/v5-improvements` (`e28ba717`, 2026-10-16). This records implementation detail and historical verification, not full feature acceptance. Start with the [feature guide](../../README.md); its agreements and [documented corrections](../../documentation-review.md) take precedence over stale claims here.

The repository builds a C++ native app and a React frontend. CMake configures the native target, invokes the UI build and packages the resulting assets. The source-tree `UI-V2/dist` is not the packaged output.

## Flow and invariants

Use `npm --prefix UI-V2 ci`, then the requested UI checks. Configure CMake only inside `Builds/` (or an accepted CLion `cmake-build-*` directory); package outputs live under the build tree. Native tests use the custom framework in `tests/core_tests.cpp` plus focused test sources registered by CMake. UI tests use Vitest. Keep build outputs and generated frontend files out of source changes.

Reuse the task's build folder and dependencies between edits. `cmake/ui_build.cmake` tracks frontend inputs and emits `index.html` only when they change; native-only builds reuse the existing frontend. Its diagnostic timestamp is generated during an actual UI build, so unrelated CMake regeneration does not invalidate it. `cmake/package_inputs.cmake` keeps resource-only and runner-only changes connected to desktop copying and signing. UI, dependency-lock, feature and presentation-version changes still invalidate the frontend. A failed UI build removes its entry point so the next build retries.

Make/Ninja builds automatically use an installed `ccache` unless a compiler launcher was explicitly set. The changing remote-runner source fingerprint lives in a generated header included by `runner_main.cpp` alone; other helper objects retain reusable compile commands. Release artifact validation and checksums remain enabled on packaging builds.

The macOS packaging script sets Linux cross-build `CMAKE_AR` and `CMAKE_RANLIB`,
including their C/C++ compiler-specific variants, to the `cmake/zig-ar.sh` and
`cmake/zig-ranlib.sh` wrappers. They forward each argument unchanged to Zig's target-aware
archive tools, using `UAM_ZIG` when supplied and `zig` on PATH otherwise. Reconfiguring
an existing helper folder also loads `cmake/zig-archive-tools.cmake` through
`CMAKE_PROJECT_INCLUDE` after cached compiler metadata, so cached host-tool variables
cannot silently override the selected archive tools. This relinks static archives without cleaning
or recompiling unchanged source objects. Host macOS archive indexing is not compatible
with the Linux ELF objects; Windows cross-builds continue using MinGW-w64's archive tools.

For focused native checks, build `uam_core_tests` and pass exact names to `scripts/run_native_tests.py`; `--list-tests` on the binary reports the compiled inventory. The `uam_check_core` target runs that complete inventory in batches across two separate processes, validating that every requested case passes exactly once. Processes isolate environment variables and singleton state; individual timing budgets and assertions remain unchanged. `UAM_TEST_TIMINGS=1` reports per-case durations on direct binary runs. The original serial `ctest` registration remains available. When using `uam_check_core` as the final core gate, exclude `uam_core_tests` from the subsequent CTest run to avoid executing the same inventory twice. Build all required test targets before running the remaining gates. Full commands are in [AGENTS.md](../../../AGENTS.md).

Unrelated CEF handler fixtures use `tests/cef_handler_fixture.h` to construct the real handler through its private test seam before either usage-history or account-allowance service is created. Configuring a private provider command or avoiding allowance actions does not isolate the ordinary constructor, which starts background readers. These fixtures exclude usage actions; usage tests require explicit isolated services. The public application constructor retains its existing startup behavior.

The supported release slice has six provider implementations and the React/CEF, ACP, terminal, workspace and memory paths. Do not add excluded engines or remove a provider without explicit direction. Update the root V5 log in the same implementation commit and retain review/user acceptance flags until they are actually complete.

## Ownership and checks

- CMake and packaging: `CMakeLists.txt`, `cmake/`, `Builds/`.
- UI source and tests: `UI-V2/src/`, `UI-V2/package.json`.
- Native tests: `tests/`, `CMakeLists.txt`.
- Repository gates and change status: `AGENTS.md`, `V5 Improvement Change log.md`.

Commands and build-directory restriction are listed in [AGENTS.md](../../../AGENTS.md). Budgets are test-specific; do not treat an individual interaction assertion as a general startup or release performance target.

## App icon

`scripts/make_app_icon_art.py` draws the flat artwork (`assets/app_icon.png`) and `scripts/compose_macos_icon.swift` turns it into the approved icon tile (`assets/app_icon_macos.png`) and `app_icon.icns`. `scripts/make_windows_icon.py` generates `assets/app_icon.ico` from that tile with the 16, 20, 24, 32, 40, 48, 64, 96, 128 and 256 pixel sizes; each PNG entry records the tile's SHA-256. `src/uam.rc` embeds the `.ico` as `IDI_APP_ICON`, and the CEF window and taskbar icon load the same tile copied beside the executable. After changing the artwork, run on macOS with Pillow installed: `python3 scripts/make_app_icon_art.py`, `swift scripts/compose_macos_icon.swift assets/app_icon.png assets`, then `python3 scripts/make_windows_icon.py`.

CTest `uam_app_icon_resource` (`tests/app_icon_resource_test.py`) fails when the `.ico` misses a size or was not generated from the current tile. On Windows it also reads the built executable's icon resource and requires the same images. Ninja tracks only `#include` in resource scripts, so CMake names the `.ico` as an object dependency of `src/uam.rc`; without it, incremental Ninja builds kept the previous icon. Explorer may still show a cached icon for an existing path until its icon cache refreshes.

## Local macOS signing

Set `-DUAM_MACOS_SIGNING_IDENTITY=<40-character identity SHA-1>` to sign the app and its owned helpers with an available certificate from `security find-identity -v -p codesigning`. CMake rejects a missing identity and stops on signing failures. The default `-` keeps ad-hoc signing available for machines without a certificate. A stable certificate requirement lets macOS retain privacy grants across rebuilds after the signed app receives its initial grant; changing from ad-hoc signing can require consent again. Existing entitlements are preserved and a valid vendor-signed CEF framework is verified without being resigned. An unsigned CEF framework, as shipped for Intel, is signed before the helpers and main executable because codesign rejects a bundle executable while nested code is unsigned.

## Separate manual test bundles on macOS

A separately named copy must also have a distinct bundle identifier and a distinct `UAM_DATA_DIR` when it needs to run alongside the normal application. The existing data-root lock remains enforced: a second process targeting the regular application's live database exits before creating a window. A renamed bundle alone does not isolate that database.

For private manual-test packaging, set a test-only `CFBundleIdentifier` and `CFBundleDisplayName`, and put the absolute test data-root path in the bundle's `LSEnvironment` dictionary. [Apple documents this environment setting for Launch Services launches](https://developer.apple.com/documentation/bundleresources/information-property-list/lsenvironment); direct command-line runs need the environment set explicitly. Re-sign the outer test bundle while preserving entitlements and verify its nested signatures. Keep the production build and normal application bundle unchanged.

A history-dependent manual test needs a populated profile: copying only `settings.txt`, `folders.txt` and `providers.txt` produces empty workspace counts and does not establish sidebar acceptance. With the test app closed, take a per-file consistent snapshot of saved chat JSON into its private data root, retain folder membership, titles, pins and complete transcript messages, and verify the written transcripts against the source snapshot. Existing snapshot chats must use the supported `imported_read_only` flag; remove queue payloads, remote recovery/control capabilities, temporary cleanup ownership, provider handoff state, agent-run and goal execution state, and disable memory/control on those copies. Preserve the nonempty `agent_run_id` and `goal_owner_chat_id` visibility markers for internal chats while retaining `imported_read_only` and omitting the run ledger; clearing those markers incorrectly exposes hidden transcripts in the sidebar. Never copy live queue outboxes, runtime directories or agent-run journals. Native session identifiers may remain as read-only import ownership metadata. Regenerate summary caches from the sanitized snapshot with matching source sizes and message counts rather than mixing stale source caches into the test profile. This is a saved-history snapshot, not a global transaction against the running production database; new test chats and imports remain independent. Verify a real Launch Services launch, the private data-root log entry, populated sidebar counts, selection of an existing conversation with rendered history, and the connected backend before calling the test bundle ready. Static signature and resource checks alone do not establish manual-test readiness. Re-signing with the test bundle identity changes the executable signature bytes; compare unsigned scratch copies to establish unchanged executable code.

The opt-in remote OpenCode first-message case requires explicit alias, workspace, executable and resource-directory inputs plus `UAM_REAL_REMOTE_OPENCODE_SAVED_MODEL` and `UAM_REAL_REMOTE_OPENCODE_EXPECTED_MODEL`; the latter identify the stale saved spelling and its advertised canonical model. It has no operator-specific model default and remains unexecuted when any required input is absent.
