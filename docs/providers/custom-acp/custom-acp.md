# Custom ACP agent

The Custom ACP agent lets UAM drive any local agent that speaks the Agent Client Protocol (ACP) over stdio, on a **best effort** basis: there is no guarantee, and only what the agent itself reports is offered. It is an opt-in provider behind the `UAM_ENABLE_RUNTIME_CUSTOM_ACP` [build flag](../../build-flags.md), which is off by default; it is not one of the six release providers. Common behaviour is defined once in the [provider agreements](../providers.md); this checklist only records delivery and known limits. David's decisions for this first slice are [recorded](../../documentation-review.md#custom-acp-agent).

One agent is configured in [provider settings](../../settings/providers.md#custom-acp-agent) with a name, a command, arguments and environment pairs. That form is the only source of its launch: nothing in a repository, chat, phone or automation request can set or change it. UAM starts the agent with an argument array, never through a shell, and refuses Windows batch scripts because starting them runs `cmd.exe`. Settings shows the exact command, and a new or changed command, argument or environment value must be allowed there before the first launch. A command that cannot be started gives one plain launch error in the chat, such as “Custom ACP agent command not found”. Diagnostics record the program and an argument count only, never the arguments or environment values. The agent runs only on this computer.

Capabilities follow strictly what the agent reports in `initialize` and `session/new`: image prompts, session load, models and modes appear only when reported, with no UAM fallback that pretends otherwise. CLI View, native history import, usage figures, text workers and compaction are unavailable and shown as such.

**Assessment:** source routes on the first slice, 2026-11-23. The configuration, launch checks and a real stdio session with a scripted ACP agent fixture (`tests/fixtures/custom_acp_agent.py`: initialize, a new session with models and modes, a streamed prompt, permission allow and reject, cancel, exact argument and environment delivery and an early exit) have a focused native test. The desktop app with the flag on has not been built or launched, no real third-party agent has been tried, and nothing here is David's acceptance.

| Capability | Delivery | Implementation | Limit or route |
|---|---|---|---|
| `structured-chat` | Both | Needs verification | Shared generic ACP session runtime with the agent's own streaming, tool calls and turn end. The stdio exchange with the scripted fixture is tested outside the app; the app route with the flag on is compile-checked only. |
| `terminal-view` | None | Not applicable | A generic ACP agent has no native CLI for UAM to embed; the Chat/Terminal switch is unavailable for these chats. |
| `terminal-sync` | None | Not applicable | No CLI View, so there is nothing to keep in step. |
| `native-import` | None | Not implemented | ACP has no common history format UAM can read; only UAM's own saved chats exist. |
| `chat-lifecycle` | UAM | Needs verification | Shared create, select, rename and delete; chats save through UAM's local history. Not exercised in the app. |
| `concurrency` | Both | Needs verification | One process per chat through the shared runtime; several at once are untested. |
| `branching` | UAM | Needs verification | Shared UAM branching applies; continuation into the agent depends on whether it reports session load. |
| `editing-messages` | UAM | Needs verification | Shared UAM editing; not exercised with this provider. |
| `mcp-connections` | Both | Partial | MCP servers from Settings are offered in `session/new` as ACP defines; the UAM tool connection and central launch setup are not offered. |
| `html-views` | None | Not implemented | Needs the UAM tool connection, which this provider does not get. |
| `secret-entry` | None | Not implemented | Needs the UAM tool connection, which this provider does not get. |
| `skills` | None | Not implemented | Central launch setup, which delivers skills, is skipped for this provider. |
| `attachments` | Both | Needs verification | Files go as ACP resource links; images only when the agent reports image prompts. |
| `compaction` | None | Not implemented | No compaction request is sent; the agent's own context handling is unseen. |
| `computer-use` | Both | Needs verification | The local computer-use server is offered in `session/new` like other MCP servers; untested. |
| `side-chats` | UAM | Needs verification | Shared related conversations; untested with this provider. |
| `models` | Provider | Needs verification | Models and modes appear only when `session/new` reports them; selection uses the shared ACP requests. |
| `agent-selection` | UAM | Needs verification | Launch arguments an agent definition adds for named CLIs are dropped, since this provider's launch is only its Settings configuration; prompt-delivered agent instructions are untested. |
| `agent-history` | None | Not implemented | No provider subagent history format is known. |
| `subagents` | UAM | Not implemented | Managed child chats rely on the UAM tool connection. |
| `goals` | UAM | Needs verification | Shared goal turns are ordinary prompts; untested. |
| `resume` | Both | Partial | UAM keeps the chat; the native session continues only when the agent reports `loadSession`. |
| `reasoning` | None | Not implemented | No reasoning levels are offered for a generic agent. |
| `fast-mode` | None | Not applicable | Fast mode is a named-provider service tier with no ACP equivalent. |
| `permissions` | Both | Needs verification | ACP permission requests reach UAM's permission card and shared permission modes; the allow and reject round trip is tested against the fixture outside the app. |
| `usage` | None | Not implemented | ACP reports no token or cost figures UAM can read. |
| `memory` | UAM | Needs verification | This provider offers no text workers, so extraction needs another provider's worker; recall into prompts is untested. |
| `central-setup` | None | Not implemented | Central launch configuration writes files for named CLIs and is skipped here. |
| `remote-execution` | None | Not implemented | SSH launches are refused with one plain error; a later slice. |
| `stream-watch` | UAM | Needs verification | Shared stream watch sees the same turn events as other ACP providers; untested. |
| `compute-pools` | UAM | Needs verification | Shared prompt gate applies to structured Chat turns; untested. |
| `automation-api` | UAM | Needs verification | The shared automation route has not been checked with this provider. |

**Source:** [configuration and launch checks](../../../src/common/provider/custom_acp/custom_acp_config.cpp), [provider runtime](../../../src/common/provider/custom_acp/custom_acp_provider_runtime.cpp), [Settings bridge](../../../src/cef/custom_acp_handlers.cpp), [focused test](../../../tests/custom_acp_agent_tests.cpp) and [scripted fixture](../../../tests/fixtures/custom_acp_agent.py).
