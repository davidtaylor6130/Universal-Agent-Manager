# Model discovery process storm: final report

**Work branch:** `v5/perf-model-discovery-fanout`, final SHA **`9ac232a6`** (4 commits on `uam/v5.2-sync-main`). No PR opened.

## Per-provider classification

| Provider | Startup scope | Evidence |
|---|---|---|
| Codex | Account-wide | `model/list` carries only a cursor; discovery stops before `thread/start`, the only request with a `cwd`. |
| Claude Code | Account-wide (project settings: needs verification) | Models come from the `initialize` control response; no workspace in the request. |
| Copilot | Account-wide | `models.list` with empty params; `workingDirectory` only on `session.create`, which discovery never sends. |
| Gemini | Per workspace | ACP `session/new` carries `cwd`; the response's current model is stored with the list. |
| OpenCode | Per workspace | `/api/model?location[directory]=<cwd>`; project `opencode.json` adds models (consistent with #1027). |
| Antigravity | Per workspace | `agy models`, then `-p /model` stores the workspace's current model as `isDefault`. |

## Launch counts (same 60-workspace fixture, base vs branch)

| | Base | Branch |
|---|---|---|
| First start | 180 (60 SSH) | 64 (22 SSH) |
| Restart, same CLI versions | 180 | 0 |
| Restart after local Codex update | 180 | 1 |
| Max running at once | 2 | 2 |

## Review rounds

**Round 1:** a fresh-context Opus reviewer (exact model ID not recorded) made 8 suggestions (0 blocker, 2 should-fix, 6 nit). I implemented 6 in full and 2 in part.
- Should-fix: a failing source stranded its group. Now the remaining workspaces get another source, newest cache first, up to 3 sources per run. Implemented.
- Should-fix: `provider-sessions.md` was stale. Implemented.
- Nit: version stamping. Partly implemented. I added the SameConnection guard. I declined stamping at launch time: it is rare (a CLI update while discovery runs), and the worst case is one extra refresh.
- Nit: diagnostics. Implemented.
- Nit: concurrency test. Implemented.
- Nit: shared-list test. Implemented.
- Nit: share-only groups wait for a process slot. Partly implemented: the share now uses the newest fresh source. I declined moving shares ahead of the concurrency check, because every waiting group would then re-check the filesystem on every poll.
- Nit: doc status wording. Implemented.

The fixes also caught a real bug: reading a missing cache entry threw.

**Round 2:** a fresh-context Opus re-check of the fixes made 3 suggestions (3 nits) and I implemented all 3:
- a new test proves the newest cache is chosen as the source;
- a discovery that stores options but no models now counts as a failed source;
- a test comment is corrected.

The re-check confirmed prior points 1-6 and 8 resolved. Point 7 is partly resolved, with the declined half accepted as justified.

## What's unverified
- **How it was tested:** a Linux-only setup. I compiled the app sources with stand-in CEF headers and a stand-in path service, linked with unresolved symbols ignored, and ran a custom test runner.
- **Results:** 24 of the 28 cases in `model_discovery_startup_tests.cpp` pass. The other 4 crash in that setup on the base branch as well, so they are unverified.
- **Not run:** the macOS/Windows native build, the full CTest run, the real test runner's state-cache check, and any live provider discovery.
- **Open questions:** whether Codex or Claude project configuration can change their account-wide list, and whether Gemini's list itself (not only its current model) varies by workspace.
- **Other branches:** `fix/1027-model-discovery` and `v5/fix-gemini-account-refusal` are not on the remote; I read issue #1027 instead.
- **Commit title:** commit 2 is titled `test(models)`, not `perf(models)`. I couldn't retitle it without a force-push.

## Screens needing Before/After capture
None. The change has no UI.

## Issue preview

**Title:** `bug(models): every restart relaunches model discovery for each provider, host and workspace`

Every UAM restart starts about 200 provider processes, about 40 of them over SSH, to re-fetch model lists. Codex and Claude return the same list in every workspace. A real install recorded 1,189 discovery processes in 3 days (564 in one hour, 207-238 per restart) across 76 workspaces, with 263 identical local Codex catalogues.

**Reproduction:** Register many workspaces with chats for several providers, then restart UAM.
**Expected:** A cache under 7 days old from the same CLI version is kept. Account-wide lists are fetched once per computer.
**Actual:** One discovery per provider, host and workspace on every restart, even with a fresh cache.

**Environment:** `uam/v5.2-sync-main` (6f235eb9).

**Evidence:** Process counts are user-reported. In code, `src/app/model_discovery_service.cpp` (~67-104) makes one scope per provider, host and workspace, and the restart path (~165-166) ignores a fresh cache. That contradicts the 7-day rule in `docs/providers/models.md`.
**Reproduction status:** Reproduced on a synthetic fixture: 60 workspaces × 3 providers → 180 launches per start.

**Acceptance criteria:**
- [ ] Account-wide providers are discovered once per provider and computer at startup, and the result is shared.
- [ ] Startup respects the 7-day freshness rule unless the CLI version changed or the user asked for a refresh.
- [ ] The concurrency limit is kept.
- [ ] Tests count discovery launches for N workspaces.

**Review:** Self-triaged only; no independent report review.

**Prepared by:** Claude Opus 5.5 in Claude Code on the web (Claude Code 2.1.330, scheduled routine), on behalf of David Taylor.

## PR preview

**Title:** `perf(models): share account-wide model lists at startup and keep fresh caches`
**Base ← head:** `uam/v5.2-sync-main` ← `v5/perf-model-discovery-fanout`

Startup model discovery launched one process per provider, host and workspace on every restart, ignoring fresh caches.

Providers now declare whether their model list is account-wide. Codex, Claude Code and Copilot are asked once per provider and computer, and the result is shared with their other workspaces. Gemini, OpenCode and Antigravity stay per workspace. Startup keeps a cache under 7 days old that the current CLI version fetched.

If a source fails, startup tries another workspace, newest cache first, up to three per provider and computer. Refresh, Retry and the two-process limit are unchanged.

Docs are updated, with per-provider evidence in `docs/reference/model-catalogue-scope.md`.

Related to #1027 (OpenCode stays per workspace, worktrees included).

**Proof:**
- Measured on the same 60-workspace synthetic fixture (40 local, 20 SSH; Codex, Claude and Gemini chats), base vs branch:
  - First start: 180 → 64 launches (SSH: 60 → 22).
  - Restart with the same CLI versions: 180 → 0.
  - Restart after a local Codex update: 180 → 1.
  - At most two discoveries ran at once in both.
- 24 of the 28 cases in `model_discovery_startup_tests.cpp` pass in a Linux-only setup: stand-in CEF headers and path service, unresolved symbols ignored, custom runner. This includes the new cases for launch counts, sharing, a failing source and newest-source choice. The other 4 crash in that setup on the base branch too, so they are unverified.
- `python3 scripts/check_docs.py` passes.
- Not run: the macOS/Windows native build, the full CTest run, and live provider discovery.

**Review:** 1 independent reviewer per round, 2 iterations; fresh-context Opus each time, exact model ID not recorded.

- Round 1: Opus reviewer suggested 8; implemented 6 in full and 2 in part.
- Round 2: Opus re-check suggested 3; implemented 3.

**Created with:** Claude Opus 5.5 in Claude Code on the web (Claude Code 2.1.330, scheduled routine), on behalf of David Taylor.
