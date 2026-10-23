# Compaction

Compaction reduces the active conversation context while preserving the information needed to continue. UAM shows a distinct compacting animation and elapsed state, followed by a completed or interrupted summary message; verified summary text stays out of ordinary streaming replies and is collapsed when settled. Saving, resuming, branching and switching views must respect the compacted context. Manual compaction is offered only where it can actually run; failed or interrupted compaction must not be shown as completed. An unavailable or stale command must preserve the draft and attached context, and typing it must not fall through to an ordinary prompt.

[UI notifications](../ui/notifications.md) · [Current compaction reference](../reference/architecture/provider-sessions.md#opencode-compaction-synchronization).

A remote maintenance request is saved before delivery and reconnects to its exact owned process with the same delivery identity. Provider acknowledgement is separate from confirmed compaction; a successful command without a native completion boundary must remain an explicit no-op. A replayed maintenance result must remain maintenance after reconnect, without adding an ordinary answer, duplicate summary or goal/checkpoint completion.
