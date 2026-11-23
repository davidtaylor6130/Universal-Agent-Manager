# Gemini account-wide refusal fix: report

**Branch:** `v5/fix-gemini-account-refusal`. **Final SHA:** `c1b258ecb900667df55c091271fcd74d607303b5` (3 commits: `f860d0a2` fix, `98836e9b` round-1 review fixes, `c1b258ec` round-2 fixes). The base `uam/v5.2-sync-main` is contained in `origin/uam/v5.2-performance`, and the branch merges into it without conflicts. Nothing was opened: no PRs, issues, comments, merges or force-pushes.

**The native build and tests have not been run.** CMake refuses Linux ("supports only Windows and macOS builds"), so `uam_core_tests` and the CTest gates still need a macOS or Windows run.

## The change

- **Classifier** (`src/common/provider/provider_account_refusal.{h,cpp}`). It applies to any provider whose replies go through the shared ACP path. A refusal must meet all three conditions:
  - it is an `initialize`, `session/new`, `session/load` or `session/resume` error;
  - it has code `-32000`;
  - it contains one of a short documented list of phrases naming the client or the account: unsupported client, ineligible or unentitled account, no active subscription, or authentication required.
- **Record** (`provider_model_catalog_service`). One refusal per provider configuration and computer, saved in `provider_model_catalog_cache.json`. It lasts 24 hours, or 1 hour for authentication. An expiry further out than its TTL, for example after the clock moved back, is ignored, and expired entries are dropped when the cache loads.
- **Skips while active:**
  - startup discovery holds the provider's scopes (a two-line skip check in `Poll`, kept small so it merges cleanly with `v5/perf-model-discovery-fanout`);
  - automatic model requests;
  - chat launches, for client and entitlement refusals only. An authentication refusal never holds a chat launch, because signing in fixes it straight away.
- **Notice.** One plain line through the existing `modelRefreshError` path (Settings → Providers, the model menu, New Chat). Gemini's line is "Google no longer supports this Gemini client for personal accounts." It uses that wording only when Google's message says "for individuals"; other cases get a neutral line. There is no React change.
- **What lifts it:**
  - Refresh or Retry in the model menu, which also releases the held startup scopes;
  - TTL expiry;
  - any later successful model answer on that computer;
  - a newer scoped failure, once the refusal has expired. An authentication refusal also gives way when a chat launch it let through fails for another reason.
- **Logging.** One classification line with a bounded, UTF-8-safe copy of the provider's message. The raw text stays in the existing `[acp-diag]` entry. The refusal is only recorded after the provider's own `OnAcpHandleError` has declined the error.
- **Docs.** New "Account refusals" agreement in `docs/providers/models.md`, with coverage per provider route. A shared pointer in `docs/providers/providers.md`. Notes on the Gemini `models` and `structured-chat` rows saying this is unverified. A new row in the source map.

## Review rounds (independent, fresh-context claude-opus-5-5)

| Round | Suggested | Implemented | Declined, with reason |
|---|---|---|---|
| 1 (whole branch) | 13 (0 blocker, 6 should-fix, 7 nit) | 11 | Nit 8 (cache the refusal check per provider and computer in `Poll`): left out to keep the `model_discovery_service.cpp` diff to a single skip check for the parallel discovery-scoping branch; each check is one mutex plus a workspace-free key. Nit 11 (clear the refusal only after `BeginDiscovery` starts): harmless, because a manual Refresh that finds a discovery already pending is still served by that discovery, and the handler path can't be tested here. |
| 2 (re-check of round-1 fixes) | 7 (0 blocker, 2 should-fix, 5 nit) | 6 | Nit 7 (the UTF-8 trim can drop one complete character at the 400-byte cut): harmless for a diagnostic line. Nit 3 was handled through its documented alternative: models.md now says Retry also releases held scopes. |

Total: 20 suggested, 17 implemented.

## Proof (only what I ran)

- `g++ -std=c++20 -fsyntax-only` with stub CEF headers and fetched nlohmann/json 3.11.3, on every changed native file and `tests/provider_account_refusal_tests.cpp`: no errors. `src/cef/acp_handlers.cpp` couldn't be checked because of gaps in the stub CEF headers (`companion_server.h`); the reviewer type-checked its one changed line by reading it.
- Standalone build of the classifier with 11 checks: all pass.
- `python3 scripts/check_docs.py`: passes (203 pages, 6 providers × 32 capabilities).
- **Not run:** the native build, `uam_core_tests` (8 new cases in `tests/provider_account_refusal_tests.cpp`), CTest, and a live Gemini account.

