# UAM-Control read-only tools: #967, #964, #963 first slices

**Branch:** `v5/feat-control-readonly-tools`, pushed. **Final SHA:** `645693b7559e41510606f58f82f9640264a30ad8`. Base: `origin/uam/v5.2-sync-main` at `c252f9a9`. No PR, issue, comment, label or merge was made.

The first slice of each issue is in, in the requested order, with one feature commit per tool and per-tool review fix commits:

- **`usage_report` (#967):** the calling chat's own input, output and total tokens and estimated cost, labelled as an estimate. Also the cached allowance of the chat's provider on this computer: used share, reset, plan and whether a limit was reached.
  - It never covers another chat, starts no account read or Keychain prompt, and leaves out credit balances and spend amounts.
  - SSH chats get the allowance reported as unavailable.
  - It uses a new cache-only `AccountAllowanceService::CachedRow`, reached through a weak handle on `AppState`.
- **`worktree_status` (#964):** the commit panel's status read (Git or SVN, local or SSH) for the calling chat's own workspace. It returns the branch or revision, changed files, and line counts with totals.
  - It runs on a new bounded background-read path, off the app thread, and replies through the existing audited read-only reply.
  - Before replying it checks the resolved workspace root and host connection again, as the panel does.
  - The file list is cut off with a marker under the 64 KiB limit.
  - Line counts are included only for local workspaces with at most 200 changed files and 16 MB of them.
  - A provider cancellation or an expired 20 s budget drops the result.
  - The goal reviewer's prompt is unchanged, because giving it the tool needs a decision.
- **`memory_search` (#963):** uses recall's own roots and ranking, made public as `MemoryService::RecallRoots`, `RankRecall`, `RecallTerms` and `RecallScore`.
  - `BuildRecallPreface` now uses the same ranking; its output is unchanged.
  - Returns up to 10 previews with their scope, and never another workspace's memory.
  - Not offered on SSH hosts, and refused when memory is off.
  - Neither memory text nor the query is logged.

All three tools carry `readOnlyHint`, use the read-only reply path, and are offered in Plan mode and to read-only callers. Goal reviewers still get no tools.

Docs updated in the same commits:
- the UAM tools page;
- the memory, usage and changes introductions;
- the read-only reply list;
- all six providers' `mcp-connections` rows;
- a new "UAM read-only insight tools" section in documentation-review.md.

`python3 scripts/check_docs.py` passes.

**Not verified:**
- The native desktop and `uam_core_tests` targets refuse Linux, and the CEF CDN is blocked here. The new control-service and memory test cases have not been built or run. They were only syntax-checked with stubbed CEF headers.
- The new tool-server cancellation path for background reads has no test.
- Before this is accepted, someone needs to run on macOS or Windows:
  - `UamControlUsageReportCoversOnlyTheCallingChatAndKeepsMoneyOut`
  - `UamControlWorktreeStatusReadsTheCallingChatsGitWorkspaceOffTheAppThread`
  - `UamControlWorktreeStatusReadsAnSvnWorkingCopy`
  - `UamControlMemorySearchUsesOnlyTheCallingChatsRecallScopes`
  - `UamControlCapabilityBoundsGoalAuthorityReplayRateCancellationAndRestart`
  - `UamControlListsOnlyToolsTheSessionCanUse`
  - `PerformanceSuiteUamRequestOverhead`
  - then the full native inventory and the CTest gates.

## Review rounds

Both reviewers were independent claude-opus-5-5 agents started with a fresh context and given the issue text, the diff against `origin/uam/v5.2-sync-main` and the relevant docs.

**Round 1 (review): 12 suggested, 11 implemented (1 blocker, 5 should-fix, 6 nits).**
- Declined: the blocker "build and run the native tests". It cannot be done on Linux. The six provider rows now say the native tests have not been run, and the limitation is recorded here and in the PR Proof.

**Round 2 (re-check of the fixes): 10 suggested, 8 implemented (2 should-fix, 8 nits).**
- Declined: "`SameConnection` also compares host instruction text, so editing it mid-read gives a false 'workspace changed'". The commit panel uses the same check, and the false positive is harmless (ask again).
- Declined: "`ChangedBytes` finds the repository root by walking up to `.git` instead of `git rev-parse`". This would need `VcsCommitService` to expose its root, which is out of scope. It only affects the 16 MB heuristic, which stays bounded by the 200-file cap.

## Open questions for David (left out, not guessed)

1. Should agents see account money figures (credit balance, spend limit and amount used) in `usage_report`? They are left out for now.
2. May goal reviewers get the single read-only `worktree_status` tool, so `goal_service.cpp`'s reviewer prompt stops using shell `git`?
3. Should `worktree_status` be limited to the workspace folder? Like the commit panel, a Git read lists the whole repository. That would change the panel's shared read.
4. Should `worktree_status` offer "changes since this chat started" (the panel's chat comparison)?
5. Should a recall budget of 0 also disable `memory_search`? It currently only follows memory Off.
6. From the scoping review, before the next slices:
   - Does a skill an agent creates become usable by that agent straight away?
   - Should `agent_define` default to workspace scope?
   - How should "never broader than the creating chat" be checked?
7. `log_tail` (#967) stays deferred until diagnostic log lines carry a chat id.

## New issue preview

**Title:** `feature(vcs): let a VCS status read be stopped`

`VcsCommitService::Status` takes no stop token. A `worktree_status` read that is cancelled, times out or outlives UAM therefore keeps its git or svn command (or SSH helper round trips) running until it finishes. Its result is discarded, and the connection's next read waits for it.

**Desired outcome:** `Status` accepts a `std::stop_token` and passes it to the local and remote command runners. `worktree_status` and the commit panel's status query stop their command when cancelled or at shutdown.

**Evidence:**
- `src/app/vcs_commit_service.h` (`Status` signature).
- `src/app/uam_control_service.cpp` (`WorktreeStatusWork`, `Shutdown`).
- `docs/providers/uam-tools.md` (recorded limitation).

**Acceptance criteria:**
- [ ] A cancelled `worktree_status` ends its git or svn process within a bounded time.
- [ ] UAM shutdown stops running status reads.
- [ ] A test cancels a status read on a large repository.

**Review:** Not independently reviewed; raised by the second review round of the change.

**Prepared by:** claude-opus-5-5 in Claude Code (cloud session).

## PR preview

**Title:** `feat(uam-control): add read-only usage_report, worktree_status and memory_search`

Agents had no way to read their own chat's usage, their workspace's changes or saved memories without shell workarounds. This adds three read-only UAM tools, each the first slice of its issue in the V5.2.x scoping review. Each covers only the calling chat:

- `usage_report`: own tokens and estimated cost, plus the cached provider allowance without money figures.
- `worktree_status`: the commit panel's Git/SVN status, off the app thread, bounded and revalidated before replying.
- `memory_search`: recall's own scopes and ranking, previews only, local chats only.

All three carry `readOnlyHint` and use the read-only reply path, so Plan and read-only agents can use them. Goal reviewers still get no tools. Product questions are recorded in the documentation review.

Related to #967, Related to #964, Related to #963 (first slices; later tools and decisions remain open).

**Proof:**
- `python3 scripts/check_docs.py`: passed (203 pages, 1522 local links, 6 providers × 32 capabilities).
- `cmake -S . -B Builds/runner -DUAM_RUNNER_ONLY=ON` and `cmake --build Builds/runner --target uam_runner` on Linux: built. SQLite came from the npm `better-sqlite3` amalgamation because the official SQLite download was blocked.
- `tests/account_allowance_service_tests.cpp` on Linux, linked against the allowance service with a minimal stand-in harness: 13/13 passed, including the new `AccountAllowanceServiceCachedRowNeverStartsARead`.
- `g++ -fsyntax-only` with stubbed CEF headers: `src/app/uam_control_service.cpp`, `src/app/memory_service.cpp` and `src/common/usage/account_allowance_service.cpp` show no errors and only the pre-existing warning types. The new test case bodies were also checked through a stand-in harness.
- Not run: `uam_core_tests`, the full native inventory, CTest gates and the interaction/performance fixtures. The desktop targets refuse Linux, and these must run on macOS or Windows before acceptance.
- UI-V2 was not touched.

**Review:** 2 independent reviewers, 2 iterations. Independent claude-opus-5-5 (review): 12 suggested, 11 implemented. Independent claude-opus-5-5 (re-check): 10 suggested, 8 implemented.

**Created with:** claude-opus-5-5 in Claude Code (cloud session).

## Screens needing Before/After capture

None. The change adds agent-facing tools and docs only, with no React or UI change, so no visual review is needed.
