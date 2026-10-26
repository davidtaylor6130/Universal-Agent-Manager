# Skills

A skill is reusable task guidance rooted in SKILL.md together with any scripts, references and assets it needs. UAM maintains a shared library and delivers selected skills through an appropriate native mechanism or readable UAM-managed resources, including on remote execution hosts. Installing a skill makes it available; explicitly attaching one binds it to a particular authored request. A skill is not an agent, a memory or a promise that every provider supports native slash commands.

[Provider skill delivery](../../providers/skills.md) · [Agents](agents.md).

The Skills page manages the shared library and its configured directory; browsing/importing or editing a skill is distinct from attaching it to a chat. Preserve user-owned supporting files and surface invalid definitions, failed saves and explicitly confirmed removal. [Provider delivery](../../providers/skills.md) owns how the selected resource reaches execution.

The app ships only the product skills under `markdown-store/bundled/` and seeds them into the configured library without replacing entries with the same title or command. Top-level `markdown-store/*.uam` files are maintainer-local and are not packaged; CTest `uam_packaged_markdown_store` fails if the built app's store differs from the bundled set.

[Skill-library actions and import collisions](skill-library.md).
