# App state and persistence

UAM persists chat history and lightweight catalogue metadata separately from transient runtime state. Data-root selection retains the current order: explicit UAM_DATA_DIR, OS app-data, temporary storage, then relative data only as a final fallback. A chat ID, provider-native session ID, workspace and process identity remain distinct. Saves and cleanup must preserve the correct owner across concurrent work and restart, and cache refresh must not resurrect deleted conversations or overwrite newer history.

[Chat](../chat/chat.md) · [Storage](../settings/storage.md).

[Engineering reference](../reference/architecture/app-state-persistence.md).

[Source-of-truth records](source-of-truth.md) distinguish canonical transcripts, durable metadata overlays, delivery journals and derived indexes; not every file named a cache is disposable.
