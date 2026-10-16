# Save and resume

UAM saves its normalised conversation and the provider’s native session identity when one exists. Resuming must reconnect the intended conversation without silently creating a different native session or replaying an already delivered prompt. A native limitation requires an explicit UAM continuation path or an honest unsupported result. Persisted chat history alone does not prove native resume, and multiple concurrent terminals must not guess each other’s session IDs.

[Terminal continuity](terminal-view/session-continuity.md) · [Persistence](../architecture/app-state-persistence.md).
