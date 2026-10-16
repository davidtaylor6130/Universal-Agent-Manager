# Agent selection

An agent selects the instructions, approved skills and execution constraints for a chat; it is not merely a model label. UAM supplies its own shared agent definitions and maps them to native provider mechanisms when suitable. The chosen agent must follow the request through startup and recovery, and a provider-specific restriction must not be silently weakened when changing provider or environment.

[Agents](../settings/central-setup/agents.md).

Build/Plan/native provider modes and custom UAM agents retain their distinct restrictions. Selecting a next-turn agent while work is active must bind that choice to the queued/authored request rather than mutate an already running turn.
