# Switching Chat and Terminal

Changing views should preserve the selected UAM chat and use its bound native session when the provider supports that route. Returning to Chat refreshes available native history without overwriting newer work or duplicating turns. If the runtime cannot synchronize or continue the same session, explain the limitation before implying continuity. An opaque terminal must never be described as a structured approval surface.

Before stopping a structured runtime for CLI View, validate any saved conversation context that would be replayed. If its complete history is unavailable, hold the transfer with an explanation to load that history; retain the running session, saved context and input. Already bound context that will not be reinjected keeps its existing continuity rules.

Starting structured chat while the native CLI is open is held only by work that can finish: a running terminal turn, which the [tool call limit](../runtime-lifecycle.md#open-tool-calls) bounds (or the provider turn timeout when that is longer), or a steering prompt still being delivered or recovered, which its own restart and failure timeouts bound. A steering prompt whose delivery or recovery was reported as failed stays retained for retry and does not hold the switch; it is not restarted or re-reported under the structured chat.

[Session continuity](session-continuity.md).
