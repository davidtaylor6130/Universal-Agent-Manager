# Fix CLI View quit-into-draft and Codex stuck-busy — report

Both bugs are fixed on `v5/fix-cli-view-quit-draft-codex-busy`, one commit each on top of `uam/v5.2-sync-main`. The `v5/` push was accepted. No issues, PRs, comments or labels were created.

**Final SHA:** `9dde0430441feab89596b0811df27ddad92e91d4`
- `7f51ac0a` fix(terminal): stop CLI terminals without typing into a possible draft
- `9dde0430` fix(codex): keep CLI View from staying busy after an Enter that starts no turn

**Nothing native was compiled or run.** The desktop and test build refuses Linux, and both SQLite and the CEF SDK are blocked by the network policy, so even the runner configure fails. The new tests have not run on macOS or Windows; the PR's CI will be their first run.

## What changed

**Bug 1, on every platform.** UAM no longer types into a CLI terminal to stop it.
- Idle shutdown and Quit now call a new platform operation, `RequestCliTerminalProcessExit`. On macOS it sends a hangup; on Windows it terminates the process tree. The existing 2.5 s shutdown fallback is kept.
- The Windows default (graceful) stop no longer writes `/quit\r\n`; it terminates the process tree directly.
- `kCliTerminalQuitCommand` is removed.
- UAM still guesses whether the composer might hold a draft, but only to keep a background terminal open and hide its idle countdown. Safety does not depend on the guess: the first review showed that Enter in a file picker or an approval overlay doesn't submit, so a draft can survive an Enter.

**Bug 2.**
- An Enter during a tracked Codex turn (an approval or a queued message) keeps that turn, so its own completion ends the busy state. This also holds when Codex wrote the completion before UAM read it.
- A submission made with no tracked turn now has a 10 s receipt deadline. After it, the terminal shows Ready only if the Codex prompt classifier sees the idle composer drawn since the submission; otherwise it shows Unknown. It never stays Busy, so UAM no longer interrupts and force-stops an idle CLI.
- Unknown is not a dead end. A later idle composer gives Ready; a turn start or another submission gives Busy.

**Docs:** `terminal-view.md`, the Codex `terminal-view` row and a new known-gap row in `documentation-review.md` ("Terminal steering into a draft"). `python3 scripts/check_docs.py` passes.

## Review rounds

**Round 1, fresh-context Opus (claude-opus-5-5): 10 suggested, 8 implemented.**
- **Declined #3 (steering pastes into a draft).** This existed before the change. Refusing, holding or Ctrl-U-clearing steers based on a guess would either block legitimate steering or silently discard drafts. It is recorded as a known gap for your decision.
- **Declined #6 (brief Ready between a turn and its queued follow-up).** Arming a wait at turn completion would also add a 10 s busy tail after every approval. Codex starts a queued turn immediately, so the gap is normally under one poll.

**Round 2, fresh-context Opus re-check (claude-opus-5-5): 5 suggested, 4 implemented.**
- **Declined #4 (the draft guess keeps background terminals open after a one-key answer, arrow keys or Backspace).** This retention is deliberate: the scout recommended keeping a terminal open while it might hold a draft. Safety no longer depends on the guess, and the cost is resources, not behaviour.
- **Implemented #1 (blocker):** the new Windows test loop never read ConPTY output, so the exit could not be observed. The test now drains output while waiting.

## Issue preview 1 (not created)

**Title:** `bug(terminal): idle shutdown submits an unsent CLI View draft as a live prompt`
**Labels:** `bug`, `reliability`. No `security` label exists. `provider` doesn't fit because the bug is shared by all providers.
**Duplicates:** searched open and closed issues. #149 (idle timeout setting) and #333 (unsent drafts in structured chat) are related but not duplicates.

