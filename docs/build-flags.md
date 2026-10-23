# Build flags

Build flags let an organisation deliberately exclude providers and capabilities that affect its policy: each provider, SSH execution, phone access and computer use. Exclusion means the resulting application and its shipped content contain no mention, entry point or usable implementation of the excluded capability: remove its settings, selectors, help text, assets and capability-specific packaged components rather than displaying a disabled option. Saved data must not reactivate it. Enabling it again requires knowingly building a version with the flag enabled. This is the product requirement; hiding UI alone does not satisfy it.

| Capability | CMake flag |
|---|---|
| Gemini | `UAM_ENABLE_RUNTIME_GEMINI_CLI` |
| Codex | `UAM_ENABLE_RUNTIME_CODEX_CLI` |
| Claude Code | `UAM_ENABLE_RUNTIME_CLAUDE_CLI` |
| OpenCode | `UAM_ENABLE_RUNTIME_OPENCODE_CLI` |
| Copilot | `UAM_ENABLE_RUNTIME_COPILOT_CLI` |
| Antigravity | `UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI` |
| SSH | `UAM_ENABLE_SSH` |
| Phone access | `UAM_ENABLE_MOBILE_COMPANION` |
| Computer use | `UAM_ENABLE_COMPUTER_USE` |

**Implementation: Partial.** These flags exist and gate runtime or packaging paths. The browser configurator enumerates all six providers and emits an explicit ON or OFF flag for every provider, including Antigravity. Complete removal of every mention from every restricted artifact has not been verified in this documentation trial. Existing compatibility code must be assessed against the requirement; do not claim policy compliance from the presence of a flag. At least one provider must remain enabled. See [build instructions](building.md), the [configurator](../tools/build-configurator/README.md) and [outstanding decisions](documentation-review.md#build-exclusion).
