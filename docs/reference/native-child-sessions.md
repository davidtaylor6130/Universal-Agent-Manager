# Native child sessions

> Engineering reference, surveyed 2026-11-11. It records how each provider stores a sub-agent's work on disk, which fields link a child to its parent and its spawning tool call, and how [native import](../chat/native-imports.md#sub-chats) uses them. The feature agreement is on that page; this page is evidence and source navigation. Formats were taken from public source and documentation and from UAM's existing readers. No real chat history was read, and every test fixture is synthetic.

## Evidence by provider

**Verified** means the public source or official documentation was read. **Reader** means the shape is the one UAM's existing reader already parses and its tests cover, without a public specification. **Inferred** means neither.

| Provider | Child stored as | Parent link | Spawning tool call link | Evidence |
|---|---|---|---|---|
| Codex | Its own rollout file, `sessions/YYYY/MM/DD/rollout-*.jsonl`, like any thread | `session_meta.payload.parent_thread_id`, and `source.subagent.thread_spawn.parent_thread_id`; over the app-server, `Thread.parentThreadId` | The parent's `spawn_agent` function call output carries `agent_id` (multi-agent v1), or a `CollabAgentToolCall` item lists `receiver_thread_ids` | Verified (`openai/codex` `protocol.rs`, `rollout/src/policy.rs`, `multi_agents/spawn.rs`, app-server `thread_data.rs`) |
| OpenCode | A full session of its own (JSON file in 1.0, SQLite `parent_id` from 1.19, `session_v2.parent_id` in 2.x) | `parentID` in the session's `info`, which `export <id>` prints | The parent's `task` tool part: `state.metadata.sessionId` (1.x); the `subagent` tool: `metadata.sessionID` (2.x) | Verified for 1.x (`session/index.ts`, `cli/cmd/session.ts`, `cli/cmd/export.ts`, `tool/task.ts`); the 2.x export's parent field is inferred |
| Claude Code | `projects/<project>/<sessionId>/subagents/agent-<agentId>.jsonl` | The folder name, plus `sessionId`, `agentId` and `isSidechain: true` on each row | The parent row's `toolUseResult.agentId` on the Task tool result | Path verified (official sub-agent documentation); row fields are Reader |
| Gemini | `chats/<parentSessionId>/<agentId>.jsonl` (or `.json`), first record `kind: "subagent"` | The folder name; there is no parent ID inside the file | The parent's tool call record carries `agentId` | Verified (`google-gemini/gemini-cli` `chatRecordingService.ts`, `chatRecordingTypes.ts`) |
| Copilot | Inline in the parent's `session-state/<sessionId>/events.jsonl`; no separate child file | The event's `agentId` | `subagent.started.data.toolCallId`, and `parentToolCallId` on the child's events | Event schema verified (`github/copilot-sdk` generated session events); inline storage is Reader |
| Antigravity | Separate conversation | Recorded only inside the parent's invoke-subagent step | Step metadata | Inferred. No public documentation; the only public description is a third-party reverse-engineered reader |

Details that affect import:

- **Codex** marks every non-interactive thread as a subagent. Only `thread_spawn` children are conversations a user started work in; `review`, `compact` and `memory_consolidation` threads are not imported. The `collab_agent_spawn_end` event that names the new thread is not persisted in current rollouts, and the multi-agent v2 spawn output carries no thread ID, so the child's own record is the dependable link.
- **OpenCode** `session list` returns root sessions only and prints no parent field, so a child can only be found through its parent's task call.
- **Gemini** always records the root session as the parent, so nested agents sit flat beneath the root. Claude Code journals name the root session in the same way.
- **Copilot** events also carry `parentId`, which is the previous event in the chain, not an agent parent.

## How T3 Code identifies them

T3 Code's public source (`pingdotgg/t3code`) models a provider sub-agent as a thread with `lineage.parentThreadId` and `relationshipToParent: "subagent"`. Such threads are kept out of thread lists and shown inside the parent's view. It builds the link from live provider events rather than from history files: for Codex, the `collabAgentToolCall` item's `receiverThreadIds` or `subAgentActivity.agentThreadId`; for Claude, the SDK stream's `parent_tool_use_id` and `task_started`; for OpenCode, `session.created` with `parentID` plus the tool's `metadata.sessionID`. This was read from the repository (verified). T3 Code does not import existing on-disk history this way, so UAM's import uses the on-disk fields above, which carry the same identities.

