# V5.2 review, round 2: #998, #902, #513, #353

#998 has one blocker. #902 can be approved. #513 and #353 can be approved after fixes. I compared each branch with `origin/uam/v5.2-sync-main`.

**What I couldn't verify:** nothing was compiled or run. I tried to build the remote helper (`uam-runner`) to run the new #353 helper test, but it needs an SQLite download that this sandbox's network blocks. I checked includes, signatures, guards and test expectations by reading the code. `scripts/check_docs.py` passes on all four branches.

## v5/fix-998-subagent-parent-link (#998)

1. **Blocker:** `src/app/managed_subagent_control_service.cpp:997-1014` (`expired_run_child`). Port back/Discard family capture treats "not in this index" as "record expired", but the index is incomplete:
   - It only holds the live run ledger and the restart-inspection catalogue.
   - The catalogue only includes v2 records with the initial history shape (`InitialHistoryShape`). Records are left out when they are nested, carry a delegation grant, have more than 33 turns or an incomplete binding.
   - When the catalogue hasn't loaded yet, nothing from disk is checked at all.
   - Startup drops unreadable v1 records and only writes them to the log.

   So a finished v2 child whose record still exists gets admitted by its saved link and skips the "Native follow-up capable subagents" refusal. That contradicts the commit message, the docs and the issue's "still blocks" criterion.

   **Fix:** do what the SSH path already does. Admit by saved link only against a complete read-only ledger observation:
   - Either record every retained run ID and transcript ID, plus a completeness flag, in `LoadRestartInspection`, or call `AgentRunLedger::LoadAllReadOnly` from the capture path.
   - Require the child's run ID and chat ID to be absent from that observation, and refuse when the read was incomplete.
   - Add tests for a retained v2 record with a non-initial shape and for an unreadable record.
2. **Should-fix:** `src/app/worktree_launch_preparation_service.cpp:268`. The saved-link branch for SSH preparation doesn't check `acp_queue_held`, `acp_dispatched_queued_prompt_count` or pending automation approval. The family path does check these (through `CleanupSettled`), and the docs say held input blocks. **Fix:** share one settlement predicate between both paths, and add a held-input case to `RemoteWorkspaceReferenceAdmissionSkipsExpiredRunChildBySavedRootLink`.
3. **Nit:** `tests/git_worktree_port_method_tests.cpp` (`AddExpiredRunWorktreeChild`). The v2 case runs with `managed_initial_history` never loaded, so it tests a state production doesn't normally reach. **Fix:** call `ManagedSubagentControlService::LoadRestartInspection(fixture.app)` in the fixture.
4. **Nit:** the fourth acceptance criterion is met only for v2 families whose records have expired. Say so on #998 and keep the issue open. The commit already uses "Refs".

No compile errors found: the aggregate initialiser order, the changed signature's only call site and the container types all check out. User messages are unchanged.

**Verdict:** approve after fixes. **4 suggestions.**

## v5/feat-902-subchat-import-gaps (#902)

1. **Nit:** `tests/empty_native_import_tests.cpp:479`. The test covers a changed host connection but not the other two new guards. Add cases for a workspace folder whose directory changed since the scan (`m_remote_folders` mismatch) and a host whose runner is no longer ready (`runner_status != "ready"`).
2. **Nit:** this closes only part of "Backfill for remote chats": it works only when this run of UAM already scanned that host. Update the #902 checklist to say that, and leave the other four items open, as the commit message says.

The logic is sound. Remote identity keys don't touch the filesystem, a manual scan only removes local children, so the remote children found at startup survive, and staging rechecks the host. Capability rows stay Needs verification with honest notes.

**Verdict:** approve. **2 suggestions.**

## v5/feat-513-child-history-gemini-antigravity (#513)

1. **Should-fix:** `tests/native_media_refresh_handler_tests.cpp:331-456`. The three `AntigravityTerminalRefresh*` cases are in `uam_managed_subagent_handler_tests`. That program is excluded from normal builds and from CTest (`CMakeLists.txt:2051`), and no CI workflow builds it. They also sit inside the `UAM_ENABLE_RUNTIME_GEMINI_CLI` guard. So nothing compiles them, including the new `<sqlite3.h>` and `legacy_golden.h` includes. **Fix:**
   - Add a core-inventory case in `antigravity_native_import_tests.cpp` that covers `LoadLocalNativeTranscript` passing on the refusal reason.
   - Run the opt-in program on macOS before merging, and say in the docs that it is opt-in.
   - Guard the cases with the Antigravity flag only.
2. **Should-fix (behaviour change):** `src/cef/chat_handlers.cpp:764`. Switching from CLI View to Chat now runs the Antigravity reader. Any refusal shows "Chat history did not refresh" (status 502), where it used to show saved history silently. That includes "unfinished … retry after the native conversation settles" while a terminal turn is running, plus unsupported media variants. **Fix:** for an unfinished conversation, serve saved history the way the short-snapshot path does, and keep the error for explicit Reload. Add a test with an unfinished-step store.
3. **Nit:** `src/app/runtime_orchestration_services.cpp:5624`. The message shown is "Antigravity conversation <uuid>: Antigravity history references parent context that this reader cannot yet attest completely." It repeats the provider name, shows a raw UUID and uses jargon. For an exact-session refresh, return the bare reason in plainer words.
4. **Nit:** the issue's acceptance for Antigravity (an upstream export or continuity-preserving reader) is still unmet. The rows correctly stay Partial, so keep #513 open.

The CMake fix to the standalone parser test target is correct: `build_features.h` requires the three extra macros. The renamed handler has no stale references. The frontend doesn't filter refresh by provider, so the change is reachable.

**Verdict:** approve after fixes. **4 suggestions.**

## v5/fix-353-remote-workspace-parity (#353)

1. **Should-fix (process):** #353's acceptance criteria and David's 2026-11-16 comment say to split each item into its own issue when work starts. This branch does both items under #353, with a "close remaining gaps" title. **Fix:** open one issue each for remote chat moves and remote folder attachments, link the commit to them, and keep #353 as the roadmap issue until live SSH acceptance.
2. **Should-fix:** `src/cef/acp_handlers.cpp:388`. The new 502 refusal for a non-absolute helper directory has no test. **Fix:** add a fixture-runner case that returns a relative `directory` and assert that no attachment is recorded.
3. **Nit:** `src/app/runtime_orchestration_services.cpp:6879`. Reuse `uam::paths::IsGitWorktreeIsolated(chat)` from `workspace_root.h` instead of repeating the `TrimmedEquals` comparison.
4. **Nit:** `acp_handlers.cpp:388`. Also require the returned directory to match the requested path (after normalisation), so a misbehaving helper can't swap in a different absolute folder.

The helper's directory-path fix is correct for `/`, a trailing `/`, `/.`, `x/..` and Windows root and UNC forms. The Python fixture's reply and error handling matches how `Bridge` works. The test includes its constants header, and the user message is one line.

**Verdict:** approve after fixes. **4 suggestions.**
