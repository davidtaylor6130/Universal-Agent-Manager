# Report: real-svn tests in CI (`v5/ci-real-svn-tests`)

**Final SHA of the work branch:** `756538d14dbd72ca79ef51c339e0fe5fefca9417` (3 commits on `uam/v5.2-sync-main`: `27140403`, `7a70fb98`, `756538d1`). No PR, issue or comment was created.

## Review rounds

Two fresh-context Opus reviewers (`claude-opus-5-5`), two iterations.

**Round 1, full review:** 11 suggested (1 blocker, 4 should-fix, 6 nits); 9 implemented.
- Blocker: the svn-branch integration test would fail on Windows because `_popen` runs `cmd /c` without `/S`, which strips the outer quotes from the quoted argv. Fixed with one extra outer pair of quotes.
- Should-fix:
  - `release.yml` ran real-svn cases without svn; it now installs svn and sets `UAM_REQUIRE_SVN=1`.
  - The keyword port-back case now asserts the exact baseline-refusal message and adds a control: a file without keywords ports normally.
  - The lock-only fix is recorded as an interim decision in `documentation-review.md`.
  - A reason is now given for the POSIX-only snapshot cases.
- Nits implemented:
  - Skip wording in `testing.md`.
  - The Linux job is gated on change selection and has a timeout.
  - SlikSVN accepts MSI reboot exit codes.
  - A real update-before-new-chat case on the managed Git snapshot path.
- Declined:
  - #9 Homebrew install cost: a cost, not a correctness problem.
  - #11 other fixtures now seeing a real svn: a residual risk, not a defect. I ran the two fixtures the reviewer named with svn installed and all 11 cases pass.

**Round 2, re-check of the fix commit:** 3 suggested (3 nits); 3 implemented.
- Corrected the lock wording: an edited locked file still shows `K`.
- Stated the real reason for the POSIX-only guard and recorded the Windows snapshot gap in `testing.md`.
- A missing Git now fails the snapshot cases under `UAM_REQUIRE_SVN=1` instead of skipping silently.

**Totals:** 14 suggested, 12 implemented.

## Issue previews

1. **`bug(svn): commit panel lists a lock-only file and reports a commit svn never made`**
   A file the working copy has locked but not edited appears as a changed row (`     K`, +0 −0). Committing it reports "SVN commit created." while nothing is published and the lock stays.
   **Reproduction:** commit a file, `svn lock` it, select it in the commit panel, commit.
   **Expected:** no row; a commit selecting it is refused.
   **Actual:** a row appears and the commit reports success.
   **Reproduction status:** reproduced with svn 1.14.3 through `VcsCommitService` in a Linux test build; fixed on `v5/ci-real-svn-tests` as an interim fix awaiting a decision on showing held locks.
   **Acceptance criteria:**
   - [ ] Lock-only rows are not listed.
   - [ ] A selection containing only a lock is refused.
   - [ ] An edited locked file still commits and releases its lock.

   **Review:** self-triage; the fix had independent review (see above).
   **Prepared by:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

2. **`question(svn): History marks your own commit "not updated yet" in a mixed-revision working copy`**
   History uses the root folder's revision. After a commit from the working copy, the root stays older than the committed file, so that commit is marked as incoming even though its content is already local.
   **Decision needed:** show a revision range, or the newest revision present?
   **Evidence:** pinned by `SvnRealMixedRevisionsUpdateToOneRevisionAndRefuseOutOfDatePublication`.
   **Prepared by:** as above.

3. **`question(svn): porting snapshot changes writes svn:needs-lock files without taking the lock`**
   Porting back with `git apply` makes a needs-lock file writable and edited without a lock. The commit still succeeds unless another working copy holds the lock.
   **Decision needed:** refuse such files, offer to lock them, or keep the current behaviour?
   **Evidence:** pinned by `SvnRealSnapshotChangesPortBackIntoTheWorkingCopy`.
   **Prepared by:** as above.

4. **`bug(svn): a chat edit cannot be ported back once the file's keyword expansion changes`**
   Snapshots store keywords already expanded, such as `$Revision: 1 $`. If an update changes that expansion, even through a property-only commit, the file no longer matches the snapshot's baseline. The port is then refused naming the file ("Source changed after the isolation baseline at k.txt…"), and the chat keeps its snapshot and edits. The same property-only change to a file without keywords ports normally.
   **Decision needed:** should the baseline comparison or the snapshot contract keywords first?
   **Evidence:** pinned by `SvnRealSnapshotPortBackRefusesWhenTheWorkingCopyMovedOn`.
   **Prepared by:** as above.

