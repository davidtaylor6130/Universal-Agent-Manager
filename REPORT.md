The review loop on `v5/fix-svn-update-conflicts` is finished and pushed. Two fresh-context Opus agents reviewed the branch over two rounds: 14 suggestions, 13 implemented, 1 declined.

**New SHA:** `56df5e05ed636ffa5bc1f9df4765a02695977cb5`. The branch holds `2ae9fe5b` (the fix), `94402320` (round-1 fixes) and `56df5e05` (round-2 fix).

## Review rounds

| Round | Reviewer | Suggested | Implemented |
|---|---|---|---|
| 1 | Independent claude-opus-5-5, fresh context | 13 (1 blocker, 6 should-fix, 6 nits) | 12 |
| 2 (re-check of round 1) | Independent claude-opus-5-5, fresh context | 1 (nit) | 1 |

**Round 1 blocker:** the broken-external fix did not work on SSH. The SSH helper keeps svn's error output in a separate field, so E205011 was never seen there. The update outcome and the external warnings now read both streams.

**Round 1 should-fixes:**
- **Link escape:** a local resolve could follow a link out of the workspace; it is now refused.
- **Untracked folder:** an untracked chat folder was silently "updated"; both updates now refuse it.
- **Externals before a new chat:** these are only logged, so svn.md now says so.
- **Fencing:** resolve now refuses and fences chats that share the working copy, the same way Update does.
- **Focus:** focus now returns after the confirmation closes.
- **Test gap:** a test now covers refusing every new file when conflict details can't be read.

**Round 1 nits:**
- Implemented: a late result no longer closes a newer dialog; confirmation wording now matches file, property or tree conflicts; the moved declarations now live in svn_history.h; the conflicted checkbox stays focusable and the resolve button gives a disabled reason; menu ids come from `useId`.
- **Declined:** skipping `svnversion` on the line-stats status pass. That second pass replaces the first, so the `r3:5` range would revert to a single revision. `svnversion` costs about the same as `svn status` (61 ms vs 48 ms on 20k files). The re-check agreed.

**Round 2:** the re-check found 12 of 13 fixed and agreed with the one decline. Its one nit: a successful resolve whose dialog had been replaced also skipped the refresh, so the row stayed marked Conflict. It now refreshes; this is `56df5e05`.

## Issue previews

**1. `bug(svn): a broken svn:externals definition makes Update fail and hides conflicts`**

With an unreachable `svn:externals` definition, every Update reports a failure, and the conflicts it left are never shown. "Update before new chat" then blocks every new chat. This came from your review of `uam/v5.2-sync-main` with svn 1.15.

**Reproduction:** Two working copies of one repository. In the first, edit `a.txt`, delete `d/x.txt`, set `svn:externals` to `ext <missing-url>` on the root and commit. In the second, edit `a.txt` and `d/x.txt`, then run Update.
**Expected:** the update reports r2, shows the text and tree conflicts, and warns that external `ext` could not be updated.
**Actual:** svn exits 1 with E205011 after updating the working copy. UAM treats any non-zero exit as failure, skips the conflict check, and shows "W205011: Error handling externals definition for 'ext':" with its trailing colon.

**Environment:** `uam/v5.2-sync-main`; svn 1.15 (your report), svn 1.14.3 (sandbox).

**Evidence:** `src/app/svn_history.cpp` ~367-378; `vcs_commit_service.cpp` ~2309-2331 (`SvnFailureSummary`); `worktree_launch_preparation_service.cpp` ~186-190.
**Reproduction status:** Reproduced with svn 1.14.3: exit 1, E205011, `C a.txt` and `   C d/x.txt` in the output.

**Acceptance criteria:**
- [ ] E205011 alone counts as a finished update, locally and over SSH; conflicts and the revision are still reported.
- [ ] "External 'x' could not be updated." appears on its own line.
- [ ] A new chat starts when the status check after the update shows no conflicts.

**Review:** Self-triage only; no independent review.

**Prepared by:** claude-opus-5-5 in a Claude Code cloud session (Claude Code; host app not recorded), on behalf of David Taylor.

**2. `bug(svn): conflict leftover files are listed as new files and can be committed`**

After a text conflict, svn's `.mine` and `.rN` copies appear as new `?` rows and can be selected and committed. This came from your review, and you confirmed the add/commit sequence published them.

**Reproduction:** Create a text conflict with Update, then select `a.txt.mine` in the panel and commit.
**Expected:** the copies are hidden and refused; the conflicted row is labelled, with ways to resolve it.
**Actual:** the copies are listed; a `?` row is scheduled as an add and committed.

**Environment:** `uam/v5.2-sync-main`.

**Evidence:** `vcs_commit_service.cpp` ~2334-2364 (parser) and ~697-704 (`?` is scheduled as an add); `VcsCommitPanel.tsx` ~36-39 and ~379.
**Reproduction status:** In the sandbox I reproduced the `?` rows and the `svn info --xml` conflict entries. I did not re-run the publish; your confirmation stands.

**Acceptance criteria:**
- [ ] Leftover copies are hidden and refused for commit, locally and over SSH.
- [ ] Conflicted rows are labelled "Conflict" with a tooltip.
- [ ] `svn resolve --accept working|theirs-full|mine-full` is available, each behind a confirmation.