## What UAM's import did before

| Provider | Controller-local scan | SSH scan |
|---|---|---|
| Codex | Dropped: a rollout whose metadata named a subagent source was skipped | Dropped: `thread/list` was asked for the default sources and any thread with a parent was skipped |
| OpenCode | Never seen: `session list` returns roots only | Never seen, for the same reason |
| Claude Code | Dropped: files below the project folder were not read, and sidechain rows were skipped | Not scanned (SSH scans cover OpenCode and Codex only) |
| Gemini | Dropped: only the session files directly in `chats/` were read | Not scanned |
| Copilot | Flattened out: the child's events were left out of the parent, and no child chat was made | Not scanned |
| Antigravity | A child with its own conversation imported as an unrelated top-level chat | Not scanned |

In every case a child could still be opened one at a time from its parent's tool call, where the provider recorded its ID.

## How import uses them now

One path serves every provider. Each provider's reader marks a discovered child with the native parent session it names (`ChatSession::native_parent_session_id`, used during discovery only and never saved):

- Codex: `LoadCodexRolloutChat` reads the parent from `session_meta` when asked for spawned children; `AppendRemoteCodexSessions` keeps `parentThreadId` for a thread whose `source` is the `thread_spawn` form, and the SSH scan adds a `thread/list` call for `sourceKinds: ["subAgentThreadSpawn"]`.
- OpenCode, Claude Code, Gemini and Copilot: `AppendNativeChildCandidates` turns the child IDs that each reader already puts on the parent's spawning tool call into unloaded candidates. Nothing is read for a child during a scan.

`StartupNativeImportService` keeps children out of the review list (`m_children`) and adds them to the import behind a selected parent. `ImportDiscoveredProviderChatsBatch` admits a child only after its parent, sets the display link with `ChatRepository::ApplyImportedNativeChildLineage` and publishes it through the same staged save, tombstone, ownership and workspace checks as any history. A child is read during staging: journals through `native_agents::LoadHistory`, the reader an explicit open uses; an OpenCode child through `export`, accepted only when its `parentID` matches; a Codex child from its rollout or `thread/read`. `LinkOwnedChildren` gives a saved, unlinked child its link once its parent is saved.

A parent-owned journal has no native session ID, so nothing can tombstone it by itself; it is only ever imported with a parent that is new to UAM.

## Known limits

- **Parents already in UAM.** Only a scan the user starts imports the missing children of a saved chat (`StartSavedParentBackfill`), per the [recorded decision](../documentation-review.md#native-sub-chat-import). It covers controller-local chats, and skips a parent filed outside the workspace that owns its directory.
- **SSH.** Remote scans read OpenCode and Codex only, so Claude Code, Gemini and Copilot children on SSH hosts are not imported. The helper can read one named child journal but has no call that lists a session's children.
- **Antigravity.** UAM's history reader does not read the invoke-subagent step, so no parent link is available. Linking needs that step decoded from a format with no public specification; nothing is guessed.
- **Claude Code.** A sub-agent whose tool result recorded no `agentId` (an interrupted run, for example) is not found.
- **OpenCode.** A task call that recorded no session ID gives no child. The 2.x export's parent field is unverified; a child whose export omits it is reported and skipped.
- **OpenCode nesting.** Only a root session's task calls are read for children, so a child's own sub-agent sessions are not imported.
- **No start time.** The link stores the parent's creation time. A parent whose listing gives none, such as some OpenCode listings, cannot be linked, and its children are left out without a report.
- **Codex over SSH.** The `sourceKinds` listing is unverified against a live Codex; a Codex that rejects it still imports its root threads.
- **Gemini.** A child with inline images needs the passive media save and is reported and skipped; it opens from the parent's tool call.
- **Different directory.** A child that ran in another directory, such as its own worktree, is not linked and so not imported.
- **Bounds.** At most 64 children per parent are taken from its tool calls, and each child history keeps the per-history size limits.

Checks: `tests/native_subchat_import_tests.cpp` (shared path and one fixture per provider) and `tests/native_child_lineage_tests.cpp` (the explicit-open rule, unchanged).
