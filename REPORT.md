# #520 SVN branch creation (first slice): report

SVN branch creation from the composer branch picker is built and pushed. It went through two independent Opus review rounds and was not checked again after the second round's fixes. I opened no PR, issue or comment.

- **Branch:** `v5/feat-520-svn-branch-create` (the `v5/` prefix was accepted).
- **Final SHA:** `4d219350ec7f8ec22d1d6ef5d79d6f97ba2b80d5`, one commit.
- **Base:** `origin/v5/refactor-519-vcs-interface` (`da59643e`), which sits on `origin/uam/v5.2-sync-main`. That branch already routes the chat branch picker through `VcsProvider`, including the create flag, so this work extends it. The PR therefore depends on the #519 branch landing first.

## What was built

- **Where the branch goes:** `CreateSvnBranch` reads the branches folder from `svn_repository_layouts`, matched by root URL or UUID; otherwise it uses `trunk`/`branches`. A project folder is kept, so `^/shop/trunk` gives `^/shop/branches`.
- **How it is created:** one server-side `svn copy` commit pinned to the source revision, with no `--parents`. UAM confirms the commit from the new path's first log entry, then switches the working copy straight to that commit with the same checks as folder switching.
- **What is refused before anything is committed:**
  - invalid names and multi-line messages;
  - a missing branches or trunk folder;
  - an existing name, including one that differs only in letter case;
  - a name the fail-closed `svn list --search` could not check;
  - a working copy whose files are at mixed revisions, or that committed deletions after its revision (copying it would bring the files back);
  - externals in the working copy or the trunk source;
  - anything the switch itself would refuse.
- **Uncertain outcomes are reported honestly:** if a copy lands inside a folder of the same name created at the same moment, or the server finishes a commit after the client gave up, the message says so.
- **Decision applied (David, 2026-11-23):** the picker asks every time whether to copy the working copy (URL@revision, the default) or **Trunk HEAD**. UAM pre-fills an editable one-line commit message. Every failure is one notification line through `reportAppFailure`, and success uses `reportAppNotice`.
- **Interface changes:**
  - `kSvnCapabilities` now declares `BranchCreate`.
  - `VcsProvider::DefersBranchCreation()` keeps the "create on first message" behaviour for Git only.
  - The SVN listing reports where a new branch would go.
- **Shared helpers:**
  - `svn_layout_lookup.h` holds the layout lookup with no JSON dependency, so the isolated SVN test build can compile it.
  - `SvnCommandChangesRepository()` names the commands a stoppable runner must let finish.
- **Docs:** updated `workspace-actions.md`, `svn.md`, `vcs-boundaries.md` and `building.md`. The #520 decision is recorded in `documentation-review.md` with today's date. The other decisions in the brief (#518, #965 and so on) are not recorded there, because this work neither implements nor depends on them.

**Merge note for `v5/fix-svn-env-auth-errors`:** that branch's stoppable runner exempts only `switch` from cancellation. It must use `SvnCommandChangesRepository` (or at least exempt `copy`), otherwise a stop could interrupt the branch commit. This is recorded in `documentation-review.md` and the commit message. Its 4-argument runner signature is already accepted by the new tests and the copy/switch calls. Expect plain text conflicts there and with the other three SVN branches in `vcs_provider_declaration.h`, `svn.md` and `building.md`.

## Review rounds

| Round | Reviewer | Suggested | Implemented | Not implemented |
|---|---|---|---|---|
| 1 | Independent claude-opus-5-5, fresh context | 13 (6 should-fix, 7 nits; 0 blockers) | 12 | 1 should-fix: making `copy` non-cancellable means editing `v5/fix-svn-env-auth-errors`, which I may not edit. In round 1 I only added a code comment; round 2 replaced it with a shared helper, a test and a recorded note. |
| 2 (re-check) | Independent claude-opus-5-5, fresh context | 9 (5 should-fix, 4 nits; 0 blockers) | 9 | none |

Round 2's fixes were not reviewed again, because the brief allows one re-check. They cover:
- the committed-deletion refusal;
- longer copy and switch timeouts;
- confirming the commit even when the copy reported failure;
- checking the name with `--search`;
- handling a failed trunk externals check separately;
- the runner helper and the recorded merge note;
- logging every outcome;
- a message that mentions the commit message;
- disabling the working-copy option at the repository root;
- cross-checking the printed commit revision.

## New issue previews (not filed)

**1. `bug(svn): folder switch reports "needs inspection" for folders whose names contain + or @`**

