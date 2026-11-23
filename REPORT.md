# Shell action safety fixes: report

The review loop is finished. Work branch `v5/fix-shell-action-safety` is at **`93b3e6ffeba7287dc6548bc2bc3940b1691812ba`**, which is 3 commits on top of `origin/uam/v5.2-sync-main`. No PR or issue has been opened.

Commit trailers: `ce1ef2f8` and `93b3e6ff` both end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. The first commit, `2fa26c00`, carries that line but ends with a `Claude-Session:` line after it. Fixing that needs a force-push, which was ruled out.

## Review rounds

Each round used a separate Opus subagent with a fresh context. They were requested as `opus`; the exact model ID served was not recorded.

| Round | Suggested | Implemented | Declined |
|---|---|---|---|
| 1: full review of the branch | 10 (0 blocker, 2 should-fix, 8 nits) | 9 | 1 |
| 2: re-check of the round 1 fixes | 6 (0 blocker, 1 should-fix, 5 nits) | 5 | 1 |
| Total | 16 | 14 | 2 |

**What was implemented**
- **Round 1, should-fix:**
  - An excluded provider's invalid actions are no longer counted in the startup notice or named in run messages.
  - After a load that could not read the file at all, the next save copies the existing file aside first, even if it now parses.
- **Round 1, nits:**
  - Every field of an invalid entry is read, so its row shows as much as possible.
  - The full (absolute) selected path is checked for control characters, not just the raw one.
  - Kept rows show only a wrapping "Invalid: <reason>" status.
  - Docs now say "ASCII letter case".
  - New tests cover prompt escaping and the forced aside copy.
  - Aside copies use a UTC timestamp in their name.
- **Round 2, should-fix:** if the reload right after a save cannot read the file, the list that was just saved stays in memory instead of an empty one.
- **Round 2, nits:**
  - A later successful seeding load clears the unreadable flag and its stale notice.
  - Apply is refused while the saved list cannot be read, so it doesn't uninstall every menu item.
  - Aside copies get a numbered suffix rather than clock digits.
  - Long reasons wrap anywhere.

**Declined**
- **Round 1, #10** (qualify Finder names only when they collide):
  - Names that change whenever another action is added would be no more stable than always-qualified names.
  - The group-path name is the fix you requested.
  - The likely loss of macOS Services keyboard shortcuts on the first Apply is left as a release note. It was not verified.
  - JSON-escaped backslashes in Windows paths are part of quoting by design.
- **Round 2, #6** (a test for excluded-provider invalid entries):
  - The reduced-provider test target doesn't compile `shell_action_service.cpp`, and adding it needs a CMake change I can't build on Linux.
  - This is a known test gap; the behaviour has only been checked by reading the code.

## Issue preview

**Title:** `bug(shell-actions): Finder name collisions, silently dropped saved actions, and file names injected into skill prompts`

Three independent shell-action defects were found by code inspection on `uam/v5.2-sync-main` (product-owner report, verified against source).

1. **Same label in different groups collides in Finder.** Uniqueness was only enforced within a group, and the macOS Info.plist used only the label. Expected: distinct Finder entries. Actual: duplicate Quick Actions, or one hides the other.
2. **Invalid saved actions are lost.** Load skipped invalid entries, and a wrong-typed field aborted loading every later entry. The next full-list save from Settings then removed them from disk.
3. **File names are embedded raw in the skill prompt.** A file name containing a newline could add instruction lines to an unattended skill run.

**Environment:** `uam/v5.2-sync-main` at `6f235eb9`; macOS (1), all platforms (2, 3).
**Evidence:**
- `shell_action_service.cpp`: `ValidateMenuHierarchy`, `MacWorkflowInfo`, `Load` and the `RunRequest` prompt.
- `shell_action_handlers.cpp`: the full-list save.

**Reproduction status:** confirmed by reading the source; not reproduced in a live Finder or Explorer.

**Acceptance criteria:**
- [ ] Same-label actions in different groups appear as separate Finder Quick Actions.
- [ ] Invalid saved actions stay listed, disabled, with a reason, and survive saves.
- [ ] A path containing a newline never reaches a skill prompt.

**Review:** self-triage only.
**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code remote session), on behalf of David Taylor.

## PR preview

**Title:** `fix(shell-actions): unique Finder names, keep invalid saved actions, quote skill paths`

- **Finder names:** Finder Quick Action names now carry the group path (`Version Control › GitHub › Review`). Save refuses enabled actions whose flat Finder names would still match. Action names cannot contain control characters.
- **Invalid saved actions:**
  - They are kept in place, disabled and read-only, with a one-line reason and a one-line startup notice.
  - They are written back disabled on every save, and only deleting one removes it.
  - An excluded provider's invalid actions stay hidden.
  - A saved file that cannot be read or parsed is copied aside before it is replaced, and Apply is refused while it cannot be read.
- **Skill prompt:** selected paths with line breaks or other control characters are refused before any workspace or chat is created. The skill prompt lists each path as an escaped JSON string marked as data.

**Proof:**
- **UI-V2:** `npm ci`; `npx vitest run` (240 files, 4,217 tests passed); `npx tsc --noEmit -p .` passed; `npm run build` passed.
- **Docs:** `python3 scripts/check_docs.py` passed.
- **Native compile check:** changed sources and tests compile with GCC 13 and clang 18 against CEF headers (some generated headers stubbed). The round 2 reviewer also compiled the macOS-only tests with the Apple guard forced on.
- **Native test run:** 7 shell-action and startup-source cases passed in a Linux runner where unrelated symbols were left unlinked.
- **Not run:** the full `uam_core_tests`, the macOS Finder test, Windows, and live Finder/Explorer installation. Tests that create chats couldn't link here, and there is no test for excluded-provider invalid entries.

**Review:** 1 independent reviewer, 2 iterations (a fresh-context Opus subagent each round). Independent Opus (requested as `opus`; exact model ID not recorded): 16 suggested, 14 implemented. Claude review sign-off of the React change and David's approval are still pending.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code remote session), on behalf of David Taylor.

## Screens needing Before/After capture

- **Settings > Shell Actions with a kept invalid row:** red "Invalid: …" status, disabled switch, no expand button.
- **Notifications panel:** the startup notice "N saved shell actions are invalid and stay disabled. See Settings > Shell Actions." and the unreadable-file notice.
- **Finder Quick Actions menu:** two same-label actions from different groups, shown as `GitHub › Review` and `SVN › Review`.
- **Apply while the saved file can't be read:** the refusal notice.
