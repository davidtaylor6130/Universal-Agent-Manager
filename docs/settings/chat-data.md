# Chat Data settings

Chat Data exports saved conversations as a portable bundle into a new or empty folder and imports bundles without overwriting existing chat identities; collisions receive new IDs. Preserve supported history and organisation while reporting skipped or failed items. Imported bundles are passive read-only transcripts until explicit supported reconnection: saved runtime IDs, queued input, goals and remote cleanup metadata must not start work merely because files were imported. Export is not a live transactional backup of running provider processes or every application setting. Earlier-release data found beside the executable is not a bundle: startup imports it once, as if upgraded in place, keeping chat IDs and provider session links and skipping IDs already stored ([persistence](../architecture/app-state-persistence.md)).

[Imports](../chat/imports.md) · [Source of truth](../architecture/source-of-truth.md) · [Storage Cleanup](storage.md).
