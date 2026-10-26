# Memory store

The memory store exposes global and workspace-local Markdown resources for browsing, searching, editing and approved scans. Global entries apply across UAM; workspace entries belong to the effective workspace and must not leak into unrelated projects or computers. Worker output must be checked against current policy before saving, and controller-global files must not be copied to SSH hosts merely because project memory is enabled.

**Implementation note:** source currently uses the effective workspace’s `.UAM` directory for local memory; older `.codex/memories` claims are stale. See [memory reference](../reference/architecture/durable-memory.md) and [settings](../settings/memory.md).

Settings → Memory Store is the only memory library surface: David removed the sidebar Memory Store button and, on 2026-10-25, the workspace menu’s Project memory window, so no standalone memory window opens outside Settings.

The library separates global/workspace/all locations, search, new/edit fields, file reveal/root opening and refresh. Manual entry title/category/confidence/body and source/observation metadata retain their scope; delete-one and delete-matching/all actions must preview the selected/search scope and require explicit destructive confirmation. They do not delete the source conversation or unrelated workspace resources. Failed writes retain drafts and leaving prompts Save/Discard/Stay where supported.

Recall with a matching request ranks complete memory facts by whole-word relevance, removes duplicate facts and limits the selected preface to five entries within its complete context budget. Generic continuation or unmatched requests retain the existing bounded fallback; this consolidation does not replace its ordering or truncate multiline facts.
