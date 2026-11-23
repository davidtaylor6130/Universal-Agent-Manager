# Report: v5/fix-svn-env-auth-errors

Final SHA of `v5/fix-svn-env-auth-errors`: `4f59e4e5` (three commits on `uam/v5.2-sync-main`: `6a45db0e` → `0c546956` → `4f59e4e5`).

Two fresh-context Opus reviews ran: the first on the whole branch, the second re-checking the fixes. Together they found 16 problems; 15 are fixed, and the 16th is reported as a test limit below. The fixes made after the re-check were tested here but were not re-reviewed, because only one re-check was asked for.

## Review rounds

**Round 1: full branch review (independent claude-opus-5-5, fresh context). 9 suggested, 8 implemented.**

1. *Blocker*, implemented. The log received up to 2 KB of svn's raw output: diff text, absolute paths, and URLs with user names.
   - Logging is now a separate call, `LogSvnFailure`.
   - It writes only svn's own error lines, or UAM's own error line, and never command output.
   - Home and workspace paths are shortened and user names are stripped from URLs.
2. *Should-fix*, implemented. A commit blocked by a hook lost the hook's reason. The hook's first line is now kept.
3. *Should-fix*, implemented. E170001 "Authorization failed" (signed in but no access) was told to save credentials. It now shows svn's own line.
4. *Should-fix*, implemented. Setting `SVN_SSH` replaced a user's own `[tunnels] ssh` command in their Subversion config. That entry now wins and `SVN_SSH` is left unset.
5. *Should-fix*, implemented. Expected failures were logged, and the five-minute limit only skipped an entry identical to the previous one.
   - The `@` probe miss, line counts and the context read are no longer logged.
   - The limit is now per key.
6. *Nit*, implemented. The docs said paths are relative everywhere. They now say this applies to the commit panel and Chat changes only.
7. *Nit*, implemented. Out-of-date and lead-line detection depended on English text. Both now work with translated svn output.
8. *Nit*, implemented. Shutdown did not stop the reads before a switch. It now does; the `svn switch` command itself is still never stopped.
9. *Nit*, declined in part.
   - Kept: the PR now states that the `env -i` and ssh integration runs use the test's own launcher, not `RunSvn`.
   - Declined: the extra core test with a fake ssh on PATH. On macOS the process service rebuilds PATH from terminal search folders, so a fake ssh placed on PATH would not reliably be the one svn finds.
- Also fixed, from the reviewer's note: when the update before a new chat fails for lack of credentials, it now gives the same advice.

**Round 2: re-check of the round-1 fixes (independent claude-opus-5-5, fresh context). 7 new suggestions, 7 implemented.**

1. *Should-fix*. Text such as `svn: E170001` inside a diff or log message counted as svn's own error. Such text is now ignored.
   - A line counts only when it starts with svn's prefix.
   - Output cut at the 4 MiB capture limit is not read as svn errors at all.
2. *Should-fix*. The VCS boundaries doc and the code map still said raw output goes to the log. Both are corrected.
3. *Nit*. UAM read the `[tunnels]` config differently from svn. It now follows svn's rules: section names must match exactly, the `ssh` option name ignores case, and indented lines are skipped (svn treats them as continuations).
4. *Nit*. Each svn launch reread the config files. The result is now cached per file until the file changes.
5. *Nit*. The five-minute log limit was shared across working copies. It now counts each working copy separately.
6. *Nit*. A switch could still start just after shutdown began. It now refuses to start once shutdown has been requested.
7. *Nit*. Home-folder shortening could cut inside a longer folder name, so `/Users/bobby` became `~by`. It now replaces only the whole home folder.
- The re-check also listed round-1 item 9 as still open. That is the partly declined item above, not a new suggestion.

## Issue previews (not filed)

Duplicate search found no matches. #913 and #597 are related but closed and different. Suggested labels for each: `bug`, `reliability`.

