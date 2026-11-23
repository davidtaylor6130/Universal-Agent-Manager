# #356 Refresh and initial-load full-state pull: measured case and regression check

**Final SHA:** `c299b66e7b48da5d3f4f5c2196e81ce8fa704676`, pushed to `v5/perf-356-refresh-fingerprint`. It is based on `origin/uam/v5.2-sync-main`, not on `v5/perf-947-state-patch-size`, because nothing in `src/cef/cef_push.cpp` changed. `git merge-tree` against `v5/perf-947-state-patch-size` merges cleanly.

**Scope:** only #356, as redirected. No #947 edits had been made, so nothing needed discarding. There is no production code change. The smallest fix the measurement suggested was tried and measured worse, so it was reverted (details below).

**What changed (6 files):**
- **New case `PerformanceRefreshFullStateFingerprint`** in `tests/interaction_performance_tests.cpp`.
  - **Fixture:** it writes the perf suite's synthetic data root (2,000 chats, 20 folders, six providers, twelve messages each, one 2,000-message chat), mirroring `write_fixture`. It loads that root through startup's own calls: `PersistenceCoordinator().LoadSettings`, `ChatFolderStore::Load`, catalogue `Initialize` plus `SetExecutionHostContext`, `LoadSidebarChats`, then selection. It then opens the long chat.
  - **What it times:** `HandleGetInitialState`'s work for a Refresh and for the initial load. That is the exact pass (`Serialize` then dump), the first-load baseline seed (keeping the summaries as `ResetPatchBaselines` does, plus `SerializeChatFingerprintDigests`), and freeing the reply tree. It prints p50/p95 and the reply size.
  - **Regression checks:**
    - the catalogue slice is built once per pull;
    - the open chat's history is hashed exactly once on the first pull and never on later ones;
    - a cold pull misses the path-probe memo exactly once per folder and kind;
    - p50 stays under 1,000 ms, which only catches gross regressions.
- **`scripts/perf_suite.py`** maps the new lines to `refresh_2000_p50_ms`, `refresh_2000_p95_ms`, `initial_load_state_2000_p50_ms`, `initial_load_state_2000_p95_ms` and `refresh_2000_reply_bytes`. `tests/perf_suite_test.py` covers that parsing.
- **`tests/ci_serial.txt`** lists the case, because it asserts a sub-second budget.
- **Docs:** a new "Full-state pulls" section in `docs/reference/architecture/react-cef-transport.md`, and an updated `native` row in `docs/reference/performance-suite.md`. No provider capability rows are affected.

**Review rounds** (both reviewers were fresh-context Opus agents):
- **Round 1:** 4 suggested (1 should-fix, 3 nits), 4 implemented.
  - The should-fix: the initial-load timing freed the whole tree, but production keeps the chat summaries as the push baseline.
- **Round 2 (re-check):** confirmed all four round-1 items fixed. 5 suggested (1 should-fix, 4 nits), 4 implemented.
  - The should-fix was process only: the PR text must carry the figures. It does, below.
  - **Declined (1):** a cross-link at `react-cef-transport.md` line 35. The other branch rewrites that line, so the edit would conflict.

## New issue preview

**Title:** `refactor(state): write desktop state replies without building a full JSON tree`

A desktop state pull (Refresh, page reload, resync or first load) builds the whole state as a JSON tree on the UI thread, dumps it to text, then frees the tree. On 2,000 chats, freeing the tree is the largest single cost of a Refresh.

**Use case / desired outcome:** Refresh and resync on a large install should hold the UI thread for less time. The reply's content, the push baseline seeded from the first reply and the phone projection must not change.

**Environment:** `uam/v5.2-sync-main` at `c252f9a9`, plus `PerformanceRefreshFullStateFingerprint` from `v5/perf-356-refresh-fingerprint`. Measured only in a Linux scratch build of the core objects, with CEF stubs and a stub platform layer; Linux is not a supported desktop platform. macOS and Windows have not been measured.

**Evidence:**
- **Breakdown (observed, 2,000 chats, 21 samples, one scratch run):** `Serialize` 43 ms, dump 38 ms, freeing the tree 61 ms.
- **Case output (observed):** a Refresh frees its tree in 59–62 ms p50, out of a 140–145 ms p50 Refresh.
- **Off-thread free (observed, rejected):** handing the tree to a background worker made back-to-back pulls slower, 131–139 ms → 221–238 ms p50. Spaced pulls gained only about 15%. Hypothesis: the UI thread's next allocations then compete with the worker's frees.
- **Hypothesis:** writing the reply text directly, without the tree, would remove both the tree build and the free.

