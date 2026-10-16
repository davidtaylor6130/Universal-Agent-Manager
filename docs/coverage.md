# Documentation coverage

This is the source-linked inventory of settings pages and major user-facing sections at the documentation trial's merged V5 revision. Every listed section has an owning file; subcontrols can share that owner and link to meaningful subtopics. Coverage means an agreement exists, not that every behaviour is implemented, tested or accepted. Build-hidden pages still need documentation for builds that include them. A new section requires a row and owner in the same commit; an intentional omission needs an explicit scope disposition.

## Detailed mapping

- [Every feature/control group](coverage/elements.md): 100 reviewed groups of settings, menus, dialogs, chat actions and browser controls, each linked to its behaviour document and source.
- [Every production rendering source](coverage/components.md): all 89 current TSX files mapped to their primary agreement, including shared primitives and error/loading surfaces.

The section map below remains the shorter starting point. New rendering files require a mapping; changes inside existing files require reviewing their specific element rows too.

## Settings pages

The checker compares these IDs with `SETTINGS_SECTIONS` in the [settings shell](../UI-V2/src/components/settings/SettingsModal.tsx). The `provider:*` family shares configuration rules; each of its six providers also has its own [capability checklist](providers/providers.md#provider-checklists). The retired Typography navigation ID redirects to Appearance, whose typography subpage remains covered.

| Section ID | User-facing section | Owning agreement |
|---|---|---|
| `settings:permissions` | Permissions | [Permissions](settings/permissions.md) |
| `settings:appearance` | Appearance | [Appearance](settings/appearance.md) |
| `settings:defaults` | Chat Defaults | [Chat Defaults](settings/chat-defaults.md) |
| `settings:agents` | Agents | [Agents](settings/central-setup/agents.md) |
| `settings:provider-summary` | Provider Overview | [Provider Overview](settings/provider-overview.md) |
| `settings:provider:*` | Each included provider page | [Each included provider page](settings/providers.md) |
| `settings:remote-hosts` | Remote Hosts | [Remote Hosts](settings/remote-hosts.md) |
| `settings:routing` | SSH Routing | [SSH Routing](remote-environments/routing.md) |
| `settings:phone` | Phone | [Phone](settings/phone-access.md) |
| `settings:voice-input` | Voice Input | [Voice Input](settings/voice-input.md) |
| `settings:memory-settings` | Memory Settings | [Memory Settings](settings/memory.md) |
| `settings:memory-store` | Memory Store | [Memory Store](memory/store.md) |
| `settings:markdown-store` | Skills | [Skills](settings/central-setup/skills.md) |
| `settings:goal-loops` | Goal Loops | [Goal Loops](settings/goal-loops.md) |
| `settings:central-configuration` | Central Setup | [Central Setup](settings/central-setup/central-setup.md) |
| `settings:mcp-servers` | MCP Servers | [MCP Servers](settings/mcp-servers.md) |
| `settings:computer-use` | Computer Use | [Computer Use](settings/computer-use.md) |
| `settings:vcs` | Version Control | [Version Control](settings/version-control/version-control.md) |
| `settings:editors` | Editors | [Editors](settings/editors.md) |
| `settings:shell-actions` | Shell Actions | [Shell Actions](settings/shell-actions.md) |
| `settings:chat-data` | Chat Data | [Chat Data](settings/chat-data.md) |
| `settings:storage-cleanup` | Storage Cleanup | [Storage Cleanup](settings/storage.md) |
| `settings:about` | About | [About](settings/about.md) |

## Major surfaces

This inventory is maintained by comparing navigation and user flows, not by creating a document for each React component. Shared dialogs inherit UI primitives and accessibility rules while the feature owner defines their meaning. Source links identify current entry points, not implementation acceptance.

| Surface ID | User-facing section | Owning agreement | Source |
|---|---|---|---|
| `surface:navigation` | Main navigation and shortcuts | [Main navigation and shortcuts](navigation/navigation.md) | [Entry point](../UI-V2/src/components/layout/AppShell.tsx) |
| `surface:chat` | Chat workspace | [Chat workspace](chat/chat.md) | [Entry point](../UI-V2/src/components/views/ChatView.tsx) |
| `surface:new-chat` | New chat/workspace picker | [New chat/workspace picker](chat/new-chat.md) | [Entry point](../UI-V2/src/components/sidebar/NewChatModal.tsx) |
| `surface:composer` | Composer | [Composer](chat/composer/composer.md) | [Entry point](../UI-V2/src/components/chat/Composer.tsx) |
| `surface:commands` | Slash commands/completions | [Slash commands/completions](chat/composer/commands.md) | [Entry point](../UI-V2/src/components/views/ChatView.tsx) |
| `surface:attachments` | Attachments/image preview | [Attachments/image preview](chat/composer/attachments.md) | [Entry point](../UI-V2/src/components/chat/ImageAttachmentPreview.tsx) |
| `surface:voice` | Voice input | [Voice input](chat/composer/voice-input.md) | [Entry point](../UI-V2/src/components/chat/Composer.tsx) |
| `surface:workspace-actions` | Workspace actions | [Workspace actions](chat/composer/workspace-actions.md) | [Entry point](../UI-V2/src/components/views/ChatView.tsx) |
| `surface:models` | Provider/model picker | [Provider/model picker](providers/models.md) | [Entry point](../UI-V2/src/components/chat/ModelPickerContent.tsx) |
| `surface:agents` | Agent selection | [Agent selection](providers/agent-selection.md) | [Entry point](../UI-V2/src/components/chat/Composer.tsx) |
| `surface:permissions` | Permission choices/decisions | [Permission choices/decisions](providers/permissions/permissions.md) | [Entry point](../UI-V2/src/components/chat/ConversationWork.tsx) |
| `surface:questions` | Provider questions | [Provider questions](chat/questions.md) | [Entry point](../UI-V2/src/components/chat/QuestionInput.tsx) |
| `surface:runtime` | Cancel/Stop/recovery | [Cancel/Stop/recovery](providers/runtime-lifecycle.md) | [Entry point](../UI-V2/src/components/chat/ConversationWork.tsx) |
| `surface:queue` | Queue and Unsent recovery | [Queue and Unsent recovery](chat/sending.md) | [Entry point](../UI-V2/src/components/chat/InlinePromptQueue.tsx) |
| `surface:messages` | Messages/work traces/navigation rail | [Messages/work traces/navigation rail](chat/messages.md) | [Entry point](../UI-V2/src/components/chat/ConversationTurn.tsx) |
| `surface:branching` | Conversation branch picker/editing | [Conversation branch picker/editing](providers/branching.md) | [Entry point](../UI-V2/src/components/chat/ChatBranchPicker.tsx) |
| `surface:side-chats` | Side chats | [Side chats](providers/side-chats.md) | [Entry point](../UI-V2/src/components/views/ChatView.tsx) |
| `surface:goals` | Goals/progress | [Goals/progress](chat/goals.md) | [Entry point](../UI-V2/src/components/shared/GoalBanner.tsx) |
| `surface:architect-worker` | Architect + worker | [Architect + worker](chat/architect-worker.md) | [Entry point](../UI-V2/src/components/chat/Composer.tsx) |
| `surface:panes` | Split chat layout | [Split chat layout](chat/panes.md) | [Entry point](../UI-V2/src/components/layout/MainPanel.tsx) |
| `surface:terminal` | Native CLI View | [Native CLI View](providers/terminal-view/terminal-view.md) | [Entry point](../UI-V2/src/components/views/CLIView.tsx) |
| `surface:sidebar` | Chat selector | [Chat selector](sidebar/sidebar.md) | [Entry point](../UI-V2/src/components/layout/Sidebar.tsx) |
| `surface:active` | Active Chats | [Active Chats](sidebar/active/active.md) | [Entry point](../UI-V2/src/components/sidebar/FolderTree.tsx) |
| `surface:pinned` | Pinned Chats | [Pinned Chats](sidebar/pinned/pinned.md) | [Entry point](../UI-V2/src/components/sidebar/FolderTree.tsx) |
| `surface:snoozed` | Snooze and wake | [Snooze and wake](sidebar/snoozed/snoozed.md) | [Entry point](../UI-V2/src/components/sidebar/SnoozeMenu.tsx) |
| `surface:catalogue` | All Chats/workspace folders | [All Chats/workspace folders](sidebar/all-chats/all-chats.md) | [Entry point](../UI-V2/src/components/sidebar/FolderTree.tsx) |
| `surface:search` | Search/message matches/filters | [Search/message matches/filters](sidebar/search.md) | [Entry point](../UI-V2/src/components/sidebar/ChatSearchBar.tsx) |
| `surface:collections` | Collections and references | [Collections and references](sidebar/collections.md) | [Entry point](../UI-V2/src/components/sidebar/CollectionMenuItems.tsx) |
| `surface:native-import` | Native import chooser | [Native import chooser](chat/native-imports.md) | [Entry point](../UI-V2/src/components/modals/StartupNativeImportChooser.tsx) |
| `surface:routing` | Execution routing decision | [Execution routing decision](remote-environments/routing.md) | [Entry point](../UI-V2/src/components/modals/ExecutionRoutingDialog.tsx) |
| `surface:changes` | Changed-file tree/review | [Changed-file tree/review](changes/changes.md) | [Entry point](../UI-V2/src/components/chat/ChangedFilesTree.tsx) |
| `surface:diff` | Unified/Split diff | [Unified/Split diff](changes/diffs.md) | [Entry point](../UI-V2/src/components/shared/RepositoryDiffDialog.tsx) |
| `surface:recorded-edits` | Recorded chat edits | [Recorded chat edits](changes/recorded-edits.md) | [Entry point](../UI-V2/src/components/layout/RecordedChatEdits.tsx) |
| `surface:commit` | Existing Git/SVN commit panel | [Existing Git/SVN commit panel](changes/commits.md) | [Entry point](../UI-V2/src/components/layout/VcsCommitPanel.tsx) |
| `surface:checkpoints` | Turn rollback/worktree recovery | [Turn rollback/worktree recovery](workspaces/checkpoints.md) | [Entry point](../UI-V2/src/components/views/ChatView.tsx) |
| `surface:inspection` | File inspection | [File inspection](files/inspection.md) | [Entry point](../UI-V2/src/components/shared/FilePreviewDialog.tsx) |
| `surface:usage` | Usage/history/filters/breakdowns | [Usage/history/filters/breakdowns](usage/history.md) | [Entry point](../UI-V2/src/components/usage/UsagePage.tsx) |
| `surface:prices` | Model-price editor | [Model-price editor](usage/model-prices.md) | [Entry point](../UI-V2/src/components/usage/UsagePage.tsx) |
| `surface:limits` | Account allowances | [Account allowances](usage/allowances.md) | [Entry point](../UI-V2/src/components/usage/UsageLimits.tsx) |
| `surface:usage-indicator` | Chat context/usage figures | [Chat context/usage figures](providers/usage.md) | [Entry point](../UI-V2/src/components/chat/ProviderAllowanceIndicator.tsx) |
| `surface:notifications` | Notifications and transient alerts | [Notifications and transient alerts](ui/notifications.md) | [Entry point](../UI-V2/src/components/layout/AppShell.tsx) |
| `surface:updates` | Updates panel | [Updates panel](updates/updates.md) | [Entry point](../UI-V2/src/components/layout/UpdatesPanel.tsx) |
| `surface:memory-library` | Memory library | [Memory library](memory/store.md) | [Entry point](../UI-V2/src/components/settings/MemoryLibraryModal.tsx) |
| `surface:memory-scan` | Memory scan chooser | [Memory scan chooser](memory/scanning.md) | [Entry point](../UI-V2/src/components/settings/MemoryScanModal.tsx) |
| `surface:skill-library` | Skill library | [Skill library](settings/central-setup/skills.md) | [Entry point](../UI-V2/src/components/settings/MarkdownStoreModal.tsx) |
| `surface:computer-use` | Computer-use target grant | [Computer-use target grant](computer-use.md) | [Entry point](../UI-V2/src/components/chat/ComputerUseModal.tsx) |
| `surface:phone` | Phone browser interface | [Phone browser interface](phone-access.md) | [Entry point](../UI-V2/src/components/companion/CompanionShell.tsx) |

## Engineering coverage

[Architecture](architecture/architecture.md) owns system relationships, code structure, data authority and OS/provider/VCS boundaries. [Engineering](engineering/engineering.md) owns testing, logging and the change workflow. [Building](building.md), [UI rules](ui/design-language.md), [documentation rules](documentation-rules.md) and [AGENTS.md](../AGENTS.md) own their respective gates. Detailed source notes stay in [reference](reference/README.md).

## Scope and limits

The existing VCS panel conflicts with older release exclusions and is [explicitly recorded](architecture/decisions.md#existing-vcs-panel), not silently omitted. RAG engines, templates, Dear ImGui, local model engines and generated frontend output are not new documented product surfaces in this release slice. The checker detects unmapped settings IDs, unmapped production TSX sources, duplicate coverage/element rows and broken links; the major-surface inventory still requires human/agent comparison when navigation changes. Runtime implementation and live/platform acceptance remain separate.

## Six provider pages

Shared [provider configuration](settings/providers.md) applies to all six pages; each provider also has its own explicit feature checklist: [Gemini](providers/gemini/gemini.md), [Codex](providers/codex/codex.md), [Claude Code](providers/claude-code/claude-code.md), [OpenCode](providers/opencode/opencode.md), [Copilot](providers/copilot/copilot.md) and [Antigravity](providers/antigravity/antigravity.md). Conditional builds do not erase the owning agreements.
