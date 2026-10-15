# Memory store

The memory store exposes global and workspace-local Markdown resources for browsing, searching, editing and approved scans. Global entries apply across UAM; workspace entries belong to the effective workspace and must not leak into unrelated projects or computers. Worker output must be checked against current policy before saving, and controller-global files must not be copied to SSH hosts merely because project memory is enabled.

**Implementation note:** source currently uses the effective workspace’s `.UAM` directory for local memory; older `.codex/memories` claims are stale. See [memory reference](../reference/architecture/durable-memory.md) and [settings](../settings/memory.md).
