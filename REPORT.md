# Report: #1029 composer shows the chosen Git base

**Work branch:** `v5/fix-1029-composer-base-branch`
**Final SHA:** `95c9522118838da93d30674f4def60f728aca759`
**Base:** `origin/uam/v5.2-sync-main`
**Commits:** `cfd4661b` (the fix), `95c95221` (review nit 2)

## Review rounds

**Round 1, independent review** (`agent-reports/review-R1`, #1029 section): 2 suggested (1 should-fix, 1 nit); 1 implemented.
- Declined, should-fix 1 (add an action to the refusal line, revert the documentation-review exception): the product owner asked for the shortest one-line messages, at the level of "Local copy doesn't match what we expect", so "Local copy is on X, not Y." stays.
- Implemented, nit 2: the composer tab now renders when either the checked-out branch or the chosen base is known, so the chosen base stays visible when the branch read is pending or has failed.

**Round 2:** not run. The follow-up was one render guard plus two tests and a one-clause docs edit, which is not material enough for a re-check.

Blockers outstanding: none. Should-fixes outstanding: none (one declined as above).

## Issue previews

None.

## PR preview (`v5/fix-1029-composer-base-branch` into `uam/v5.2-sync-main`; base to confirm)

**`fix(composer): show a new chat's chosen Git base and shorten the pull refusal`**

Fixes #1029. Before the first message, a new chat whose starting branch differs from the checked-out branch now shows that starting branch in the composer branch tab. Its tooltip reads "Starts from build/x. Local copy is on main.", and the starting branch stays visible when the checked-out branch can't be read. Chats with no override, an override equal to the checked-out branch, or an explicit HEAD base look and behave as before.

The Pull before first chat refusal for a mismatched base is now one line naming both branches: "Local copy is on main, not build/x." The safety check itself is unchanged.

Docs updated:
- `docs/chat/composer/workspace-actions.md`
- `docs/chat/new-chat.md`
- `docs/settings/version-control/git.md`
- `docs/documentation-review.md`: this refusal is the exception to "names its cause and an action".

No provider capability rows are affected.

**Proof:**
- `npx vitest run` (UI-V2): 239 files, 4,221 tests passed. There are 6 new composer cases for #1029. The two key cases fail without the matching code change and pass with it.
- `npx tsc --noEmit -p .`: clean.
- `npm run build`: succeeded on `cfd4661b`. It was not rerun after `95c95221`.
- `python3 scripts/check_docs.py`: passed (203 pages, 1,510 links, 6 providers × 32 capabilities).
- Not run: the native build and `uam_core_tests`. CMake refuses Linux, and a syntax-only compile needs CEF headers that aren't available. `GitWorktreePullRefusesUnsafeSourceBeforeArtifacts` was updated to check the exact refusal text but has never been compiled or run. The GUI was not launched, and macOS and Windows are unverified.

**Review:** 1 independent reviewer, 1 iteration. Independent claude-opus-5-5: 2 suggested, 1 implemented (declined: the refusal-message action, per the product owner's one-line direction).

**Acceptance:** Claude review of the UI change and David's approval are still pending.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code remote session, scheduled task), on behalf of David Taylor.

## Screens needing Before/After capture

1. The composer workspace/branch tab in a new chat before its first message, with Pull before first chat on, the workspace on `main` and another existing branch chosen as the starting branch in New chat. Before: the tab shows `main`. After: it shows the chosen branch, and hovering shows "Starts from <branch>. Local copy is on main."
2. The refusal in the same setup after sending the first message. Before: the long "Pull before chat requires the default base…" message. After: "Local copy is on main, not <branch>."
3. Optional: both captures in the light and dark themes.