**1. `bug(vcs): SVN folder switching fails for working copies with non-ASCII paths`**
```markdown
Composer SVN folder discovery and switching run svn through a shell string without the shared SVN environment, so a working copy whose path has non-ASCII characters cannot be read when UAM starts without a locale (a Finder launch). svn's reason is also dropped.

**Reproduction:** Check out a working copy into a folder named `wcé`, start UAM without a UTF-8 locale, open the SVN folder selector and discover a parent.
**Expected:** Folders are listed and switching works.
**Actual:** "Could not verify the full SVN working-copy root." with no reason.

**Environment:** `uam/v5.2-sync-main`; `src/app/svn_branch_process.cpp` (`ExecuteCommand("svn " + JoinEscapedArgs(...))`), compared with `RunSvn` in `vcs_commit_service.cpp`.

**Evidence:** Observed with svn 1.14.3 on Linux: under `env -i`, `svn info` on `wcé` fails with E000022; with `LC_ALL=C.UTF-8` it succeeds. E155007 on macOS is user-reported.
**Reproduction status:** Reproduced with real svn on Linux; not attempted on macOS.

**Acceptance criteria:**
- [ ] Discovery and switching work on a non-ASCII working copy with no launch locale.
- [ ] A failed svn command's reason appears in the refusal.

**Review:** Self-triaged; no independent review of this report.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
```

**2. `bug(vcs): SVN authentication failures read as "Unable to connect"`**
```markdown
When svn has no saved credentials, or the server rejects them, UAM shows E170013 "Unable to connect to a repository" and drops the E170001 line that names the real cause. svn+ssh can also wait on an ssh prompt nobody can answer.

**Reproduction:** Serve a repository with `anon-access = none`, use a working copy with no saved credentials, then load History or discover SVN folders.
**Expected:** A plain message saying credentials are needed and how to save them.
**Actual:** "E170013: Unable to connect to a repository at URL '…'".

**Environment:** `uam/v5.2-sync-main`; `SvnFailureSummary` in `vcs_commit_service.cpp` returns the first E line.

**Evidence:** Observed: svn prints E170013 and then E170001 "Can't get password" (or "Password incorrect").
**Reproduction status:** Reproduced with a local svnserve and real svn on Linux.

**Acceptance criteria:**
- [ ] E215004, E170001 "Can't get…"/"Authentication error", and E170013 caused by ssh authentication show "SVN needs your credentials. Run svn info <url> once in a terminal to save them, then retry."
- [ ] E170001 "Authorization failed" (signed in, no access) does not suggest saving credentials.
- [ ] svn+ssh runs ssh in batch mode unless the user set SVN_SSH or configured their own `[tunnels]` ssh command.

**Review:** Self-triaged; no independent review of this report.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
```

**3. `bug(vcs): SVN commit, status and diff errors show raw multi-line output`**
```markdown
SVN commit, status and diff failures show svn's raw multi-line output with absolute paths, while History and Update already use a one-line summary.

**Reproduction:** Change a file, commit the same file from a second working copy, then commit the first copy from the panel.
**Expected:** "f.txt is out of date. Update, then commit again."
**Actual:** Three raw lines (E155011 "details follow", E155011 with the absolute path, E160028) plus the retained-scheduling note.

**Environment:** `uam/v5.2-sync-main`; `CommitSvnSelection` and `SvnOutput` in `vcs_commit_service.cpp`.

**Evidence:** Observed svn output from a real out-of-date commit.
**Reproduction status:** Reproduced with real svn on Linux.

**Acceptance criteria:**
- [ ] SVN commit, status and diff errors are one line from a shared helper.
- [ ] Out-of-date commits name the file relative to the workspace; a hook-blocked commit shows the hook's reason.
- [ ] Only svn's own error lines reach the diagnostic log, never command output.

**Review:** Self-triaged; no independent review of this report.

**Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
```

## PR preview (not opened)

