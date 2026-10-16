# UAM project guide for Claude

Read [AGENTS.md](AGENTS.md) first: it owns release scope, work ownership, the V5 change log, build commands, acceptance and documentation maintenance. These instructions supplement it and do not redefine those agreements.

Start feature work at [docs/README.md](docs/README.md), then read [How these docs work](docs/documentation-rules.md), the relevant parent introduction and owning feature page. The guide describes expected behaviour; explicit provider checklist statuses and [open decisions](docs/documentation-review.md) distinguish gaps from acceptance. Update affected documentation and capability rows in the same implementation commit, even when the user did not request docs, and run `python3 scripts/check_docs.py`.

[Architecture](docs/architecture.md) explains system connections. [The source map](docs/reference/source-map.md) and [engineering references](docs/reference/README.md) help find code and existing checks. [The previous code map](docs/reference/agent-code-map.md) retains detailed bridge/action notes without acting as a second current product specification.

React changes follow [the UI language](docs/ui/design-language.md), including primitives, accessibility, motion and interaction. Settings also follow [the settings layout agreement](docs/ui/settings.md). Required Claude review and David’s specific approval remain separate from builds, tests and installation, as specified in AGENTS.md.

Keep Gemini, Codex, Claude Code, OpenCode, Copilot and Antigravity provider implementations, with structured Chat and native CLI View in scope for all six. David confirmed Antigravity’s full inclusion and retention of the current data-storage routing on 2026-10-16; see the [recorded decisions](docs/documentation-review.md). Scope inclusion does not establish that every capability is implemented or accepted.
