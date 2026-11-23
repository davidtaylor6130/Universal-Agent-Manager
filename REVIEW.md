# V5.2 security review

- **Scope:** `git diff origin/main...origin/uam/v5.2-sync-main`, which is everything V5.2 adds on top of the released V5.1.0. That is 76 files (+4,329 / −384), 66 of them outside `docs/` and Markdown.
- **Reviewed commit:** `6f235eb98fe8724f7e4c2c7eea1a7065ea47c502` (`origin/uam/v5.2-sync-main`).
- **Method:** a read-only manual review of the diff and the code paths it touches. The reviewer edited, built and ran nothing, and posted no comments. File and line references point to the reviewed commit.

## Summary

| Severity | Count |
|---|---|
| Critical | 0 |
| High | 0 |
| Medium | 0 |
| Low | 0 |

The review found **no real issues with evidence behind them** in V5.2. Because there are no findings, there are no draft issues.

V5.2 is mostly performance work:
- the window now opens before the saved chat list loads, and bridge requests wait ("park") until it does;
- control-tool audit records and remote-output acknowledgements now save in the background;
- model catalogues are sent once and shared between chats by reference;
- chat files and UI asset files are read by several threads at once;
- memory pushes are throttled;
- there is a new performance suite and a CI job that only reports.

None of the diff changes the agent permission ceilings, approval routing, companion pairing or token authentication, origin checks, the SSH runner protocol, how provider, git or svn command arguments are built, or how secrets are stored.

## Ranked findings

None.

## Focus areas and what was checked

| # | Focus area | Result | Evidence (file:line) | Why it is clean |
|---|---|---|---|---|
| 1 | Agent permission ceilings and approval routing (must never fail open) | Clean | `src/app/uam_control_service.cpp:629`, `:647`, `:1967`, `:3158` | No permission or approval code changed. Only `agent_status`, `agent_list`, `goal_get`, `skill_list` and `skill_read` are treated as read-only, and that only skips the whole-chat rollback copy. They still pass signature verification (`VerifyRequest`), the 30-per-minute rate limit (`:124`, `:1967`) and the checks inside `ExecuteTool` first. The approval-gated methods (`goal_create`, `chat_create`) are unchanged. |
| 2 | Agent control tools with deferred audit saves | Clean | `src/app/uam_control_service.cpp:498`, `:640`, `:647-674`, `:691-722`; `src/common/runtime/acp/acp_session_runtime.cpp:9811`, `:9897` | Duplicate request IDs are still rejected, including against replies waiting to be written (`:498`). The 64 KiB reply cap (`:120`) is applied before a reply is stored. A reply is written only once a full save has made its audit record durable. A lost save gets an error reply and its audit record is removed. Replies are written only while their capability still exists (the ownership check at `:640`). Growth is bounded: at most 30 requests a minute, and the 64-record audit cap (`:127`) closes out older receipts as "not recorded" instead of retrying them forever (`acp_session_runtime.cpp:9819-9827`). |
| 3 | Companion and phone APIs, token-scoped APIs | Clean | `src/cef/uam_query_handler.cpp:214`; `src/cef/companion_server.cpp:925`; `src/common/companion/protocol.h:132` | Phone requests get 503 while chats are loading, so they cannot act on chats that are not loaded yet. Pairing, token checks and origin checks are unchanged. `MobileState` puts the shared model catalogues back inline and drops the lookup table. It does not add any fields to what phones already received. |
| 4 | Localhost exposure | Clean (one older behaviour noted below) | `scripts/perf_driven.py:7-10`, `:334` | V5.2 opens no new listening sockets in the app. The performance driver passes Chromium's DevTools switch with a loopback port, but only on a throwaway data root, and normal launches never pass it. See "Older behaviour noted". |
| 5 | CEF renderer bridge: what the renderer can send, and validation | Clean | `src/cef/uam_query_handler.cpp:583`, `:616`, `:765-787`; `src/app/application.cpp:449` | The trusted-main-frame check (`:583`) and `ParseBridgeRequest` run before a request is parked (`:616`). Resumed requests go through `OnQuery` again, so they are fully re-validated (`:773`). Cancelled requests are removed from the queue (`:783`). Only the trusted bundled UI can fill the queue, and it is emptied once the chat list loads (`application.cpp:449`). |
| 6 | Actions that skip the parking gate | Clean | `src/cef/uam_query_handler.cpp:597-614`; `src/cef/settings_handlers.cpp:1515`; `src/app/application.cpp:1174` | Window, data-folder, restart and quit actions are handled before the gate. Data-folder rollback is still disabled (`kTransfersValidated`, `:1515`). Choosing a folder only records the choice. Restart and quit wait for the chat-list loader thread before shutting down (`application.cpp:1174`). |
| 7 | Remote SSH runner protocol | Clean | `src/common/runtime/acp/acp_polling.cpp:829`, `:859`; `src/common/runtime/acp/acp_session_lifecycle.cpp:4865`, `:5019-5029`; `src/common/runtime/acp/acp_session_runtime.cpp:9856`, `:9874` | The message format is unchanged. The helper is acknowledged only after a full save has recorded exactly the acknowledged output positions. A save that loses or moves those positions is marked lost and decided again. Once a remote process reports that it exited, the chat stays on the synchronous save path with rollback (`:5029`). Pending permission requests are not dropped before a save has made them durable. |
| 8 | Path traversal and symlinks in workspace and data-folder reads | Clean | `src/cef/ui_asset_snapshot.h:21-23`, `:38`, `:66`, `:70`; `src/common/chat/chat_repository.cpp:5815`, `:5860` | The UI snapshot rejects symlinks and non-regular files when it lists the folder, and checks each path again just before reading it (`:66`). Chat files are read only from regular `*.json` files listed directly in the chats folder, with the same backup and malformed-file handling as before. No new workspace-relative path is accepted from the renderer, a phone or an agent. |
| 9 | Command and argument injection into provider CLIs, git or svn | Clean | (no changed lines) | The diff contains no process launches or command-line building in `src`. The changes to OpenCode history import and model discovery only skip work for turned-off providers (`src/app/startup_native_import_service.cpp:268`, `:387`; `src/app/model_discovery_service.cpp:77`). |
| 10 | Secret handling and logging | Clean | `src/app/application.cpp` (startup `[performance]` lines), `src/cef/uam_cef_app.cpp` (asset-snapshot timing); `scripts/perf_suite.py:233` | The new diagnostic lines log only times, counts and existing file-path warnings, never a token or secret. The legacy OpenCode secret clean-up during parallel reads takes the per-chat save lock. The performance suite runs the app with an allowlisted environment: no tokens, API keys or real provider config paths, and a throwaway home folder. |
| 11 | Resource use: unbounded reads or allocations driven by file or network input | Clean | `src/cef/ui_asset_snapshot.h:21-23`, `:70`; `src/common/chat/chat_repository.cpp:5860`; `src/cef/cef_push.cpp:89`, `:532`; `UI-V2/src/store/cpp/sanitizers.ts:964`, `:985`, `:2185`; `src/app/agent_run_scheduler.cpp:472`; `src/app/chat_lifecycle_service.cpp:1365`, `:1397` | **UI snapshot:** at most 16 MiB per file, 64 MiB in total and 4,096 files, checked before any memory is allocated. Each read takes exactly the listed size and fails if the file changed. **Chat reads:** at most 8 threads. **Model catalogues:** native code tracks at most 256 sent IDs and then resets. The page stores catalogues in a `Map`, so special object keys are not a risk, sanitizes each entry and caps config options at 64. A reference to a catalogue the page never received makes it reject the update and pull full state instead. **Other caches:** the cached parent transcript is shared per file version and expires after the retry interval. The workspace-owner cache is cleared whenever the folder list changes. |
| 12 | Model-catalogue IDs used as references | Clean | `src/cef/state_serializer.cpp` (`AddModelCatalog`, `ReferenceModelCatalog`); `src/cef/cef_push.cpp:532` | IDs are content hashes that native code computes from data it already trusts, and they only travel from native code to the trusted page. In the worst case two catalogues share an ID and a chat shows the wrong model list. That affects the display only, and no outside party can choose the IDs. |
| 13 | CI and supply chain | Clean | `.github/workflows/ci.yml:10`, `:364` | The new `performance` job runs on `pull_request` under the workflow-wide `contents: read` permission and uses no secrets. It downloads only artifacts from the same run, and every measuring step is `continue-on-error`, so it cannot block a merge. There is no `pull_request_target`. |
| 14 | Startup loading and app-thread work | Clean | `src/app/application.cpp:446-449`, `:1174` | The whole poll tick is skipped until the chat list has loaded, so agent control requests and runtimes cannot act on a partial chat list. |