```markdown
UAM stops a background CLI View terminal by typing `/quit\r\n` into the provider's composer. If the user left typed but unsent text there, the quit command is appended to it and its Enter submits `<draft>/quit` as an ordinary prompt. The provider can start tool calls from it before UAM force-stops the process, and the stray turn is imported into the chat. Reported by a read-only code scout of `uam/v5.2-sync-main`.

**Reproduction:** Open a Codex, Claude Code or Gemini chat in CLI View (local workspace, default `cli_idle_timeout_seconds = 600`). Type `delete the build folder and redeploy` without pressing Enter and leave the window for just over 11 minutes.
**Expected:** The draft is never submitted; the terminal either stays open or ends without sending it.
**Actual:** Idle shutdown writes `/quit\r\n`; `delete the build folder and redeploy/quit` is submitted as a prompt, then the process is force-stopped 2.5 s later.

**Environment:** `uam/v5.2-sync-main` at `c252f9a9`; all providers; macOS and Windows.

**Evidence:** Code reading only. `RequestCliTerminalQuit` writes `kCliTerminalQuitCommand` with `WriteToCliTerminal` (`src/common/runtime/terminal/terminal_lifecycle.cpp`). Idle eligibility checks only lifecycle, pending steer and idle time; typing without CR/LF never leaves Idle. The explicit Quit (`HandleStopCli` with `quit: true`) uses the same path. On Windows, the default graceful `StopCliTerminalProcess` also types `/quit\r\n` into the ConPTY (`src/common/platform/platform_pty_windows.cpp`), which affects `StopAllCliTerminals` and relaunch. A POSIX PTY check confirmed that `draft` followed by `/quit\r\n` submits one line `draft/quit`.
**Reproduction status:** Not reproduced against a live provider CLI.

**Acceptance criteria:**

- [ ] No idle shutdown, Quit or stop path types into the provider CLI.
- [ ] Unsent text is discarded, never submitted, on macOS and Windows.
- [ ] A regression test with a real PTY child shows that no line is submitted when a draft is present.

**Review:** Scout report triaged by code reading; not independently reviewed as an issue.

**Prepared by:** claude-opus-5-5 in a Claude Code cloud session (host app not recorded), on behalf of David Taylor.
```

## Issue preview 2 (not created)

**Title:** `bug(codex): CLI View stays busy after an Enter that starts no new turn`
**Labels:** `bug`, `provider`, `reliability`
**Duplicates:** no match found. #884 (tool calls cancelled by the inactivity timeout) is related but not a duplicate.

```markdown
In a local Codex chat in CLI View, an Enter that does not start a new native turn leaves the terminal Busy indefinitely. The chat shows working while Codex is idle, switching to structured chat fails with "Cannot start structured chat while terminal fallback is busy.", and idle shutdown never runs. After the busy ceiling (default 120 minutes), UAM sends Ctrl-C to the idle CLI and force-stops it with a misleading "turn exceeded" message. Reported by a read-only code scout of `uam/v5.2-sync-main`.

**Reproduction:** Any of the following in a local Codex chat in CLI View: (A) ask for something that needs approval and accept the approval with Enter; (B) after a turn finishes, press Enter on the empty composer; (C) type `/status` (or choose an entry in `/model`) and press Enter.
**Expected:** The terminal returns to Ready when the turn ends or when no turn starts.
**Actual:** The terminal stays Busy until the busy ceiling interrupts and force-stops the CLI.

**Environment:** `uam/v5.2-sync-main` at `c252f9a9`; Codex CLI; local workspace.

**Evidence:** Code reading only. Every write containing CR or LF calls `CheckpointCodexTerminalSubmission`. That call moves the rollout offset to EOF, clears `codex_activity_turn_id` and sets `codex_activity_awaiting_turn`; `MarkCliTerminalTurnBusy` also clears the turn ID. Only a new `task_started` clears the wait, and a completion counts only when it matches the (now empty) tracked turn, so `PollCodexTerminalActivity` keeps returning Busy, which overrides prompt detection. There is no receipt deadline, unlike the OpenCode observer.
**Reproduction status:** Not reproduced against a live Codex CLI.

**Acceptance criteria:**

- [ ] An Enter during a running turn does not lose that turn's completion.
- [ ] A submission that starts no turn never leaves the terminal Busy; it shows Ready when the idle composer is visible, and Unknown otherwise.
- [ ] Structured chat is not refused as busy for an idle Codex CLI.

**Review:** Scout report triaged by code reading; not independently reviewed as an issue.

**Prepared by:** claude-opus-5-5 in a Claude Code cloud session (host app not recorded), on behalf of David Taylor.
```

