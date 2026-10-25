# Build flags

Build flags let an organisation deliberately exclude providers and capabilities that affect its policy: each provider, SSH execution, phone access and computer use. Exclusion means the resulting application and its shipped content contain no mention, entry point or usable implementation of the excluded capability: remove its settings, selectors, help text, assets and capability-specific packaged components rather than displaying a disabled option. Saved data must not reactivate it. Enabling it again requires knowingly building a version with the flag enabled. This is the product requirement; hiding UI alone does not satisfy it.

| Capability | CMake flag | Interface bundle | Native application and runner |
|---|---|---|---|
| Gemini | `UAM_ENABLE_RUNTIME_GEMINI_CLI` | Implemented | Partial |
| Codex | `UAM_ENABLE_RUNTIME_CODEX_CLI` | Implemented | Partial |
| Claude Code | `UAM_ENABLE_RUNTIME_CLAUDE_CLI` | Implemented | Partial |
| OpenCode | `UAM_ENABLE_RUNTIME_OPENCODE_CLI` | Implemented | Partial |
| Copilot | `UAM_ENABLE_RUNTIME_COPILOT_CLI` | Implemented | Partial |
| Antigravity | `UAM_ENABLE_RUNTIME_ANTIGRAVITY_CLI` | Implemented | Partial |
| SSH | `UAM_ENABLE_SSH` | Partial | Partial |
| Phone access | `UAM_ENABLE_MOBILE_COMPANION` | Implemented | Partial |
| Computer use | `UAM_ENABLE_COMPUTER_USE` | Partial | Partial |

Any CMake spelling (`OFF`, `0`, `FALSE`) means the same exclusion everywhere: CMake normalises every flag once and passes the same 0/1 value to native code, the remote runner, tests and the interface build. At least one provider must remain enabled; the first enabled provider in the order above is the default. `UAM_RUNNER_ONLY` builds only the remote runner with the same provider exclusions, and `UAM_PACKAGE_REMOTE_RUNNERS=OFF` packages only the current platform's helper without leaving earlier cross-platform helpers behind.

**Implementation: Partial.** The interface bundle drops an excluded provider's metadata, name, logo, settings page, model labels, usage colours and provider-specific branches, and an excluded feature's settings pages, controls, labels, manifest and service worker; an automated scan of every single-flag and single-provider bundle finds no remaining name outside reviewed exceptions. SSH and computer-use builds still carry their internal state-field names, with no label, control or request. Native builds compile only included providers' sources, register only included features' bridge actions, list only included providers' history, import and allowance sources, and never read an excluded provider's configuration folders. Shared runtime, settings and state code still contains names of excluded providers and features, so native artifacts do not yet meet the zero-mention requirement. Native artifact scans remain unverified until the leak gate runs on native builds. See [building](building.md), the [configurator](../tools/build-configurator/README.md), the [build engineering reference](reference/architecture/build-test-release.md#build-flags) and [outstanding decisions](documentation-review.md#build-exclusion).
