# #998 Managed child chats keep their parent link after run records expire: report

Final SHA: `f4ecb78641ca9cb7e6308de982f8c8232a3e9d91` on `v5/fix-998-subagent-parent-link`, pushed. Base: `origin/uam/v5.2-sync-main`.

The bug was present on the base. A finished managed child recorded its root chat only in the agent-run ledger. Once its terminal record was pruned (90 days or beyond 5,000 records), the child blocked the root's worktree: SSH worktree preparation refused, and Port back and Discard stayed refused.

## What changed

- **Saved root link.** New managed children (v1 delegation, v1 interrupted continuation, v2 `@agent`) save `managed_root_chat_id` and `managed_root_created_at` in their own record. The fields are separate from `subagent_parent_chat_id`, so native-subagent display lineage and native-history import are unchanged. Bundle import clears them along with `agent_run_id`.
- **One recogniser.** `ManagedSubagentControlService::SavedRootLinkMatches` matches the exact root ID and creation time. It is used only when a complete ledger read names neither the child nor its run:
  - **SSH preparation:** the worker's own read-only ledger read, which must have no unreadable records.
  - **Port back/Discard:** the restart catalogue now records every retained run ID and transcript ID, in every shape, and whether the read was complete. It is read when a data folder's chats load, after startup pruning. The cleanup worker's fresh ledger read refuses if any record names a link-admitted child.
- **One settlement check.** `ChildInputSettled` (saved work in flight: queued, held, dispatched or approval-pending input; remote process, turn, delivery, stop, restart or recovery state) now gates both paths, for ledger-backed and link-backed children. Port back/Discard also keep refusing a child whose chat-recovery preference is on, as before.
- **Unchanged:** children saved before this change keep the old rule (they block once their record expires). v2 families whose run records still exist stay refused by Port back/Discard, because retiring their follow-up rights is separate work.
- **Docs:** `docs/chat/subagents.md` (new "Saved root link" section, cleanup paragraph) and `docs/workspaces/workspaces.md` (SSH preparation and family cleanup). Provider capability rows are unchanged: this is shared UAM behaviour, and every `subagents` row stays Partial.

## Review rounds

- **Round 1: independent review R2, #998 section. 4 suggested (1 blocker, 1 should-fix, 2 nits), 3 implemented.**
  - Blocker (implemented): family capture took "absent from the live index" as "expired", although the index omits non-initial-shape v2 records, unreadable records, and everything when the catalogue has not loaded.
  - Should-fix (implemented): the SSH saved-link branch missed held, dispatched and approval-pending input. Both paths now share one predicate.
  - Nit 3 (implemented): the fixtures now load the restart catalogue as production does.
  - Nit 4 (declined, as directed): it is an issue-tracker note, and this routine posts no issue comments. It appears under issue previews below.
- **Round 2: fresh-context `claude-opus-5-5` re-check of the round-1 fixes (material). 4 suggested (0 blockers, 1 should-fix, 3 nits), 2 implemented.**
  - Should-fix (implemented): the shared predicate brought the persisted chat-recovery preference into SSH preparation, which would have newly blocked finished ledger-backed SSH children with recovery switched on. SSH now checks saved work only.
  - Nit (implemented): the docs now say when the observation is taken, and that an unreadable record refuses the whole family.
  - Nit (declined): trimming the SSH path's explicit checks that the shared predicate repeats. It is cosmetic, and it would rewrite already-reviewed lines.
  - Nit (declined): a specific message for a record that appears after admission. The refusal is safe, and the reviewer judged it acceptable as is.
- **Total: 8 suggested, 5 implemented.**

Separately, a macOS run of the first commit, reported to this routine rather than run by it, found a deterministic failure in the new SSH test. The test set the parent's creation time after the task had copied the parent, so the owner check dropped the task. The test was fixed in `3cb8a883`, and production code was not affected.

## Checks run

