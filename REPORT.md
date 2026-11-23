# Report: v5/refactor-dead-native-code

**Work branch:** `v5/refactor-dead-native-code`
**Final SHA:** `bdc6d12add33eef0136fba7d2b06d264a8bb88df`

- **Commits:** two, both on the work branch.
  - `895cdd36` is the cleanup.
  - `bdc6d12a` holds the review fixes.
- **Base:** cut from `uam/v5.2-sync-main` at `6f235eb9`.
- **Merging:** the branch merges cleanly into `origin/uam/v5.2-sync-main` (`c252f9a9`) and `origin/uam/v5.2-performance` (`64671114`).
- **Callers on newer bases:** neither base adds a caller of any removed symbol.
- **Pushes:** only the work branch and this report branch. No PR, issue, comment, label, merge or force-push.

## Deleted (24 files, 260 lines removed, 1 changed)

| Item | Lines |
|---|---|
| `GoalService::GetGoalsForChat`, with its doc comment | 16 |
| `NativeSessionLinkService::InferNativeSessionIdForLocalDraft` | 6 |
| `PersistenceCoordinator::LoadFrontendActions` and its `frontend_actions.h` include | 26 |
| `AppState::frontend_actions` (only the deleted loader used it) and the unused include in `application.cpp` | 2 |
| `ProviderResolutionService::ActiveProviderUsesNativeOverlayHistory` | 7 |
| `AppPaths::DefaultGeminiUniversalRootPath`, with its summary line | 12 |
| `LatestCliTransportActivityTime` | 6 |
| `ResolveImportedProjectRootOrFallback`, plus `ImportedProjectRootExists` (orphaned by it) | 25 |
| `PendingCallMatchesCliTerminalIdentity` (declared, never defined) | 1 |
| `kSupportedGeminiVersion` | 5 |
| `src/core/gemini_cli_compat.{h,cpp}` | 133 |
| `gemini_cli_compat`: CMake entries / `test_harness.h` include / compat test case | 2 / 1 / 17 |

**Skipped:** none.

The audit missed one test caller: `tests/core_state_cli_tests.cpp:2165` passed `PreferredGeminiCliVersion()` as an install version. It now passes the same literal, `"latest"`.

No document named a removed item, so no docs changed.

**Forbidden files:** none of them were touched.

## Review rounds

**Round 1:** a fresh-context Opus reviewer read the full diff. 4 suggested, 3 implemented.
- **Implemented (should-fix):** removed the unused `AppState::frontend_actions` field and the unused include in `application.cpp`.
- **Implemented (nit):** removed the leftover doc comment in `goal_service.h`.
- **Implemented (nit):** removed the leftover summary line in `app_paths.h`.
- **Declined (nit):** `agent-code-map.md` line about "frontend action metadata". The module still exists and tests still use it, so the line remains accurate.
- **Compile errors:** the reviewer found none. It also confirmed there is no `-Werror` and that the leak-gate residue counts cannot fail on removed strings.

**Round 2:** a fresh-context Opus re-check of the fixes, run because removing an `AppState` field was material. 1 suggested, 0 implemented.
- **Declined (nit):** the `frontend_actions.h` include in `app_state.h` is now unused there. Other files may rely on it for `<map>` and `string_utils.h`, and no full build was available here to prove they don't. Kept.

**Totals:** 5 suggested, 3 implemented, across 1 reviewer model and 2 iterations.

## Not verified
- **Native builds and tests:** the desktop build, `uam_core_tests`, CTest and the build-flag leak-gate rows were not run. The native desktop build refuses Linux, and the CEF headers download was blocked.
- **Uncompiled files:** `goal_service.cpp`, `native_session_link_service.cpp`, `provider_resolution_service.cpp`, `terminal_polling.{cpp,h}`, `application.cpp` and the test files were not compiled. They were reviewed by hand only.

## Screens needing Before/After capture

None. This change is native-only, with no UI or visible behaviour change.

---

## Issue preview

**Title:** `refactor(app): remove native C++ helpers with no callers`

A read-only audit of `uam/v5.2-sync-main` found native functions, declarations and one module that nothing calls, not even tests:
- `GetGoalsForChat`
- `InferNativeSessionIdForLocalDraft`
- `LoadFrontendActions`
- `ActiveProviderUsesNativeOverlayHistory`
- `DefaultGeminiUniversalRootPath`
- `LatestCliTransportActivityTime`
- `ResolveImportedProjectRootOrFallback`
- `PendingCallMatchesCliTerminalIdentity` (declared, never defined)
- `kSupportedGeminiVersion`
- `src/core/gemini_cli_compat.{h,cpp}`, which only its own test reached

They add build and reading cost without affecting behaviour.

**Desired outcome:** remove them and any helpers they leave orphaned, with no behaviour change.

**Evidence:** a token search across `src/`, `tests/` and `UI-V2/src` at `6f235eb9` found no callers. The one exception was a test that used `PreferredGeminiCliVersion()` as an input value.

**Acceptance criteria:**

- [ ] The listed items, their CMake entries and the compat test are removed.
- [ ] The native build and core tests pass on macOS.
- [ ] The build-flag leak gate passes.

**Review:** self-triage only.

**Prepared by:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

## PR preview

**Title:** `refactor(app): remove native helpers that nothing calls`

This PR deletes code that nothing calls:
- nine unused native functions or declarations;
- one constant;
- the test-only `gemini_cli_compat` module, with its CMake entries and compat test.

It also removes what those deletions orphaned:
- `ImportedProjectRootExists`;
- the `AppState::frontend_actions` field;
- one unused include;
- leftover doc comments.

One unrelated test now passes the literal `"latest"` instead of `PreferredGeminiCliVersion()`, which returned that same constant. Behaviour is unchanged, and no document names a removed item.

**Proof:**
- **Caller search:** a repository-wide `git grep` found no remaining callers. It was run at `6f235eb9`, `c252f9a9` and `uam/v5.2-performance` `64671114`.
- **Runner build:** `uam_runner` (runner-only Release build on Linux) built. It used the system SQLite because sqlite.org was blocked. Of the edited files, it compiles only `app_state.h`.
- **Syntax checks:** `g++ -fsyntax-only` passes for:
  - `persistence_coordinator.cpp`, `app_paths.cpp` and `chat_import_utils.cpp`;
  - `app_state.h` and every edited header except `terminal_polling.h`.
- **Docs:** `scripts/check_docs.py` passes.
- **Not run:**
  - the desktop build, `uam_core_tests`, CTest and the leak-gate profiles;
  - the other five edited sources and the test files, which need CEF headers that could not be downloaded here.

**Review:** Reviewed by 1 agent over 2 iterations.

- Agent 1: claude-opus-5-5 — suggested 5; implemented 3. One declined nit was an accurate doc line about a module that still exists. The other was an include in `app_state.h`, kept until a full build can confirm nothing relies on it.

**Created with:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
