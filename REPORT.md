# PR watch: untrusted comments: agent report

**Work branch:** `v5/fix-prwatch-untrusted-comments` (based on `origin/uam/v5.2-sync-main` at `6f235eb9`)
**Final SHA:** `41319fcd` (commits `b4351afd`, `2abe4fd0`, `41319fcd`)

The review loop is done: one independent Opus reviewer and one fresh-context Opus re-check, both with no prior context. All three commits are pushed to the work branch. The first commit's author is David's name and email from the repo history instead of the session's Claude identity. It stays because force-pushes are not allowed.

## Review rounds

**Round 1: independent review of `b4351afd`.** 0 blockers, 3 should-fix, 8 nits. **11 suggested, 10 implemented** (in `2abe4fd0`).

- **Should-fix 1 (implemented):** ten outside comments could still start a YOLO turn, because the message that ends a watch was never held. Now it is held when it is outside-only, and it asks the agent to check with the user before watching again.
- **Should-fix 2 (implemented):** check names and review file paths come from a fork's own files. They are now quoted like comment text.
- **Should-fix 3 (implemented):** more invisible characters are now handled:
  - U+2028 and U+2029 are read as spaces.
  - Tag characters, U+061C, U+2060–2064 and variation selectors are stripped.
- **Nits (implemented):**
  - The held flag is cleared wherever an undelivered wake is dropped.
  - The watch list is saved as version 2, and undelivered wakes saved by version 1 are dropped.
  - The docs say "every wake that lists pull request news".
  - The docs say the notice does not come back after a restart.
  - Hold and release are logged, naming only the chat.
  - A narrowing conversion in a lambda is fixed.
  - New tests cover Stop while held, the end of a watch in YOLO, the legacy `approval_mode` YOLO value and dropping version-1 wakes.
- **Nit declined:** cutting the pending wake at 16 KiB keeps its start rather than its end. This was already the case before this branch, and it can't be reached in practice: at most 10 items per change, each line bounded to a few hundred bytes.

**Round 2: fresh-context re-check of `2abe4fd0`.** 0 blockers, 2 should-fix, 5 nits. **7 suggested, 7 implemented** (in `41319fcd`), with one part of a nit declined.

- **Should-fix 1 (implemented):** the release log almost never fired. A held wake is now marked in `Apply` and before the early return in `DeliverWakes`. In the scratch run, 5 holds and 3 releases were logged.
- **Should-fix 2 (implemented):** the merged or closed message now also says "UAM wrote this update; the user did not."
- **Nits (implemented):**
  - The held mark is cleared on every drop path, so a wake that was never held is not logged as released.
  - Watching again drops a held message that ended the previous watch.
  - More invisible characters are stripped: U+034F, the Hangul fillers, U+FFF9–FFFB and U+1D173–1D17A. U+0085 is read as a space.
  - The docs wording for the held ending message is corrected.
  - Test improvements:
    - `#include <iterator>` is explicit.
    - A test was renamed.
    - New coverage: more hidden characters, and leaving YOLO releasing a held ending.
- **Part declined:** a test that captures the log lines. `uam::diagnostics::Write` has no test hook, and adding one is outside this fix. I checked the log by hand in the scratch run instead.

## Issue preview 1

**bug(pr-watch): wakes pass non-collaborator comment text to the agent as an instruction**

Pull request watch wakes arrive in the chat as a normal user turn. They quoted every commenter's text and ended with "Look into each item and act on it as your task requires." On a public repository anyone can comment, so a stranger's text reached the agent looking like a user instruction. That includes YOLO chats, where nobody approves the commands that follow.

**Desired outcome:** comment text is quoted only from owners, members and collaborators, and marked as untrusted. Other authors are named with their association and link only. The closing line never tells the agent to act on comment content. In YOLO chats, comments only from outsiders don't start a turn without the user's approval.

**Environment:** `uam/v5.2-sync-main` at `6f235eb9`: `src/common/pr_watch/pr_watch_rules.cpp` (`BuildMessage`), `src/app/pr_watch_service.cpp` (`SendWake`, `DeliverWakes`), `src/app/uam_control_service.cpp` (`PullRequestWatch`).

**Evidence:** seen by reading the source. Not reproduced against live GitHub or a live provider.

