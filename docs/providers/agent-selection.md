# Agent selection

An agent selects the instructions, approved skills and execution constraints for a chat; it is not merely a model label. UAM supplies its own shared agent definitions and maps them to native provider mechanisms when suitable. The chosen agent must follow the request through startup and recovery, and a provider-specific restriction must not be silently weakened when changing provider or environment.

[Agents](../settings/central-setup/agents.md).