> When a working copy is switched to an SVN folder whose name contains `+`, `@` or other characters svn leaves unescaped in URLs, the switch succeeds but UAM reports "SVN switching needs inspection". The folder list also never marks such a folder as current.
>
> **Reproduction:** in a repository with `^/lines/a+b`, discover `^/lines` in the composer picker and switch to `a+b`.
> **Expected:** the switch is confirmed and `a+b` shows as current.
> **Actual:** the switch happens but is reported as needing inspection.
>
> **Evidence:** `SwitchSvnBranch` (`src/app/svn_branch_service.cpp`, the final `current.url != target` check) and `Discover`'s `current` flag compare svn's canonical URL with UAM's own percent-encoding (`UrlPath`). The independent reviewer of #520 reproduced this mismatch with real svn 1.14 for branch creation, which now compares decoded paths instead. The folder-switch path has not been reproduced separately.
> **Reproduction status:** reasoned from the code and the creation reproduction only.
>
> **Acceptance criteria:**
> - [ ] Folder switching and the current-folder mark compare decoded repository paths.
> - [ ] A real-svn case covers a `+`/`@` folder.
>
> **Prepared by:** claude-opus-5-5 in Claude Code (cloud session, Claude Code harness), on behalf of David Taylor.

**2. `bug(svn): composer folder switch is limited to 10 seconds`**

> `SwitchSvnBranch` runs `svn switch` through `Probe::Run`, which caps each command at 10 s within a 30 s total. Switching a large working copy over the network can be killed partway, leaving a locked, half-switched working copy that needs inspection.
>
> **Desired outcome:** the switch gets a timeout sized for a full working-copy switch, as branch creation now does (600 s), and reads stay bounded.
>
> **Evidence:** reasoned from the code. Not reproduced: it needs a slow remote server.
>
> **Prepared by:** claude-opus-5-5 in Claude Code (cloud session, Claude Code harness), on behalf of David Taylor.

## PR preview (not opened)

**Title:** `feat(svn): create SVN branches from the composer picker`

> SVN working copies could switch between existing folders but not create a branch in the app. The composer branch picker now offers **Create SVN branch…** for local SVN working copies. It asks each time whether to copy the working copy at its revision (the default) or trunk at its newest revision, and pre-fills an editable commit message. UAM then makes one server-side `svn copy` into the branches folder from the repository's layout in Settings and switches the working copy to the new branch. Everything the copy or the switch would refuse is checked before anything is committed, and the commit is confirmed from the repository log before switching. Failures are one notification line.
>
> This is the first #520 slice. Merging branches, SVN parity in the new-chat selector, greyed-out reasons and SSH working copies come later. It builds on the #519 branch-picker interface (`v5/refactor-519-vcs-interface`), which must land first. The picker form is new UI and awaits Claude review and David's approval.
>
> Related to #520.
>
> **Screens needing Before/After capture**, from the desktop app in light and dark themes (not captured: the desktop app cannot be built on Linux):
> 1. SVN composer picker open with no folder discovered. Before: no create row. After: "Create SVN branch…".
> 2. Create form default: name, Working copy selected, the "Copies … to ^/branches/<name>…" line, UAM's default commit message.
> 3. Create form with Trunk HEAD selected and an edited message.
> 4. Create form for a repository-root working copy: Working copy disabled with its tooltip.
> 5. A failure notification (for example a missing branches folder): transient alert and bell entry.
> 6. The success notice, and the composer branch label showing the new branch path and revision.
>
> **Proof:**
> - **Isolated SVN tests:** `UAM_SVN_BRANCH_TESTS_ONLY` CTest passes 4/4 on Linux: `uam_svn_branch_service`, `uam_svn_branch_integration`, the new `uam_svn_branch_create` fake-repository cases and the new `uam_svn_branch_create_integration`, which uses real svn 1.14 against disposable `file://` repositories.
>   - The real-svn cases cover: a working-copy source, a trunk source, an existing name refused with nothing committed, a `shop+@ x` project folder, and a committed deletion refused until the working copy is updated.
>   - On Linux this configuration needs the root CMakeLists guard change from `v5/ci-real-svn-tests`; it was applied locally only for the run, not committed.
> - **Runner:** `uam_runner` builds. sqlite.org is blocked in the sandbox, so it used a sqlite amalgamation from npm.
> - **Frontend:** `npx vitest run` passes (239 files, 4,221 tests); `tsc --noEmit` and `npm run build` pass.
> - **Docs:** `python3 scripts/check_docs.py` passes.
> - **Not run:**
>   - `chat_branch_handlers.cpp`, `vcs_commit_service.cpp` and `tests/vcs_provider_tests.cpp` were only syntax-checked with g++ against CEF headers from GitHub that are newer than the pinned version, with generated headers stubbed. The CEF CDN is blocked here.
>   - The desktop app and `uam_core_tests` were not built or run (macOS/Windows only).
>   - No GUI check, no SSH, and no Windows run of the `cmd.exe`-based runner with unusual commit-message characters.
>
> **Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 22 suggested, 21 implemented. One round-1 item needed a change to a sibling branch; it was handled in round 2 with a shared helper, a test and a recorded merge note.
>
> **Created with:** claude-opus-5-5 in Claude Code (cloud session, Claude Code harness), on behalf of David Taylor.