**Reproduction status:** measured as above. Not reproduced on a supported platform.

**Acceptance criteria:**

- [ ] `PerformanceRefreshFullStateFingerprint` Refresh p50 is lower on the same machine, from at least three runs before and after.
- [ ] Reply content is unchanged for the frontend and the phone projection, and the first reply still seeds the push baseline.

**Review:** drafted from my own measurements; no independent review of this report.

**Prepared by:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.

## PR preview

**Title:** `test(perf): measure the Refresh and initial-load full-state pull on 2,000 chats`

Every desktop `getInitialState` summary pull (Refresh, page reload, resync and the first load) serializes the whole state on the UI thread in an exact pass. Until now it had no measurement and no regression check (#356). This adds `PerformanceRefreshFullStateFingerprint`.

- **Fixture:** the performance suite's synthetic 2,000-chat data root, written to disk and loaded through startup's own calls, with the 2,000-message chat open.
- **Timing:** it times what `HandleGetInitialState` does for a Refresh and for the first load, and prints p50/p95.
- **Checks:** a pull builds the catalogue slice once, hashes the open chat's history at most once and no transcript on a repeated pull, and misses the path-probe memo exactly once per folder and kind.

The perf suite reports the new figures, and the case runs in the CI serial phase. There is no production change. Freeing the reply tree on a background worker was measured and not adopted (figures below). Related to #356: the live 643-chat install has still not been measured, and acceptance is pending.

**Proof:**
- **Where it ran:** the desktop build refuses Linux and this container cannot download CEF. All native runs below therefore come from a scratch Linux build of the core objects, with CEF stubs, a stub platform layer and unused symbols left unresolved. The case's code path ran unmodified. Linux is not a supported platform; these are not macOS or Windows figures.
- **Harness sanity check:** the existing `PerformanceStatePushModelCatalogueCost` passes in it and reproduces the 261 KB live patch the triage reported.
- **`PerformanceRefreshFullStateFingerprint`, final commit, 3 runs, 21 samples each:** all passed, with a 6,728,333-byte reply and 40 memo probes for 20 folders.

  | Figure | p50 | p95 |
  |---|---|---|
  | Refresh | 140.4–144.6 ms | 161.7–201.0 ms |
  | Initial load | 99.2–111.1 ms | 110.5–159.6 ms |
  | Serialize and dump (inside initial load) | 79.9–91.0 ms | |
  | Baseline seed (inside initial load) | 19.4–20.3 ms | |
  | Refresh freeing its reply tree | 59.1–61.7 ms | |

  The first load measures faster than a Refresh because it keeps its chat summaries as the push baseline instead of freeing them.
- **The check catches a real regression:** a scratch serializer whose provider-catalogue slice probes workspace ownership without the memo fails the case with "made 20 memo path probes for 20 folders; expected one per folder and kind". The real code passes.
- **Rejected trial (handler freeing the reply tree on a background worker), same case before/after:**
  - back-to-back pulls, 3 runs each: Refresh p50 130.5–138.8 → 221.1–237.9 ms;
  - pulls spaced 400 ms apart, 2 runs each: 146.3–154.9 → 127.0–130.3 ms;
  - in both, serialize and dump themselves got slower (about 78 → 213–238 ms back to back, about 87 → 127–132 ms spaced). The fix was reverted.
- **Other checks:** `python3 tests/perf_suite_test.py` passed. `python3 scripts/check_docs.py` passed. `git merge-tree` with `v5/perf-947-state-patch-size` merges cleanly. Also passing in the same harness: `PerformanceInitialStateSummaryResponse`, `PerformanceLargeCatalogStateFingerprint`, `PerformanceIdleInstallPushTick` and `StatePushSendsEachModelCatalogueOnlyWhenThePageLacksIt`.
- **Not run:** `uam_core_tests` on macOS or Windows, the full native inventory, CTest gates, the perf suite's `native`/`app`/`driven` pieces, and any live 643-chat measurement. No UI code changed.

**Review:** 2 independent reviewers, 2 iterations.

- Agent 1: Opus, fresh context (exact model ID not recorded; requested as `opus`): suggested 4; implemented 4.
- Agent 2: Opus, fresh context re-check (exact model ID not recorded; requested as `opus`): suggested 5; implemented 4. Declined: a cross-link on `react-cef-transport.md` line 35, which `v5/perf-947-state-patch-size` rewrites.

**Created with:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Code 2.1.330), on behalf of David Taylor.

## Screens needing Before/After capture

None. The change is a native test case, the perf-suite script and docs, with no UI change.