**Title:** `fix(svn): shared environment, credential prompts and one-line errors`
**Base/head:** `uam/v5.2-sync-main` ← `v5/fix-svn-env-auth-errors`

```markdown
SVN commands now share one environment and one failure helper. Composer folder discovery and switching run through `RunSvn` (argv only, UTF-8 locale, cancellable before the switch itself), so a non-ASCII working copy works without a launch locale and svn's reason reaches the refusal. On this machine svn+ssh uses batch-mode ssh unless the user set `SVN_SSH` or configured their own `[tunnels]` ssh command. Commit, status, diff, history, update, origin and update-before-new-chat failures read as one line:
- missing or refused credentials say how to save them, while "Authorization failed" shows svn's line;
- an out-of-date commit names the file, and a hook-blocked commit shows the hook's reason;
- only svn's own error lines reach the diagnostic log, never command output, with paths shortened, URL user names removed and repeats limited.

Local repository-route reading also uses `RunSvn`. On SSH hosts the host's own `SVN_SSH` applies; this is documented as an implementation note.

Related to <issue 1>, <issue 2>, <issue 3>.

**Before/After needed** (not captured: the desktop app does not build on Linux):
1. Commit panel error after an out-of-date commit.
2. History failed-load alert on a repository that needs credentials.
3. Composer SVN folder selector on a non-ASCII working copy launched from Finder.

**Proof:**
- **Isolated SVN branch tests** (Linux, Debug, real svn 1.14.3 and svnserve): 2/2 pass (`ctest`).
  - Injected cases: svn's reason reaches every refusal; the log keeps only svn's lines, sanitised and rate limited.
  - A `wcé` working copy under `env -i PATH=…` discovers, switches and refuses with the shared environment. Without it, a control run fails with E000022.
  - With a stand-in ssh, `-o BatchMode=yes` is passed when nothing is configured; the user's `SVN_SSH` and the user's configured `[tunnels]` command each win.
  - svnserve with `anon-access = none` gives the credentials message.
  - These runs use the test's own shell launcher with the shared environment, not `RunSvn`. The production argv path is covered by a core case that runs only on macOS and Windows.
- **The 6 new `tests/svn_failure_tests.cpp` cases** pass on Linux. They ran in a scratch harness against the production `svn_failure.cpp` with the logger stubbed.
- **Real svn output run through the helper by hand:** an out-of-date commit, `svnserve -X` with `anon-access = none`, and a locked working copy.
- **`uam_runner` Release build** on Linux passes. A local SQLite copy was used because sqlite.org is blocked in the sandbox.
- **Syntax checks** pass for the changed `src/app` files and the new `svn_history_tests.cpp` case, with stubbed CEF headers.
- **`python3 scripts/check_docs.py`** passes.
- **Not run:**
  - the desktop build and `uam_core_tests`, including the core case for the non-ASCII runner without a launch locale;
  - the CEF bridge compile (`chat_branch_handlers.cpp`, `uam_query_handler`, `worktree_launch_preparation_service.cpp`);
  - macOS/Windows runs;
  - UI-V2, which this change does not touch.

**Review:** 2 independent reviewers, 2 iterations.
- Independent claude-opus-5-5 (round 1, full branch): 9 suggested, 8 implemented. Declined: a core test with a fake ssh on PATH, because macOS rebuilds PATH for child processes; the verification limit is stated above instead.
- Independent claude-opus-5-5 (round 2, re-check of the fixes): 7 suggested, 7 implemented. The round-2 fixes were tested but not re-reviewed.

**Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
```

## Screens needing Before/After capture

1. Commit panel: failed SVN commit for an out-of-date file (before: three raw lines with absolute paths; after: one line naming the file).
2. Commit panel History tab: failed load on a repository that needs credentials (before: "E170013: Unable to connect…"; after: the credentials advice).
3. Composer SVN folder selector: discovery on a non-ASCII working copy launched from Finder (before: "Could not verify the full SVN working-copy root."; after: folders listed).
