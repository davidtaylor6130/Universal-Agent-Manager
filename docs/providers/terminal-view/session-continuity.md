# Terminal session continuity

A terminal binding identifies the exact native conversation owned by that UAM chat across terminal close, reconnect and application restart. Do not discover ownership by taking an arbitrary recent session from the same workspace, especially when terminals run concurrently. A failed binding save must not launch a replacement conversation under the old identity. Native transcript import and UAM history preservation are separate from proving that subsequent terminal input reaches the same native session.

**Implementation: Partial.** OpenCode has an explicit session-creation/binding route; Codex first-session discovery remains ambiguous for concurrent unbound terminals. Other provider continuity needs provider-specific verification. See the [recorded evidence](../../reference/provider-runtime-parity.md#chats-and-persistence).