- `python3 scripts/check_docs.py`: passes (203 pages, 1512 local links, 6 providers × 32 capabilities).
- `g++ -std=c++20 -fsyntax-only` with every build flag defined passes for `src/app/managed_subagent_control_service.cpp`, `src/app/worktree_launch_preparation_service.cpp`, `src/app/agent_run_scheduler.cpp`, `src/common/chat/chat_repository.cpp`, `src/app/local_chat_bundle_service.cpp`, `tests/git_worktree_port_method_tests.cpp` and `tests/acp_workspace_launch_tests.cpp`. CEF types came from small local stand-in headers, because the CEF download host is blocked here.
- **Not run:** the native build and tests. CMake refuses Linux (`universal_agent_manager supports only Windows and macOS builds`). UI-V2 was not touched.

## Issue previews

**Comment on #998 (preview, not posted):**

> Implemented on `v5/fix-998-subagent-parent-link` (`f4ecb786`); native verification on macOS and Windows is still pending.
>
> - New managed children (v1 and v2) save their root chat and its creation time, separate from native-subagent display lineage.
> - SSH worktree preparation and Port back/Discard recognise a finished child by that link only when a complete ledger read names neither the child nor its run. Queued, held, dispatched or approval-pending input, runtimes, saves, delivery and recovery state still block.
> - Children saved before this change keep today's behaviour.
>
> The fourth acceptance criterion is met only for v2 families whose run records have expired. v2 families whose records still exist stay refused, because their follow-up rights need a separate retirement step. Keeping this issue open for that.

## PR preview (not opened)

**Title:** `fix(subagents): keep managed children linked to their root after run records expire`

> Fixes the parent's worktree being blocked by its own finished managed children once their run records expire (#998).
>
> - **Saved link:** new managed children (v1 delegation, v1 continuation, v2 `@agent`) save `managed_root_chat_id` and `managed_root_created_at`, separate from the display link `subagent_parent_chat_id`.
> - **Where it is used:** SSH worktree preparation and Port back/Discard accept a finished child by that link only when a complete ledger read names neither the child nor its run. For Port back/Discard that is the read taken when a data folder's chats load, checked again by the cleanup worker.
> - **Still blocks:** queued, held, dispatched or approval-pending input, a runtime, save, delivery or recovery state, or a follow-up owner, through one settlement check shared by both paths. Children saved before this change keep the old rule.
> - **Not in scope:** v2 families whose run records still exist stay refused by Port back/Discard.
> - **Docs:** `docs/chat/subagents.md` ("Saved root link") and `docs/workspaces/workspaces.md`. Capability rows unchanged (shared UAM behaviour, still Partial).
>
> Refs #998
>
> **Proof:**
> - `python3 scripts/check_docs.py` passes (203 pages, 1512 local links, 6 providers × 32 capabilities). This checks documentation integrity, not runtime behaviour.
> - `g++ -fsyntax-only` passes for the five changed source files and both changed test files, with CEF types from local stand-in headers.
> - Native build and tests were not run: CMake refuses Linux. New and changed native cases: `RemoteWorkspaceReferenceAdmissionSkipsExpiredRunChildBySavedRootLink` (10 cases), `GitWorktreeExpiredRunChildWithSavedRootLinkPermitsPortAndDiscard` (4), `GitWorktreeExpiredRunChildWithoutMatchingRootLinkStillBlocksCleanup` (4), `GitWorktreeExpiredRunChildLinkRequiresCompleteLedgerObservation` (3). Live SSH was not exercised.
>
> **Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 8 suggested, 5 implemented. Declined: one issue-tracker note (no issue comments in this routine) and two nits (a cosmetic de-duplication, and a message for a safe refusal the reviewer judged acceptable).
>
> **Created with:** Claude Opus 5.5 (`claude-opus-5-5`) in Claude Code on the web (Claude Agent SDK), on behalf of David Taylor.

## Screens needing Before/After capture

- **Port back or Discard on a parent chat whose finished managed children have expired run records.** Before: the operation is refused with "Another chat uses this worktree. Its files and branch were left untouched." After: it completes, and the children's history stays intact. Staging this needs a data folder whose child chats carry the new link and whose run records were pruned, so a fresh install would need a child created after this change, then its record removed.
- **Opening that parent on its SSH host.** Before: worktree revalidation refuses and names the child chats. After: it proceeds. Same staging, plus a ready SSH host.