**Review:** Self-triage only; no independent review.

**Prepared by:** claude-opus-5-5 in a Claude Code cloud session (Claude Code; host app not recorded), on behalf of David Taylor.

**3. `bug(svn): update before new chat times out on large working copies and uses a different scope`**

Update before new chat gives info and status reads only 10 s. Its timeout message always blames the update, even when status timed out. It also updates the whole checkout while the panel updates only the chat's folder, which leaves mixed revisions that the panel doesn't show (it shows only the root revision). This came from your review.

**Expected:** reads use the 120 s bound, the error names the step that failed, both paths use one documented scope, and mixed revisions show as `r3:5`.

**Environment:** `uam/v5.2-sync-main`.

**Evidence:** `worktree_launch_preparation_service.cpp` ~101, ~103-105, ~142, ~191; `svn_history.cpp` ~367; `vcs_commit_service.cpp` ~1433.
**Reproduction status:** Confirmed by reading the code; the 10 s timeout was not reproduced on a large checkout.

**Review:** Self-triage only; no independent review.

**Prepared by:** claude-opus-5-5 in a Claude Code cloud session (Claude Code; host app not recorded), on behalf of David Taylor.

## PR preview

**Title:** `fix(svn): finish updates past broken externals and keep conflict leftovers out of commits`
**Base / head:** `uam/v5.2-sync-main` ← `v5/fix-svn-update-conflicts` (`56df5e05`)

A broken `svn:externals` definition no longer fails Update, locally or over SSH. svn exits 1 with E205011 after updating the working copy, so UAM now treats that as finished. It still reports the conflicts and the revision, and names each skipped external on its own line in the panel; before a new chat they are only logged. Update before new chat starts when the status check afterwards shows no conflicts.

The copies svn leaves beside a conflict are hidden and refused at commit. If they can't be read, every new file is refused instead. Conflicted rows are labelled "Conflict" and offer three ways to resolve, each behind a confirmation. Resolve stays inside the workspace and is fenced like Update against chats that share the working copy.

Update before new chat now uses the panel's read and update bounds and names the step that failed. It updates the chat's folder, the same scope as the panel's Update. A folder SVN doesn't track is refused, because svn would report "At revision N." and change nothing. Mixed revisions show as `r3:5`.

Closes #<issue 1>, #<issue 2>, #<issue 3>

**Proof:**
- **Real-svn cases:** svn 1.14.3, not 1.15. Ran all 21 `svn_history_tests.cpp` cases through a temporary Linux test harness, outside the repository, against real svn, with no skips: 21/21 pass. They cover:
  - the SSH-shaped update outcome
  - the broken external and the conflicts it leaves
  - hidden and refused leftovers
  - refusing every new file when conflict details can't be read
  - refusing a resolve path that leads out of the workspace through a link
  - the untracked folder for the panel's Update
  - `r2:3` after a partial commit
  - the resolve choices
- **Mutation checks:** restoring the old exit-code rule, removing the leftover filtering, or skipping `svnversion` each makes the end-to-end case fail.
- **Launch step:** the committed `PrepareSvnUpdate`, copied verbatim into a scratch test, ran against real svn. It passes a broken external, updates only the chat's folder, refuses an untracked folder, blocks on conflicts and names the failed step. The base-branch version fails the broken-external case with `SVN preparation failed (E205011 …)`.
- **Native compile:** all changed native files and tests compile with no errors (syntax-only) under g++ and clang++-18, against CEF's upstream public headers with a few generated headers stubbed. `uam_runner` builds; a local SQLite 3.52 was used because sqlite.org is blocked here.
- **UI:** `npx tsc --noEmit -p .`, the full Vitest run (239 files, 4,225 tests) and `npm run build` pass.
- **Docs:** `python3 scripts/check_docs.py` passes.
- **Not run:**
  - the `uam_core_tests` binary
  - the new launch-preparation test cases in their real form
  - the desktop app, macOS or Windows
  - the SSH route against a real host
  - CI
  - svn 1.15

**Review:** 2 independent reviewers, 2 iterations. David's acceptance pending.

- Independent claude-opus-5-5 (review): 13 suggested, 12 implemented. Declined: skipping `svnversion` on the line-stats pass, which would revert `r3:5` to a single revision.
- Independent claude-opus-5-5 (re-check): 1 suggested, 1 implemented.

**Created with:** claude-opus-5-5 in a Claude Code cloud session (Claude Code; host app not recorded), on behalf of David Taylor.

## Screens that need Before/After capture

- **Commit panel file list** with a text, a property and a tree conflict: "Conflict" label, tooltip, unavailable checkbox, resolve button and its menu. Before: `C` rows plus `.mine`/`.rN` rows.
- **Resolve confirmation dialog:** Use the repository version (danger) and the error state.
- **Update notice** with "External 'x' could not be updated." lines. Before: the W205011 failure.
- **Panel header revision** with `r3:5` and its tooltip. Before: the root revision only.
- **New-chat launch error text:** a step that failed (e.g. "SVN status timed out…") and the untracked-folder message. Before: always "SVN update timed out".
