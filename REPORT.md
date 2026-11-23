# SSH ACP fixes: initialize refusal and duplicate replies

**Work branch:** `v5/fix-ssh-acp-refusal-and-duplicates` (base `uam/v5.2-sync-main`)
**Final SHA:** `5f93e6df`

| Commit | What it does |
|---|---|
| `918479ee` | fix(remote): stop the SSH proxy replaying delivered output on stop |
| `6556259f` | fix(remote): treat a provider refusal at initialize as final on SSH |
| `cb1cb723` | Round 1 review fixes |
| `5f93e6df` | Round 2 review fixes |

## Confirmed causes

**Bug 1: initialize refusal.** When Copilot refused (SDK protocol is not 3), `HandleAcpResponse` called `RecoverDisconnectedRemoteAcpTransport`. On SSH that relaunched the remote process and reconnected. The second process then failed the same way, and the error shown was "invalid native frame" instead of the refusal.

The refusal is now final:
- **Discovery:** records the provider's message as the refresh failure, then calls `StopBackgroundModelDiscovery`.
- **Chat:** stops the transport without remote recovery and keeps the message.
- **A first prompt waiting for the session:** comes back as an unsent message with Retry.
- **Input queued behind it:** is held for Resume or Remove.

Nothing relaunches on its own.

**Bug 2: duplicate replies.** The cause is the SSH proxy's stop drain (`stop_owned_process` in `src/remote/runner_proxy.cpp`), not `ProcessBufferedAcpStdoutAfterStop`.
- The drain polled the runner with no cursor, so it read from the last acknowledged offset.
- Discovery stops on its `session/new` answer before acknowledging that batch, so the batch was written to the app twice.
- A chat stopped mid-stream replayed its notifications the same way.

The drain now starts after the last batch already written, or at the cursor the chat saved when it reattached, and advances after each read. #955 is related but is a different post-stop bug; it is referenced only.

## Tests

**Run:**
- **Linux runner build:** passes. SQLite came from an npm package, because the network policy blocks `sqlite.org`.
- **Existing helper scripts:** `test_remote_runner_memory.py` and `test_remote_runner_closed_bridge.py` pass.
- **New `scripts/test_remote_runner_stop_replay.py`:** I ran it against a scratch Linux build with the macOS/Windows proxy guard removed. That build is not committed.
  - Before the fix it fails: the reply is written twice, and a streamed notification is written twice.
  - After the fix it passes, including after the review changes.
  - With the normal Linux runner it reports SKIPPED.
- **Docs:** `python3 scripts/check_docs.py` passes.

**Not run:** two native tests, which need the macOS or Windows test build:
- `RemoteRunnerProxyStopDoesNotReplayADeliveredUnacknowledgedBatch`
- `AcpRemoteInitializeRefusalIsFinalWithoutRemoteRecovery`

## Unverified

- **Desktop build:** no desktop code was compiled, and the native tests were never compiled or run.
- **Live hosts:** nothing was run against a live host or a real outdated Copilot CLI.
- **Reattach drain:** starting the drain at the saved cursor after a reattach has no test.
- **Codex rate-limit snapshot:** the 6 lost snapshots are probably the duplicate replay, but this is not proven.
- **Behaviour change:** a local chat whose waiting prompt meets a Copilot initialize refusal now returns the prompt unsent, where it used to retry.

## Review

**Round 1** (independent `claude-opus-5-5`, fresh context): 5 suggested, 5 implemented.
- **Blocker:** the test's catalogue had no remote host set.
- **Should-fix:** an endless relaunch loop on SSH when a prompt was waiting.
- **Should-fix:** the docs overstated verification.
- **Nit:** replay after a reattach.
- **Nit:** a timing race in the script's skip check.

**Round 2 re-check** (independent `claude-opus-5-5`, fresh context): 4 suggested, 2 implemented.
- **Implemented, blocker:** the new test could never confirm the stop, because the fixture held a duplicate of the process input.
- **Implemented, should-fix:** a held lone prompt had nothing to Resume. It now returns unsent with Retry.
- **Declined, nit:** route initialize error replies through the same path. That would break an existing, tested local design that retries a waiting prompt after an initialize error (`CodexInitializeErrorStopsTheStaleTransportAndRetriesAnUndeliveredPrompt`). I corrected the code comment instead.
- **Declined, nit:** add a test for the reattach drain. Building an attach-with-saved-cursor proxy harness is larger than this fix; the gap is recorded in `ssh.md`.

## Issue previews

These follow your skill's issue format. I didn't search for duplicates and didn't file anything.

### 1. `bug(remote): SSH initialize refusal is reported as an invalid native frame`

> On SSH, a provider that refuses at initialize (Copilot without SDK protocol 3) is treated as a disconnect. UAM relaunches it, then reports "Copilot returned an invalid or oversized native protocol frame" instead of the refusal.
>
> **Expected:** the refusal is final and its message is shown. For discovery it is recorded as the refresh error.
>
> **Actual:** `remote_transport_recovery_scheduled` → relaunch → reconnect → `invalid_native_frame` → `ignored_unknown_id request_id=1`.
>
> **Evidence:** all 5 SSH Copilot discovery attempts (10 processes) in a real install's logs.
>
> **Reproduction status:** not reproduced live.
>
> **Acceptance criteria:**
> - [ ] No reconnect after an initialize refusal on SSH.
> - [ ] The refusal message is shown, or recorded for discovery.
> - [ ] A waiting first prompt returns unsent.
>
> **Review:** self-triage only.
>
> **Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

### 2. `bug(remote): SSH stop replays output the app already received`

> When an SSH session stops while a batch of output has been written but not yet acknowledged, the proxy writes that batch again. Discovery logs `ignored_unknown_id request_id=2`, and a chat stopped mid-stream can replay notifications.
>
> **Evidence:** all 15 SSH OpenCode and all 75 SSH Codex discovery sessions in the logs, against 0 of 574 local sessions.
>
> **Reproduction status:** reproduced with the proxy script on Linux, using the scratch proxy build.
>
> **Acceptance criteria:**
> - [ ] No line is delivered twice on stop, including a mid-stream stop.
>
> Related to #955.
>
> **Review:** self-triage only.
>
> **Prepared by:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.

## PR preview

**Title:** `fix(remote): make SSH initialize refusals final and stop replaying delivered output`

> A Copilot refusal at initialize on SSH is now final. There is no reconnect, the refusal message is kept (or recorded as the discovery error), a waiting first prompt returns unsent with Retry, and queued input is held. The SSH proxy's stop no longer re-sends output the app already has, so discovery replies and mid-stream notifications are not processed twice.
>
> **Proof:**
> - The Linux runner build passes, and the existing memory and closed-bridge scripts pass.
> - `test_remote_runner_stop_replay.py` fails before the fix (the reply and a streamed notification are written twice) and passes after it, using a scratch Linux build with the proxy enabled.
> - `check_docs.py` passes.
> - **Not run:** the desktop build, both new native tests (they need macOS or Windows), and any live host.
>
> **Review:** 1 independent reviewer, 2 iterations. Independent claude-opus-5-5: 9 suggested, 7 implemented. The 2 declined nits: changing initialize error replies would break an existing tested local retry; the reattach test needs a larger harness.
>
> **Created with:** claude-opus-5-5 in Claude Code on the web (Claude Code), on behalf of David Taylor.
>
> Related to #955.

## Screens needing Before/After capture

None: no UI code changed. One visible state could optionally be captured: a Copilot chat whose first prompt shows as unsent with Retry after an initialize refusal.
