# Provider boundaries

Provider-specific launch arguments, wire formats, native session identities, model capabilities, permission mappings and history parsing belong in provider adapters behind the runtime contract. Shared UAM services own product behaviour such as chat organisation, authored input, branching and fallback decisions; they consume normalized events and advertised capabilities rather than testing provider names throughout the UI. Add explicit adapter support when protocols differ, preserve provider-specific restrictions and use UAM equivalents where the shared agreement requires them. A native limitation must not silently redefine the common feature.

**Current structure:** [runtime interfaces and registry](../../src/common/provider/provider_runtime.h) connect the six adapters under `src/common/provider/` with common ACP and terminal machinery. Native app-server and stream JSON remain valid routes. Existing provider branches are not proof of complete modular isolation; new work should respect the boundary and document any required exception. Update the [shared requirements and provider checklist](../providers/providers.md) in the same change.

[Code structure](code-structure.md) · [Provider sessions](provider-sessions.md) · [Testing](../engineering/testing.md).
