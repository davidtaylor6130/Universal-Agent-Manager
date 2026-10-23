# UAM feature guide

These docs describe what UAM’s features mean and how they are expected to behave. Start with the relevant introduction, then follow the specific feature. Read [How these docs work](documentation-rules.md) before implementing or assessing a feature. **An agreement is not a claim that it is implemented or accepted.** Provider checklists and explicit gap notes keep those distinctions visible.

## System overview

[Architecture](architecture/architecture.md) · [Build flags](build-flags.md) · [Building UAM](building.md) · [Security and managed deployment](security-enterprise.md) · [Recorded decisions](architecture/decisions.md)

## Features

| Area | Start here |
|---|---|
| Providers and runtimes | [Shared requirements and six provider checklists](providers/providers.md) |
| Chat | [Lifecycle, messages, sending, goals, imports and panes](chat/chat.md) |
| Composer | [Input, attachments, voice and workspace actions](chat/composer/composer.md) |
| Sidebar | [Active, Snoozed, Pinned and All Chats](sidebar/sidebar.md) |
| Settings | [Defaults, scope, providers and shared configuration](settings/settings.md) |
| Memory | [Durable knowledge and the memory store](memory/memory.md) |
| Workspaces and VCS | [Workspaces](workspaces/workspaces.md), [Git, SVN and snapshots](settings/version-control/version-control.md) |
| Execution environments | [Local and SSH execution](remote-environments/remote-environments.md) |
| Computer use | [Shared controller and native-provider routing](computer-use.md) |
| Phone access | [Shared main-device connection](phone-access.md), [native iPhone client](phone-native.md) and [browser companion](phone-browser.md) |
| Usage | [Tokens, context, account allowances and history](usage/usage.md) |
| Changes and diffs | [Workspace review, recorded edits and commits](changes/changes.md) |
| File inspection | [Current file content and host identity](files/inspection.md) |
| Updates | [Application, provider and helper updates](updates/updates.md) |
| Navigation | [Main surfaces, keyboard and focus](navigation/navigation.md) |
| UI | [Visual language, primitives, accessibility and interaction](ui/design-language.md) |

## Implementation and maintenance

[Architecture and data authority](architecture/architecture.md) · [Engineering rules](engineering/engineering.md)

- [Documentation review](documentation-review.md): corrected contradictions, confirmed decisions, unresolved scope and known implementation gaps.
- [Engineering reference](reference/README.md): useful technical detail and source navigation, outside the short feature agreements.
- [V5 change log](<../V5 Improvement Change log.md>): work ownership, verification, review and David’s specific acceptance.
- [Repository instructions](../AGENTS.md): required workflow, builds and documentation maintenance.

Before committing behaviour changes, update the owning feature page and applicable provider checklist in the same commit. Follow [the maintenance agreement](documentation-rules.md#changes-and-commits) even when the user did not request documentation. Run `python3 scripts/check_docs.py`; it checks documentation integrity, not feature correctness. David approved retaining this guide and requested its adoption into `uam/v5-improvements` on 2026-10-17; see the [adoption record](<../V5 Improvement Change log.md#documentation-adoption-2026-10-17>).