## Screens needing Before/After capture

There's no React change, but the backend changes what these existing screens show for an account Google refuses:
- the Settings → Providers Gemini "Model refresh failed" notice: before, a raw `session/new … code=-32000` error per workspace; after, the one-line Google notice;
- the model menu error and Retry for a Gemini chat;
- the New Chat model status for Gemini;
- a Gemini chat send while the refusal is active: after, the notice plus "Use Retry in the model menu to ask again."

Capturing these needs a macOS or Windows build.

---

## Issue preview

**Title:** `bug(gemini): account-wide refusal relaunches Gemini for every workspace on startup`

When Google refuses Gemini CLI for the whole account, UAM records the failure for each workspace, and only in memory. Every startup therefore launches Gemini model discovery again for every registered workspace, and no Gemini models are ever saved.

**Reproduction:** Use a Gemini Code Assist for individuals account with several workspaces registered, then restart UAM.
**Expected:** One refusal is recorded for Gemini on that computer, with one plain-language notice and no repeated launches until a retry.
**Actual:** Each workspace's discovery fails at `session/new` with `code=-32000` and "This client is no longer supported for Gemini Code Assist for individuals. To continue using Gemini, please migrate to the Antigravity suite...", and the same thing happens on every startup.

**Environment:** `uam/v5.2-sync-main`; operating system not recorded.

**Evidence:** From the report: 26 of 26 Gemini discovery launches failed. One workspace was relaunched 11 times in 3 days, and the model cache has no Gemini entries. Code inspection: `acp_response_handlers.cpp` (around lines 594–606) stores the failure for each provider, workspace and computer. `model_discovery_service.cpp` (around lines 165–166) refreshes on every restart, including scopes that failed.
**Reproduction status:** Not reproduced; no live account was used.

**Acceptance criteria:**
- [ ] The refusal is recorded once for the provider and computer, with a time limit, and survives a restart.
- [ ] While it's active, startup discovery and chat launches skip Gemini, and Gemini shows one notice.
- [ ] A manual retry and the end of the time limit both ask Google again.
- [ ] Gemini remains a supported provider.

**Review:** Self-triage only.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

## PR preview

**Title:** `fix(gemini): treat account-wide refusals as one provider-and-host state`
**Base/head:** `uam/v5.2-performance` ← `v5/fix-gemini-account-refusal`

When Google refuses Gemini for the whole account, UAM now records one refusal for Gemini on that computer instead of one failure per workspace. A refusal must be an ACP setup error (`initialize`, `session/new`, `session/load` or `session/resume`) with code `-32000` and one of a short documented list of client or account phrases. The refusal is saved with the model cache. It lasts 24 hours, or 1 hour for sign-in errors.

While it's active, startup discovery and automatic model requests skip the provider. Client and entitlement refusals also hold chat launches; sign-in refusals never do. Gemini shows a single line: "Google no longer supports this Gemini client for personal accounts." It lifts on Retry in the model menu, when the time limit passes, or on a later successful model answer. The change to startup discovery is a single skip check, so it merges beside the startup discovery-scoping work. The docs and the Gemini checklist rows are updated.

Related to: <the issue above, once filed>

**Proof:**
- `python3 scripts/check_docs.py` passed.
- A standalone classifier build with 11 checks passed.
- Every changed native file and `tests/provider_account_refusal_tests.cpp` (8 cases) compiled with `-fsyntax-only` against stubbed CEF headers with no errors. `src/cef/acp_handlers.cpp` could not be checked.
- **Not run:** the native build, `uam_core_tests` and CTest. The desktop build refuses Linux, so these need macOS or Windows.
- **Not done:** live Gemini account check.

**Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 20 suggested, 17 implemented. Round 1: 13 suggested, 11 implemented. Round 2: 7 suggested, 6 implemented. Three nits were declined (a `Poll` cache, kept out to protect the parallel branch's merge; clearing the refusal only after discovery starts, which is harmless; a one-character UTF-8 trim, which is harmless).

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
