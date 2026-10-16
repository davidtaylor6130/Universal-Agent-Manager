# Storage Cleanup settings

Storage Cleanup configures independent retention for managed worktrees, backup logs and HTTP cache. Worktree cleanup removes eligible old working directories only after ownership/safety checks while retaining branches and commits; it must preserve active or unresolved work. Backup-log retention keeps the current log and uses zero to retain backups without age expiry. Periodic HTTP cache clearing preserves profiles/sign-in data, starts its interval without an immediate purge and can be disabled. Worktree and log/cache groups retain their own Save/Discard state and failed-save drafts; neither is permission to delete chats, provider histories or user-owned workspace files.

[Source of truth](../architecture/source-of-truth.md) · [Chat Data](chat-data.md) · [Memory store](../memory/store.md) · [Logging](../engineering/logging.md).
