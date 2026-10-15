# Build, test and release

The repository builds a C++ native app and a React frontend. CMake configures the native target, invokes the UI build and packages the resulting assets. The source-tree `UI-V2/dist` is not the packaged output.

## Flow and invariants

Use `npm --prefix UI-V2 ci`, then the requested UI checks. Configure CMake only inside `Builds/` (or an accepted CLion `cmake-build-*` directory); package outputs live under the build tree. Native tests use the custom framework in `tests/core_tests.cpp` plus focused test sources registered by CMake. UI tests use Vitest. Keep build outputs and generated frontend files out of source changes.

The supported release slice has six provider implementations and the React/CEF, ACP, terminal, workspace and memory paths. Do not add excluded engines or remove a provider without explicit direction. Update the root V5 log in the same implementation commit and retain review/user acceptance flags until they are actually complete.

## Ownership and checks

- CMake and packaging: `CMakeLists.txt`, `cmake/`, `Builds/`.
- UI source and tests: `UI-V2/src/`, `UI-V2/package.json`.
- Native tests: `tests/`, `CMakeLists.txt`.
- Repository gates and change status: `AGENTS.md`, `V5 Improvement Change log.md`.

Commands and build-directory restriction are listed in [AGENTS.md](../../AGENTS.md). Budgets are test-specific; do not treat an individual interaction assertion as a general startup or release performance target.

## Local macOS signing

Set `-DUAM_MACOS_SIGNING_IDENTITY=<40-character identity SHA-1>` to sign the app and its owned helpers with an available certificate from `security find-identity -v -p codesigning`. CMake rejects a missing identity and stops on signing failures. The default `-` keeps ad-hoc signing available for machines without a certificate. A stable certificate requirement lets macOS retain privacy grants across rebuilds after the signed app receives its initial grant; changing from ad-hoc signing can require consent again. Existing entitlements are preserved and a valid vendor-signed CEF framework is verified without being resigned.
