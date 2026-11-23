# Agent selection

An agent selects the instructions, approved skills and execution constraints for a chat; it is not merely a model label. UAM supplies its own shared agent definitions and maps them to native provider mechanisms when suitable. The chosen agent must follow the request through startup and recovery, and a provider-specific restriction must not be silently weakened when changing provider or environment.

[Agents](../settings/central-setup/agents.md).

Build/Plan/native provider modes and custom UAM agents retain their distinct restrictions. Choosing provider-native features for a provider keeps a Plan or read-only agent's read ceiling. Selecting a next-turn agent while work is active must bind that choice to the queued/authored request rather than mutate an already running turn.

CLI View carries a custom agent's captured instructions into the native CLI at startup, through Central Setup when it applies and otherwise through a provider-native instruction route. A provider that cannot take them refuses CLI View for that agent with a one-line message; it never starts the agent without its role.

A custom agent's skill allowlist governs UAM's skills. Where a provider has its own skill tool and a launch option to limit it, UAM also restricts that tool for custom agents: OpenCode's native agent configuration denies skills outside the allowlist, and Claude Code's skill tool is removed because Claude cannot narrow it to a list. Elsewhere the allowlist is enforced by UAM only, and the provider checklists say so. Build and Plan keep provider-native skills.

Built-in Build and Plan support shared UAM delegation under the [agent definition agreement](../settings/central-setup/agents.md#built-in-delegation); native execution and environment restrictions still apply.