## Older behaviour noted (not introduced by V5.2)

- **Release builds accept Chromium's DevTools switch.** The new `scripts/perf_driven.py:7-10` says the app passes its command line straight to CEF, so `--remote-debugging-port` works on any build. Chromium binds that port to loopback. Anyone who can launch the app with the switch can control the privileged UI page through DevTools. That already needs the ability to run code as the same user, and V5.2 did not change it, so it is not counted as a finding.
- **Possible hardening:** strip remote-debugging switches in `OnBeforeCommandLineProcessing` in non-test builds, or allow them only behind a build flag that the performance suite turns on.

## What is missing from this review

- **No dynamic testing.** The reviewer did not build or run the native test suites, UI tests, the performance suite or the app, so the conclusions rest on reading the code.
- **Thread-safety of parallel chat parsing was not fully audited.** `ParseLocalChatFile` and `ApplySummaryOpenedAtToFullChat` now run on up to 8 threads (`src/common/chat/chat_repository.cpp:5860`). The reviewer did not check every helper they call for mutable shared state. A data race there would show up as a reliability problem rather than a security bypass, but it was not ruled out.
- **Durability logic was read for fail-open behaviour only.** The reviewer checked that the background-save and acknowledgement-ticket state machines (`acp_session_runtime.cpp:9811-9925`, `acp_polling.cpp:820-880`) never acknowledge or reply before a durable save. Every crash-timing interleaving was not traced.
- **Tests and harnesses were not reviewed as product code.** Files under `tests/` and the bodies of `scripts/perf_suite.py` and `scripts/perf_driven.py` were read only for secret handling and network exposure.
- **The iOS app (`Mobile/`) was not reviewed.** V5.2 does not change it.

## Draft issue previews

None, because there are no findings.
