# Source of truth

UAM owns its saved product state in the resolved data root; a provider's native history is an external session source, not authority to overwrite UAM organisation, authored intent or recovery records. Native services reconcile durable records into runtime models and React projections. Authority is field-specific: never choose whichever file, timestamp, cache or screen looks newest without the repository's revision and ownership rules. Keep [existing storage routing](app-state-persistence.md) and user data in place.

| Record | Authority and boundary |
|---|---|
| `chats/<chat-id>.json` | Canonical saved UAM conversation and chat metadata, including native session links and persisted control state; load through ChatRepository and supported migrations. A UAM ID, provider session ID and process ID are distinct. |
| `chat-summaries/<chat-id>.json` | Lightweight transcript-derived fields **and durable metadata overlays**, including attention, snooze and recency values that may be newer than the primary. Preserve valid overlays when hydrating or regenerating; do not bulk-delete these as disposable caches. |
| `queue-outboxes/<chat-id>.json` | Revision-bound queued-input journals reconciled with the primary full-save revision. Retain authored input and delivery/recovery ownership; an arbitrary newest-file merge or missing process does not prove input was never delivered. |
| Chat message-page indexes | Derived navigation into a specific primary transcript. Stale or invalid indexes can be regenerated through repository validation; they do not replace message content. |
| `.bak` companions | Validated recovery candidates, not competing live primaries. Use supported backup validation and publication rather than manually choosing by modification time. |
| `settings.txt`, `folders.txt`, `resource_collections.json` and their owning stores | Persisted configuration, environment overrides and organisation. Use the relevant serializers/services, including collection/reference storage, rather than reconstructing them from visible sidebar rows. |
| Agent/skill/instruction and memory files | User-managed resources with their own global/workspace scope. Referencing or indexing a file does not authorize overwriting it; memory and instructions are not recovered from arbitrary chat prose. |
| Agent-run ledgers, deletion intents and workspace/remote recovery receipts | Durable execution, cleanup and ownership evidence. They are not caches; preserve unresolved work and tombstones until the owning service settles them. |
| Usage ledgers, imported receipts and account snapshots | History facts and identity witnesses are distinct from derived aggregates; quota snapshots record an account observation, not the present balance. Consult the usage service before removing retained state. |
| React/Zustand state and browser preferences | Native-backed values are projections/drafts until native acknowledgement. Some appearance and usage-price preferences are deliberately local to the frontend; they are not substitutes for native settings or chat files. |
| Provider transcripts, logs and terminal output | External conversation evidence or diagnostics. Import/refresh must preserve native identity, tombstones, UAM-only metadata and newer owned work. Logs and terminal text do not authorize replay, deletion or execution. |

On disagreement, stop the affected mutation, retain evidence and use the owning service's guarded reconciliation. A save failure must retain the draft or restore the previous authoritative state; success is reported only after the relevant commit acknowledgement. A passive export or test-profile copy must not acquire live queue, agent-run or remote-control authority.

[Repository](../../src/common/chat/chat_repository.h) · [Paths](../../src/common/paths/app_paths.h) · [Persistence reference](../reference/architecture/app-state-persistence.md) · [Sending](../chat/sending.md) · [Chat bundles](../settings/chat-data.md).
