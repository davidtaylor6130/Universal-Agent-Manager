# App state and persistence

UAM persists chat history and lightweight catalogue metadata separately from transient runtime state. Data-root selection retains the current order: explicit UAM_DATA_DIR, OS app-data, temporary storage, then relative data only as a final fallback. A chat ID, provider-native session ID, workspace and process identity remain distinct. Saves and cleanup must preserve the correct owner across concurrent work and restart, and cache refresh must not resurrect deleted conversations or overwrite newer history.

[Chat](../chat/chat.md) · [Storage](../settings/storage.md).

[Engineering reference](../reference/architecture/app-state-persistence.md).

[Source-of-truth records](source-of-truth.md) distinguish canonical transcripts, durable metadata overlays, delivery journals and derived indexes; not every file named a cache is disposable.

An explicit [data-folder choice](../settings/storage.md#application-data-folder) requires a stopped, exclusively owned transition before changing the authoritative root; a read-only path preflight cannot authorize publication, queue replay or restart. The native bootstrap service captures the original environment override before exporting the effective root, owns the folder preference separately from ordinary settings and preserves pending/uncertain choices across restart. The pending choice alone cannot activate a custom root. A separately acknowledged stopped transaction selects the destination before profile writers load, retains all prior source owners and refuses uncertain routing without fallback. The [handoff contract](../reference/data-folder-transition.md) records the executor, restart, guarded rollback and live-validation gate.

Native media originals are stored separately under the data root and remain bound to the saved UAM chat identity. Message and archival queued-input references preserve their native provenance without becoming execution-host paths or URI-fetch authority. A portable import prepares new originals for its final chat identity and retains them conservatively if primary publication is uncertain; ordinary passive loading still suppresses runnable queued input. Preserve the bounded archival metadata independently of that runnable state through hydration and metadata saves, with the current primary archive retaining authority over stale copies.

An opt-in execution-binding save can retain a witness of its own serialized primary and published file object. Its worker verifies bounded content on that same opened object; completion checks that the canonical primary still names the verified publication. Ordinary save callers retain their existing path, and summary overlays and indexes retain their existing transaction. This receipt proves a specific storage publication rather than native session identity or permission to submit input. The optional cold launch sequence carries repository-minted publication lineage through native binding and final packet publication, while current cold Retry admission remains disabled and native qualification is held.

A stopped-session launch preparation must retain the complete saved conversation and companion ownership evidence through its own publication; a storage receipt does not authorize starting or feeding a provider. Missing, conflicting or uncertain evidence leaves the operation unconfirmed and must preserve foreign replacements.

Only an initial typed transport publication is a stopped launch publication origin; later own continuations retain its ephemeral lineage without becoming new launch origins. An owning save's cancellation handle can revoke that exact worker once and cannot restore ownership or cancel a newer ordinary save. These handles and lineage are not decoded from persisted metadata.
