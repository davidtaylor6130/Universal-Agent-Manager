# AGENTS.md

## V5 Master Change Log

Every agent implementing a V5 change must read and update the tracked root document
[`V5 Improvement Change log.md`](<V5 Improvement Change log.md>) in the same commit as
the implementation. Update an existing matching row or append distinct work using
**Group | Type | Change | Summary | Status**. Types are **Performance**, **Graphical**,
**Code Only**, **Bugfix** or **Misc**; Change names have at most five words and Summary
is one factual sentence. Preserve other agents' entries and explicitly distinguish
completed work from partial implementation or unverified acceptance. Keep detailed
working evidence in `.UAM/AINotes/`, but do not substitute ignored notes for this log.

Status flags must reflect acceptance, not just implementation: use **Un-Reviewed**
and **Not User Tested** where those checks are outstanding. Every React/frontend
UI or UX code change, including another agent's work and changes classified as
Bugfix or Performance, requires **Claude Review** until Claude's review is recorded
and its findings resolved. Only David's explicit approval of the specific change
can authorize **Complete**; record the approval reference and satisfy its required
review first. Automated tests, builds, installation, restarts and requests to
continue are not user sign-off. Preserve pending flags when adding follow-up fixes.

Agents must also maintain the **Incomplete Work** section in that root log using
**Group | Type | Change | Summary | Status | Owner**. Add newly discovered fixes or
missing acceptance immediately, deduplicate against existing entries, and claim work
before starting by setting Owner to the exact **UAM chat ID** and Status to **In
Progress** while retaining acceptance flags. Unclaimed work is **Unassigned**; never
invent IDs or substitute display names, PIDs or branch names. Contact an existing
owner using available UAM tools before overlapping work or transferring ownership;
when communication is unavailable, record the gate and avoid conflicting edits.
Record explicit handoffs with the prior and next chat IDs in linked working evidence.
Move completed implementation into the main Changes table while retaining unresolved
acceptance in Incomplete Work; preserve a recorded disposition before removing a
resolved task. A completed implementation is not David's user sign-off.

## Feature documentation agreement

Before changing a feature, read [docs/README.md](docs/README.md), [the documentation rules](docs/documentation-rules.md), its parent introduction and the owning feature page. These describe expected behaviour; code and old engineering notes can contain defects or incomplete work. Do not silently rewrite the agreement to match an implementation gap. Shared UI rules apply logically across features, including primitives, accessibility, interaction and motion.

Every behaviour-changing commit must update the owning feature agreement when the expectation changes, any affected provider capability rows, relevant system relationships and the V5 log in the same commit. Review documentation impact during commit preparation even when the human did not mention docs. If no documentation change is needed, explain why in the commit/PR description rather than making a cosmetic edit. Preserve explicit implementation gaps and pending acceptance; documenting a feature does not implement or approve it.