5. **`question(svn): should the commit panel show a lock held on an unedited file?`**
   After the lock-only fix, a lock on an unedited file is no longer visible in UAM; an edited locked file still shows `K`. Lock display and actions are panel features that need David's approval.
   **Prepared by:** as above.

## PR preview (`v5/ci-real-svn-tests` into `uam/v5.2-sync-main`; base to confirm)

**`test(svn): run real-svn cases in CI and cover untested working-copy paths`**

Real-svn tests used to return early, and so pass, wherever svn was missing, and no workflow installed it. CI and the release workflow now install the svn client in every job that runs these tests (apt `subversion`, brew `subversion`, choco `sliksvn`) and set `UAM_REQUIRE_SVN=1`, so a missing client fails. A new Linux job builds the SVN-branch service tests (no CEF) and runs their real-repository integration test. That test now reports Skipped without svn instead of not being registered, and checks that a pinned switch turns a mixed-revision working copy into a single revision.

Nine new real-svn tests in `tests/svn_working_copy_tests.cpp` cover:
- **Update before a new chat:** a mixed working copy reaches one revision; the update runs before the managed Git snapshot; a tree conflict blocks launch and a retry does not update again.
- **Tree conflicts:** in Update, Status and Commit.
- **Locks and needs-lock:** locks held elsewhere and `svn:needs-lock`.
- **Mixed revisions:** History, plus a commit that svn refuses as out of date.
- **Keyword expansion:** in status, line counts and diffs.
- **Porting back:** a snapshot's changes ported with `git apply`, including the refusal once keyword expansion changes.

The lock test found that a file that was only locked was listed as a change, and that committing it reported a commit svn never made. The status parser now skips such rows, as an interim fix. Five decisions are recorded in `docs/documentation-review.md`. Tests for broken externals, conflict leftovers, auth failure and the non-ASCII locale are left to their branches. The CMake 4.5 area is untouched.

**Proof:**
- **Linux test build:** the core binary can't be built on Linux, so I linked the core sources into a temporary Linux test build that isn't committed. It uses placeholder CEF headers and a process shim that combines stderr with output as macOS does; three existing tests needed that shim and are harness artifacts. With svn 1.14.3, `UAM_REQUIRE_SVN=1` and a non-root user, the SVN test files pass: 86 tests at the first commit, then the new file's 9 tests at HEAD. The new file is also clean under AddressSanitizer. With svn installed, `vcs_settings_tests` and `workspace_migration_alias_tests` pass (11 tests).
- **Lock fix regression:** the lock test and the parser test fail against the previous `vcs_commit_service.cpp` and pass after the fix.
- **Mutation check:** removing the tree-conflict column from update-before-new-chat makes the new launch test fail.
- **SVN-branch tests on Linux:** CTest 2/2 pass. Without svn the integration test exits 77 (Skipped); with `UAM_REQUIRE_SVN=1` it exits 1.
- **Other checks:** clang++ and g++ syntax checks are clean. `check_docs.py` passes, including under a non-UTF-8 locale. `select_native_tests_test.py` and `native_test_runner_test.py` pass.
- **Not run:**
  - macOS and Windows builds, Apple clang and MSVC compiles, and CI itself.
  - The runner build: the sandbox blocks the sqlite.org download, and `src/common` and `src/remote` are unchanged.
  - UI-V2 checks: no UI code changed.

Windows real-svn tests run for the first time in this PR's CI; the managed-snapshot tests are POSIX-only.

**Review:** Reviewed by 2 agents over 2 iterations.
- Agent 1: claude-opus-5-5 — suggested 11; implemented 9 (declined: Homebrew install cost, and a residual-risk note checked locally).
- Agent 2: claude-opus-5-5 — suggested 3; implemented 3.

**Created with:** Claude Opus 5.5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

## Screens needing Before/After capture

The commit panel with a locked, unedited file in an SVN working copy. Before: the file shows as a row with +0 −0. After: no row. An edited locked file still shows a row with `K` in its status. No UI code changed; the difference comes from the native status parser.