## PR preview (not opened)

**Title:** `fix(terminal): never type into CLI View to stop it, and keep Codex from staying busy`
**Base/head:** `uam/v5.2-sync-main` ← `v5/fix-cli-view-quit-draft-codex-busy`

```markdown
Two native CLI View fixes, one commit each.

**Stopping a CLI never types into it.** Idle shutdown, Quit and the Windows default stop typed `/quit\r\n` into the provider composer, so an unsent draft was submitted as `<draft>/quit`. They now end the process through the platform: the new `RequestCliTerminalProcessExit` sends a hangup on macOS and terminates the process tree on Windows, with the existing 2.5 s fallback. A best-effort draft guess only keeps a background terminal open and hides its idle countdown; safety does not depend on it.

**Codex CLI View no longer stays busy.** An Enter during a tracked turn (an approval or a queued message) keeps that turn, so its completion ends the busy state. A submission that records no turn start within 10 s shows Ready if the idle Codex composer has been drawn since, and Unknown otherwise, never Busy. A later idle composer, a turn start or another submission ends Unknown. The inactivity ceiling no longer interrupts or kills an idle CLI.

Docs: `terminal-view.md`, the Codex `terminal-view` row, and a known gap for steering into a draft (pending David's decision).

**Proof:** `python3 scripts/check_docs.py` passes. Changed C++ translation units (`terminal_lifecycle.cpp`, `terminal_polling.cpp`, `codex_cli_provider_runtime.cpp`, `state_serializer.cpp`, `terminal_handlers.cpp`, `tests/terminal_tests.cpp`) were syntax-checked on Linux with g++ and clang++ against stub CEF headers, with no new errors relative to the base; `platform_pty_macos.cpp` was checked with stub Apple headers. A Python PTY check confirmed the POSIX fixture: typing `/quit\r\n` after `draft` submits a line, typing nothing does not. Not run: native build, `uam_core_tests`, CTest, macOS or Windows CI; `platform_pty_windows.cpp` was never compiled. New and changed cases awaiting CI: `CliTerminalComposerDraftClearsOnlyOnSubmittedLine`, `CliTerminalShutdownNeverSubmitsAnUnsentDraft` (added to `tests/ci_serial.txt`), `CodexNativeActivityKeepsRunningTurnAcrossMidTurnEnter`, `CodexNativeActivityReleasesSubmissionThatStartsNoTurn`, `CodexCliViewEnterWithoutNewTurnDoesNotStayBusy` and `CodexNativeSubmissionCheckpointSkipsOnlyTheAbandonedRecordTail`. No live provider CLI was used.

**Review:** 2 independent reviewers, 2 iterations.

- Independent claude-opus-5-5 (round 1): 10 suggested, 8 implemented. Declined: steering into a draft (recorded as a known gap for David) and a brief Ready between a turn and its queued follow-up (fixing it would add a 10 s busy tail after every approval).
- Independent claude-opus-5-5 (re-check): 5 suggested, 4 implemented. Declined: the conservative draft guess keeping background terminals open (deliberate retention; safety does not depend on it).

**Created with:** claude-opus-5-5 in a Claude Code cloud session (host app not recorded), on behalf of David Taylor.
```

**Screens that need Before/After capture** (no React code changed, but what these screens show does):
1. **Terminal View idle countdown:** with an unsent draft in the composer, the countdown should no longer appear.
2. **Codex chat header and Terminal View status after an approval accepted with Enter:** working before, Ready after.
3. **Codex Terminal View after Enter on an empty composer:** working indefinitely before; Ready or Unknown after.
4. **Switching that Codex chat to structured chat:** the "terminal fallback is busy" error should no longer appear when Codex is idle.
5. **Quit or idle shutdown with a draft present:** before, the native transcript gained a `<draft>/quit` turn; after, no stray turn.