All six [provider checklists](docs/providers/providers.md#provider-checklists) must account for every shared required capability. Use explicit Implemented, Partial, Not implemented, Needs verification or reasoned Not applicable entries, with provider/UAM delivery recorded. A missing file or row is a documentation defect, not an unsupported-feature convention. Avoid repeating common requirements in provider-specific pages. Run `python3 scripts/check_docs.py` before committing documentation or provider-capability changes; this gate checks documentation integrity, not runtime support or acceptance.

Questions and unresolved contradictions belong in [Documentation review](docs/documentation-review.md). Useful implementation detail lives under [engineering reference](docs/reference/README.md); feature pages should stay around one paragraph per concept, retaining important behavioural boundaries. Source tests, installation records and debug receipts do not belong in feature prose.

## Release Slice

This repository supports Gemini CLI, Codex CLI, OpenCode CLI, Claude Code CLI, GitHub Copilot CLI, and Antigravity CLI, including structured Chat and native CLI View for all six providers. Keep the app focused on React/CEF, structured chat through ACP or native provider protocols, xterm.js terminal sessions, provider save/resume, chat rename/delete/select/create/pin/branch, one-level workspace folders, durable memory files, and multiple concurrent CLI instances on macOS and Windows.

Do not remove any of these six provider implementations. Provider removal requires explicit user direction. RAG engines, templates, VCS panels, local model engines, Dear ImGui, and checked-in frontend build output remain outside this release slice.

## Build Commands

Reuse one isolated build folder for the task. Do not delete it, run `clean`/`--clean-first`,
or reinstall frontend dependencies after every edit. Run `npm --prefix UI-V2 ci` on first
setup or when `package-lock.json` changes. CMake tracks native headers, frontend sources,
configuration, installed-package locks and packaged resources automatically.

For agent work, configure once, then build only the affected target:

```bash
cmake -S . -B Builds/agent -DCMAKE_BUILD_TYPE=Release -DUAM_BUILD_TESTS=ON -DUAM_PACKAGE_REMOTE_RUNNERS=OFF
cmake --build Builds/agent --config Release --target universal_agent_manager --parallel 2
# Native test changes: compile tests without rebuilding the desktop app or frontend.
cmake --build Builds/agent --config Release --target uam_core_tests --parallel 2
# Run relevant exact cases first; names come from --list-tests.
python3 scripts/run_native_tests.py Builds/agent/uam_core_tests ExactCaseName AnotherCaseName
# Final native inventory, then the remaining CTest gates without rerunning the same core cases.
cmake --build Builds/agent --config Release --parallel 2
cmake --build Builds/agent --config Release --target uam_check_core --parallel 2
ctest --test-dir Builds/agent -C Release --output-on-failure -E '^uam_core_tests$'
```

On Visual Studio generators, the test binary is `Builds/agent/Release/uam_core_tests.exe`.
The existing serial `ctest` path remains available. Never shorten timing assertions or
remove cases to speed up verification. Batch connected shared-code edits before the final
full run, and repeat it only after a new change, failure or unresolved concern.

Install optional `ccache` to cache native compiler results (`brew install ccache` on macOS).
CMake uses it automatically with Make/Ninja and respects explicit compiler launchers.
Visual Studio/Xcode generators do not support this launcher; Windows caching needs Ninja
in an initialized MSVC environment. Use separate build folders for different generators.
Do not use `build.sh`/`build.ps1` for ordinary iteration: those prepare complete remote-helper packages.

```bash
npm --prefix UI-V2 ci
npm --prefix UI-V2 run test
npm --prefix UI-V2 run build

cmake -S . -B Builds
cmake --build Builds --config Release
```

Windows must initialize MSVC first:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B Builds
cmake --build Builds --config Release
```

## Critical: Build Directory Restriction

CMake enforces the build directory must be inside `Builds/`. Use one of:

- `cmake -S . -B Builds`
- `cmake -S . -B Builds/<name>`

CLion default `cmake-build-*` directories are also accepted.

## Run

```bash
# macOS
open Builds/universal_agent_manager.app

# Windows
.\Builds\Release\universal_agent_manager.exe

# Custom data root on macOS
UAM_DATA_DIR=/tmp/uam-data ./Builds/universal_agent_manager.app/Contents/MacOS/universal_agent_manager
```

## Tests

```bash
npm --prefix UI-V2 ci
npm --prefix UI-V2 run test
npm --prefix UI-V2 run build

cmake -S . -B Builds/tests -DUAM_BUILD_TESTS=ON -DUAM_PACKAGE_REMOTE_RUNNERS=OFF
cmake --build Builds/tests --config Debug
ctest --test-dir Builds/tests -C Debug --output-on-failure
```

Tests use a custom framework in `tests/test_harness.h` (runner `main` in `tests/core_tests.cpp`, general cases split by area in `tests/core_*_tests.cpp`), a CMake platform ifdef guard, and Vitest for `UI-V2`.

## UI Design

React/UI changes follow `docs/ui/design-language.md`; settings pages also follow `docs/ui/settings.md`. Update those documents with any deliberate design change.

## Code Style

- Brace style: Allman
- Indentation: 4 spaces, tabs for indentation
- Column limit: 10000
- Use `.clang-format` in the project root

## Architecture

- Entry point: `src/main.cpp` -> `src/app/application.cpp` -> `src/app/application.h`
- React UI: `UI-V2/src`
- CEF bridge: `src/cef/uam_query_handler.cpp`
- Gemini provider: `src/common/provider/gemini/`
- Codex provider: `src/common/provider/codex/`
- OpenCode provider: `src/common/provider/opencode/`
- Claude Code provider: `src/common/provider/claude/`
- GitHub Copilot provider: `src/common/provider/copilot/`
- Antigravity provider: `src/common/provider/antigravity/`
- ACP runtime: `src/common/runtime/acp/`
- Terminal runtime: `src/common/runtime/terminal/` plus platform services
- Computer use: `src/computer_use/` (MCP server and macOS/Windows platform layers)
- Phone companion: `src/cef/companion_server.cpp`, protocol in `src/common/companion/`, browser UI in `UI-V2/src/components/companion/`, iOS client in `Mobile/iOS/`
- Remote runner (`uam_runner`): `src/remote/`
- Markdown store: `src/app/markdown_store_service.cpp`; shipped entries in `markdown-store/bundled/` (top-level `markdown-store/*.uam` are maintainer-local)
- Memory service: `src/app/memory_service.cpp`
- Data storage: `<data-root>/chats/` JSON files plus optional `<data-root>/memory/`

## System documentation

Use [docs/README.md](docs/README.md) as the feature map and [docs/architecture/architecture.md](docs/architecture/architecture.md) for high-level system relationships and existing diagrams. Follow the Feature documentation agreement above; preserve the distinction between agreed behaviour, implementation, verification and acceptance.

## Data Root Resolution

1. `UAM_DATA_DIR` env var
2. OS default app-data location
3. Temp fallback
4. Relative `data` directory only if no temporary directory can be resolved

David confirmed on 2026-10-16 that the existing storage behaviour should remain. Do not add working-directory-first routing; see the [recorded storage decision](docs/documentation-review.md#data-root).

## Engineering agreements

Read [the architecture overview](docs/architecture/architecture.md), [data authority](docs/architecture/source-of-truth.md) and [engineering rules](docs/engineering/engineering.md) before changing system behaviour. OS-specific code belongs behind platform interfaces, provider-specific code behind runtime adapters, and Git/SVN-specific code behind VCS service/module boundaries. Existing exceptions do not authorize new scattered conditionals or an unrequested refactor. Use [proportional testing](docs/engineering/testing.md), [bounded private-data-safe logging](docs/engineering/logging.md) and the existing mandatory build/review gates. Keep the relevant feature and engineering agreements aligned when behaviour changes; a passing link check does not establish feature correctness.