- [ ] Wakes show each author's association (owner, member, collaborator or other)
- [ ] Comment text is quoted only for owners, members and collaborators, as escaped one-line untrusted data
- [ ] The closing line treats quoted GitHub text as untrusted information and never says to act on it
- [ ] In a YOLO chat, comments only from outsiders are held and the user is notified
- [ ] Docs and tests updated

**Review:** not recorded.
**Prepared with:** Claude Code (Claude Opus 5.5).

## Issue preview 2

**question(pr-watch): outside comments count towards the comment-only stop, and wakes have no distinct turn style**

After the fix above, two things are still open:
1. Comments from outside authors still count towards the 10-comment-only limit, so an outsider can end a watch just by commenting. In a YOLO chat, the message that says so is held.
2. A wake is still shown and admitted like a message the user typed, so a provider may still treat it as coming from the user despite the marking.

**Desired outcome:** David decides whether outside comments should count towards the limit, and whether wakes should get a separate transcript style (or a per-wake Allow card in YOLO).

**Evidence:** both are recorded as known gaps in `docs/chat/pull-request-watch.md` and `docs/documentation-review.md` on `v5/fix-prwatch-untrusted-comments`.

- [ ] Decision recorded for the comment-only limit
- [ ] Decision recorded for how wakes are styled and admitted

**Review:** not recorded.
**Prepared with:** Claude Code (Claude Opus 5.5).

## PR preview

**fix(pr-watch): quote only collaborator comments as untrusted data**

Pull request watch wakes quoted every commenter's text in a user turn that ended with "act on it". This change makes three things untrusted data:
- **Comments:** each comment or review is listed with its author's association. Text is quoted only for owners, members and collaborators, as escaped one-line untrusted data. Other authors get their login, association and link only.
- **Fork-controlled names:** check names and review file paths can be set by a fork's own files, so they are quoted the same way.
- **Hidden characters:** control, direction, joining, tag and other invisible characters are removed.

Every wake that lists pull request news says that UAM wrote it and that quoted GitHub text is untrusted information. In a YOLO chat, news that is only outside comments is held, including the message that ends a watch. A notification and a diagnostic line naming only the chat record the hold. The news is sent with the next other update or when the chat leaves YOLO. Undelivered wakes saved in the old format, which quoted everyone, are dropped once. Docs are updated in the same commits, and the YOLO hold is recorded as an open decision for David.

Related to the issue above (`Closes #N` once it's filed).

**Proof**
- `python3 scripts/check_docs.py`: passed (203 pages, 1512 local links, 6 providers × 32 capabilities).
- GCC 13 and Clang 18 syntax checks (`-fsyntax-only`, Clang with `-Wimplicit-int-conversion`) of `pr_watch_rules.cpp`, `pr_watch_service.cpp` and `tests/pr_watch_tests.cpp`: no errors or warnings in the changed code. GCC also checked `uam_control_service.cpp`. Stub CEF headers were used because the desktop build refuses Linux.
- Scratch Linux runner: `tests/pr_watch_tests.cpp`, minus the `UamControlPrWatchToolsActOnTheCallingChatOnly` case, compiled with `pr_watch_service.cpp`, `pr_watch_rules.cpp`, a minimal harness and link stubs. **All 27 cases passed** at `41319fcd`. Against the base revision the new cases don't compile, because the association and outside-only fields don't exist there.
- Not run:
  - `uam_core_tests`, CTest, the desktop build, and the macOS and Windows builds.
  - Live GitHub and live providers.
  - The runner build: configure failed because the proxy blocked the SQLite download from sqlite.org (403).

**Review:** 1 independent reviewer, 2 iterations. Independent Claude Opus 5.5: 11 suggested, 10 implemented. Fresh-context Claude Opus 5.5 re-check: 7 suggested, 7 implemented (one part declined: no test hook for diagnostics).

**Created with:** Claude Code (Claude Opus 5.5).

## Screens needing Before/After capture

None required. No React or UI code changed. Two visible pieces of text did change and can be captured if wanted:
- **Wake text in the chat transcript.** Before: a comment quoted as `reviewer: "…"` followed by "act on it". After: author with association, quoted untrusted text, and the new closing line.
- **Notifications panel entry for a held update.** New text, so there is no Before: "… new comments from non-collaborators. Not sent to the agent, because this chat runs in YOLO …".
