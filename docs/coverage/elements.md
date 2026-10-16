# Feature and user-facing element map

This maps the controls/actions reviewed in source to the document that defines their meaning. Rows group related instances of one concept: each dynamically named chat, provider, model, host or memory entry inherits that row rather than requiring a separate document. Save/Discard/Cancel/Close, errors, loading, keyboard behaviour and confirmations also follow [settings](../ui/settings.md), [primitives](../ui/primitives.md) and [accessibility](../ui/accessibility.md). This is source-level documentation coverage, not a live GUI or implementation acceptance report.

## Navigation and shared UI

| Element ID | Controls and actions | Owning document | Source |
|---|---|---|---|
| `element:navigation-and-shared-ui:navigation` | Chats/Usage/Settings; expand/collapse selector; resize panels; focus/keyboard shortcuts | [Agreement](../navigation/navigation.md) | [Entry point](../../UI-V2/src/components/layout/AppShell.tsx) |
| `element:navigation-and-shared-ui:settings-search` | Search settings; switch sections; Back to chats; Save/Discard/Stay and failed saves | [Agreement](../ui/settings.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |
| `element:navigation-and-shared-ui:primitives` | Buttons, icon actions, menus, switches, cards, tooltips, number fields, overlays and confirmations | [Agreement](../ui/primitives.md) | [Entry point](../../UI-V2/src/components/ui/index.ts) |
| `element:navigation-and-shared-ui:accessibility` | Keyboard reachability, modal focus, accessible names, disabled/loading/error and reduced motion | [Agreement](../ui/accessibility.md) | [Entry point](../../UI-V2/src/components/ui/Overlay.tsx) |
| `element:navigation-and-shared-ui:recovery` | Failed panel/interface, Dismiss, Reload and unsaved-draft warning | [Agreement](../ui/interface-recovery.md) | [Entry point](../../UI-V2/src/components/shared/InterfaceErrorBoundary.tsx) |
| `element:navigation-and-shared-ui:notifications` | Unread/count, mark all read, dismiss individual notices, failure details and shared transient alert | [Agreement](../ui/notifications.md) | [Entry point](../../UI-V2/src/components/layout/AppShell.tsx) |
| `element:navigation-and-shared-ui:updates` | Check/update all; individual application/provider actions; release notes; stop-session confirmation; retry/dismiss | [Agreement](../updates/updates.md) | [Entry point](../../UI-V2/src/components/layout/UpdatesPanel.tsx) |

## Sidebar and workspace selection

| Element ID | Controls and actions | Owning document | Source |
|---|---|---|---|
| `element:sidebar-and-workspace-selection:create` | New chat; workspace picker, host/provider/model choice and existing-worktree reuse | [Agreement](../chat/new-chat.md) | [Entry point](../../UI-V2/src/components/sidebar/NewChatModal.tsx) |
| `element:sidebar-and-workspace-selection:workspace-browse` | Local/remote directory browsing, selected host and error/empty states | [Agreement](../workspaces/workspaces.md) | [Entry point](../../UI-V2/src/components/sidebar/RemoteDirectoryBrowser.tsx) |
| `element:sidebar-and-workspace-selection:active` | Active collapse/status counts, attention, Done, Clear all and next attention | [Agreement](../sidebar/active/active.md) | [Entry point](../../UI-V2/src/components/sidebar/FolderTree.tsx) |
| `element:sidebar-and-workspace-selection:pin` | Pin/unpin and persistent Pinned Chats | [Agreement](../sidebar/pinned/pinned.md) | [Entry point](../../UI-V2/src/components/sidebar/SessionItem.tsx) |
| `element:sidebar-and-workspace-selection:snooze` | Snooze presets/custom time, wake label, explicit Unsnooze and timed wake | [Agreement](../sidebar/snoozed/snoozed.md) | [Entry point](../../UI-V2/src/components/sidebar/SnoozeMenu.tsx) |
| `element:sidebar-and-workspace-selection:search` | Catalogue text, Search contents, provider/state filters, clear and message hits | [Agreement](../sidebar/search.md) | [Entry point](../../UI-V2/src/components/sidebar/ChatSearchBar.tsx) |
| `element:sidebar-and-workspace-selection:row-actions` | Rename/F2, regenerate/cancel title, copy chat details, Delete and disclosed branch-family scope | [Agreement](../sidebar/management.md) | [Entry point](../../UI-V2/src/components/sidebar/SessionItem.tsx) |
| `element:sidebar-and-workspace-selection:bulk` | Shift/multi-select, drag/drop/reorder, selected-chat/workspace deletion and progress | [Agreement](../sidebar/management.md) | [Entry point](../../UI-V2/src/components/sidebar/FolderTree.tsx) |
| `element:sidebar-and-workspace-selection:collection` | Create/rename/reorder/delete collections; move/remove chat/workspace/resource references | [Agreement](../sidebar/collections.md) | [Entry point](../../UI-V2/src/components/sidebar/CollectionMenuItems.tsx) |
| `element:sidebar-and-workspace-selection:icons` | Chat/workspace/host/collection icon; Lucide search, emoji, PNG and reset | [Agreement](../sidebar/custom-icons.md) | [Entry point](../../UI-V2/src/components/shared/CustomIcon.tsx) |
| `element:sidebar-and-workspace-selection:workspace-actions` | Rename folder; host/directory; Project memory; Review native chats; Delete folder with chats | [Agreement](../sidebar/management.md) | [Entry point](../../UI-V2/src/components/sidebar/FolderTree.tsx) |
| `element:sidebar-and-workspace-selection:folder-recovery` | Rebuild workspace folders; Ready/Needs attention; explicit unavailable-chat deletion | [Agreement](../workspaces/folder-recovery.md) | [Entry point](../../UI-V2/src/components/sidebar/FolderTree.tsx) |
| `element:sidebar-and-workspace-selection:native-import` | Startup/review import selection, retry/skip and eligible nonempty native history | [Agreement](../chat/native-imports.md) | [Entry point](../../UI-V2/src/components/modals/StartupNativeImportChooser.tsx) |

## Chat, composer and runtime

| Element ID | Controls and actions | Owning document | Source |
|---|---|---|---|
| `element:chat-composer-and-runtime:panes` | Split/select/resize panes; active chat and Chat/CLI view per pane | [Agreement](../chat/panes.md) | [Entry point](../../UI-V2/src/components/layout/MainPanel.tsx) |
| `element:chat-composer-and-runtime:draft` | Compose/send; retained per-chat draft, failure preservation and direct versus queued input | [Agreement](../chat/sending.md) | [Entry point](../../UI-V2/src/components/chat/Composer.tsx) |
| `element:chat-composer-and-runtime:queue` | Queue reorder/edit/remove/clear, held/resume and Unsent retry/delete; no automatic uncertain replay | [Agreement](../chat/sending.md) | [Entry point](../../UI-V2/src/components/chat/InlinePromptQueue.tsx) |
| `element:chat-composer-and-runtime:models` | Provider/model search/groups, catalogue refresh, cached availability and selection | [Agreement](../providers/models.md) | [Entry point](../../UI-V2/src/components/chat/ModelPickerContent.tsx) |
| `element:chat-composer-and-runtime:agent` | Build/Plan/custom UAM agent; next-turn agent; native provider mode | [Agreement](../providers/agent-selection.md) | [Entry point](../../UI-V2/src/components/chat/Composer.tsx) |
| `element:chat-composer-and-runtime:effort` | Reasoning effort and provider config/variant choices | [Agreement](../providers/reasoning.md) | [Entry point](../../UI-V2/src/components/chat/Composer.tsx) |
| `element:chat-composer-and-runtime:speed` | Supported speed/service tier and saved selection | [Agreement](../providers/fast-mode.md) | [Entry point](../../UI-V2/src/components/chat/Composer.tsx) |
| `element:chat-composer-and-runtime:approval` | Default/Accept edits/AI Review/YOLO, dangerous-choice confirmation and request decisions | [Agreement](../providers/permissions/permissions.md) | [Entry point](../../UI-V2/src/components/chat/ConversationWork.tsx) |
| `element:chat-composer-and-runtime:questions` | Choice/free-text question answers, submission, stale/cancelled requests | [Agreement](../chat/questions.md) | [Entry point](../../UI-V2/src/components/chat/QuestionInput.tsx) |
| `element:chat-composer-and-runtime:attachments` | Add/remove files/folders/images/skills; context chips; preview and edited-message context | [Agreement](../chat/composer/attachments.md) | [Entry point](../../UI-V2/src/components/chat/ImageAttachmentPreview.tsx) |
| `element:chat-composer-and-runtime:voice` | Start recording/transcription, cancel, editable text, unavailable mode and errors | [Agreement](../chat/composer/voice-input.md) | [Entry point](../../UI-V2/src/components/chat/Composer.tsx) |
| `element:chat-composer-and-runtime:commands` | Slash completions, UAM command feedback, provider options and skill discovery | [Agreement](../chat/composer/commands.md) | [Entry point](../../UI-V2/src/components/views/ChatView.tsx) |
| `element:chat-composer-and-runtime:memory-choice` | Chat memory toggle/policy and inherited versus explicit choice | [Agreement](../settings/memory-policies.md) | [Entry point](../../UI-V2/src/components/chat/Composer.tsx) |
| `element:chat-composer-and-runtime:goals` | Arm next message as goal, token budget, objective/progress, pause/resume and completion | [Agreement](../chat/goals.md) | [Entry point](../../UI-V2/src/components/shared/GoalBanner.tsx) |
| `element:chat-composer-and-runtime:architect` | Architect + worker, worker/reviewer model selection and stepwise review | [Agreement](../chat/architect-worker.md) | [Entry point](../../UI-V2/src/components/chat/Composer.tsx) |
| `element:chat-composer-and-runtime:tools` | Worked/tool/reasoning/plan disclosure; terminal/file/subagent details; Markdown/code copy | [Agreement](../chat/tools-and-content.md) | [Entry point](../../UI-V2/src/components/chat/ToolCallViews.tsx) |
| `element:chat-composer-and-runtime:history` | Load earlier history, navigate messages, scroll latest and preserve raw positions | [Agreement](../chat/tools-and-content.md) | [Entry point](../../UI-V2/src/components/chat/MessageNavigationRail.tsx) |
| `element:chat-composer-and-runtime:edit` | Edit delivered versus definitely unsent message; save new branch; cancel edit | [Agreement](../providers/editing-messages.md) | [Entry point](../../UI-V2/src/components/views/ChatView.tsx) |
| `element:chat-composer-and-runtime:branch` | Choose branch family/history; create branch; reject empty-chat branching | [Agreement](../providers/branching.md) | [Entry point](../../UI-V2/src/components/chat/ChatBranchPicker.tsx) |
| `element:chat-composer-and-runtime:side-chat` | Create/resume/return/dismiss side chat and reference context | [Agreement](../providers/side-chats.md) | [Entry point](../../UI-V2/src/components/views/ChatView.tsx) |
| `element:chat-composer-and-runtime:runtime` | Cancel turn, Stop runtime, waiting/ready/failed/stopped state, timeout/grace and restart/resume | [Agreement](../providers/runtime-lifecycle.md) | [Entry point](../../UI-V2/src/components/chat/ConversationWork.tsx) |
| `element:chat-composer-and-runtime:compaction` | Manual compact where available, compacting animation/timer and collapsed completed/interrupted summary | [Agreement](../providers/compaction.md) | [Entry point](../../UI-V2/src/components/chat/StatusScenes.tsx) |
| `element:chat-composer-and-runtime:subagent` | Related child activity, waiting state, tool details and addressable child history | [Agreement](../providers/agent-history.md) | [Entry point](../../UI-V2/src/components/chat/SubagentWaitStatus.tsx) |
| `element:chat-composer-and-runtime:uam-tools` | Missing UAM tools, Enable UAM connection, View settings, Continue without it and next-start setup | [Agreement](../providers/uam-tools.md) | [Entry point](../../UI-V2/src/components/shared/ProviderMcpWarning.tsx) |
| `element:chat-composer-and-runtime:workspace-menu` | Open editor/explorer; configured shell actions and actual execution workspace | [Agreement](../chat/composer/workspace-actions.md) | [Entry point](../../UI-V2/src/components/views/ChatView.tsx) |
| `element:chat-composer-and-runtime:terminal` | PTY input/resize, interrupt/arrow controls, startup/attachment retry, CLI settings and errors | [Agreement](../providers/terminal-view/terminal-view.md) | [Entry point](../../UI-V2/src/components/views/CLIView.tsx) |
| `element:chat-composer-and-runtime:view-sync` | Switch Chat/CLI, exact native binding, refresh and honest unsupported continuity | [Agreement](../providers/terminal-view/switching-views.md) | [Entry point](../../UI-V2/src/components/views/CLIView.tsx) |
| `element:chat-composer-and-runtime:context` | Per-chat context/consumption indicator versus independent account allowance | [Agreement](../providers/usage.md) | [Entry point](../../UI-V2/src/components/chat/ProviderAllowanceIndicator.tsx) |

## Files, changes and repository operations

| Element ID | Controls and actions | Owning document | Source |
|---|---|---|---|
| `element:files-changes-and-repository-operations:changes` | Review changes, file/folder tree, status, +/- totals and open file diff | [Agreement](../changes/changes.md) | [Entry point](../../UI-V2/src/components/chat/ChangedFilesTree.tsx) |
| `element:files-changes-and-repository-operations:diff` | Unified/Split preference, baseline, patch metadata, copy, binary/no-text/error and loading | [Agreement](../changes/diffs.md) | [Entry point](../../UI-V2/src/components/shared/RepositoryDiffDialog.tsx) |
| `element:files-changes-and-repository-operations:inspect` | Inspect file, line numbers, copy content, optional Show changes and correct host/path | [Agreement](../files/inspection.md) | [Entry point](../../UI-V2/src/components/shared/FilePreviewDialog.tsx) |
| `element:files-changes-and-repository-operations:receipts` | Workspace versus Recorded chat edits; tool/message link and original captured patch | [Agreement](../changes/recorded-edits.md) | [Entry point](../../UI-V2/src/components/layout/RecordedChatEdits.tsx) |
| `element:files-changes-and-repository-operations:commit` | Git/SVN status, refresh, select files, message title/body, generate suggestion and explicit commit | [Agreement](../changes/commits.md) | [Entry point](../../UI-V2/src/components/layout/VcsCommitPanel.tsx) |
| `element:files-changes-and-repository-operations:checkpoint` | Completed-turn checkpoint, Roll back confirmation, port/discard ownership and failures | [Agreement](../workspaces/checkpoints.md) | [Entry point](../../UI-V2/src/components/views/ChatView.tsx) |

## Usage

| Element ID | Controls and actions | Owning document | Source |
|---|---|---|---|
| `element:usage:history` | Cost/Tokens/Limits, periods, All machines/local/host, refresh, provider filters and source coverage | [Agreement](../usage/history.md) | [Entry point](../../UI-V2/src/components/usage/UsagePage.tsx) |
| `element:usage:charts` | Totals, reported/estimated/known cost, token categories, chart tooltips and model/time breakdown | [Agreement](../usage/costs.md) | [Entry point](../../UI-V2/src/components/usage/UsagePage.tsx) |
| `element:usage:prices` | Inspect model, Model prices, rate source/fallback, save/remove local override and validation | [Agreement](../usage/model-prices.md) | [Entry point](../../UI-V2/src/components/usage/UsagePage.tsx) |
| `element:usage:limits` | Cached allowance freshness, Refresh/cooldown, provider periods, model expansion, credits and unavailable feeds | [Agreement](../usage/allowances.md) | [Entry point](../../UI-V2/src/components/usage/UsageLimits.tsx) |
| `element:usage:diagnostics` | Unknown/unpriced values, partial machine collection, duplicates/fork suppression and stale prior data | [Agreement](../usage/history.md) | [Entry point](../../UI-V2/src/components/usage/UsagePage.tsx) |

## Settings: General and providers

| Element ID | Controls and actions | Owning document | Source |
|---|---|---|---|
| `element:settings-general-and-providers:theme` | Built-in/custom palette, add/copy/edit/delete, import/export, base/name/review and drafts | [Agreement](../settings/appearance/themes.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |
| `element:settings-general-and-providers:fonts` | Interface/input/code/terminal fonts, sizes, previews and Reset typography | [Agreement](../settings/appearance/typography.md) | [Entry point](../../UI-V2/src/components/settings/TypographySettings.tsx) |
| `element:settings-general-and-providers:display` | Message width, contrast, diff colours, animation, provider icons, worktree path and work-trace preferences | [Agreement](../settings/appearance/display.md) | [Entry point](../../UI-V2/src/components/settings/AppearanceDisplaySettings.tsx) |
| `element:settings-general-and-providers:defaults` | Default new-chat provider, model/catalogue visibility, reasoning, speed, native/UAM preference and lightweight mode | [Agreement](../settings/chat-defaults.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |
| `element:settings-general-and-providers:review-defaults` | Reviewer provider/model, initial approval mode and default YOLO confirmation | [Agreement](../settings/permission-defaults.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |
| `element:settings-general-and-providers:scope` | Applying settings for, All environments/SSH host, inheritance and unsupported scoped saves | [Agreement](../settings/environment-scope.md) | [Entry point](../../UI-V2/src/components/settings/SettingsScopeBar.tsx) |
| `element:settings-general-and-providers:availability` | Overview and per-host provider enablement; presence/version/connection status | [Agreement](../settings/provider-overview.md) | [Entry point](../../UI-V2/src/components/settings/ProviderConnections.tsx) |
| `element:settings-general-and-providers:cli` | Six provider pages, CLI path/version/refresh/install/update, confirmations and provider defaults | [Agreement](../settings/providers.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |
| `element:settings-general-and-providers:phone` | Enable publishing, copy URL/token, address change and serving status | [Agreement](../settings/phone-access.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |
| `element:settings-general-and-providers:dictation` | System/Server/Local AI, server URL/endpoint/model/key-variable, Save/Discard and unsupported routes | [Agreement](../settings/voice-input.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |

## Settings: Connections, agents and memory

| Element ID | Controls and actions | Owning document | Source |
|---|---|---|---|
| `element:settings-connections-agents-and-memory:hosts` | Add alias/name, helper preview/location/install, state/update/remove and host icons | [Agreement](../settings/remote-hosts.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |
| `element:settings-connections-agents-and-memory:host-options` | Start runner at login, CLI updates with sudo, inline/legacy instructions and Save/Discard | [Agreement](../settings/remote-hosts.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |
| `element:settings-connections-agents-and-memory:routing` | Automatic enablement, candidate hosts/capacity, provider sign-in confirmation and folder/repository mappings | [Agreement](../remote-environments/routing.md) | [Entry point](../../UI-V2/src/components/settings/ExecutionRoutingSettings.tsx) |
| `element:settings-connections-agents-and-memory:mcp` | Guided/import/Form/JSON; stdio/HTTP/SSE; commands/args/headers/env references; enabled/scope and Save/Discard | [Agreement](../settings/mcp-servers.md) | [Entry point](../../UI-V2/src/components/settings/McpServerSetup.tsx) |
| `element:settings-connections-agents-and-memory:builtins` | Built-in UAM MCP enablement/Computer Use and protected non-deletable entries | [Agreement](../providers/uam-tools.md) | [Entry point](../../UI-V2/src/components/settings/McpServerSetup.tsx) |
| `element:settings-connections-agents-and-memory:central` | Instruction files edit/save/remove, legacy text, default agent/skills/agents and shared setup capture | [Agreement](../settings/central-setup/central-setup.md) | [Entry point](../../UI-V2/src/components/settings/CentralProviderSettings.tsx) |
| `element:settings-connections-agents-and-memory:agent-store` | Create/edit/view/delete agent, built-in read-only, role/access/skills/instructions and immutable ID | [Agreement](../settings/central-setup/agents.md) | [Entry point](../../UI-V2/src/components/settings/CentralProviderSettings.tsx) |
| `element:settings-connections-agents-and-memory:agent-import` | Source provider/path, review, ID/access/scope, skills and omitted-field acknowledgement | [Agreement](../settings/central-setup/agents.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |
| `element:settings-connections-agents-and-memory:agent-cycle` | Favourites/add/remove/reorder and cycle shortcut including Disabled | [Agreement](../settings/central-setup/agents.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |
| `element:settings-connections-agents-and-memory:skills` | Skills directory browse/manual/save; search/group/pin/attach/edit/reveal and Markdown preview | [Agreement](../settings/central-setup/skill-library.md) | [Entry point](../../UI-V2/src/components/settings/MarkdownStoreModal.tsx) |
| `element:settings-connections-agents-and-memory:skill-import` | Create/provider/file/folder import, review/selection, Skip/Replace/Keep separate and overwrite/discard | [Agreement](../settings/central-setup/skill-library.md) | [Entry point](../../UI-V2/src/components/settings/MarkdownStoreModal.tsx) |
| `element:settings-connections-agents-and-memory:memory-policies` | Default/Off/Read Only/Strict/Balanced/Remember Everything; app/provider/current chat | [Agreement](../settings/memory-policies.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |
| `element:settings-connections-agents-and-memory:memory-workers` | Worker provider/model per conversation provider, idle delay, recall byte budget, last run/log | [Agreement](../settings/memory-workers.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |
| `element:settings-connections-agents-and-memory:memory-store` | Global/workspace/all locations, search, create/edit fields, reveal/root/refresh and explicit scoped delete | [Agreement](../memory/store.md) | [Entry point](../../UI-V2/src/components/settings/MemoryLibraryModal.tsx) |
| `element:settings-connections-agents-and-memory:memory-scan` | Choose chats, select all/none, extraction progress, cancel and results | [Agreement](../memory/scanning.md) | [Entry point](../../UI-V2/src/components/settings/MemoryScanModal.tsx) |
| `element:settings-connections-agents-and-memory:goals` | Maximum iterations, setup inactivity timeout and per-turn output ceiling | [Agreement](../settings/goal-loops.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |
| `element:settings-connections-agents-and-memory:shell-actions` | Enable/create/remove, name/group/reorder, provider/model, When run, file/folder input and skill; OS registration | [Agreement](../settings/shell-actions.md) | [Entry point](../../UI-V2/src/components/settings/ShellActionsSettings.tsx) |

## Settings: Security, workspace and app

| Element ID | Controls and actions | Owning document | Source |
|---|---|---|---|
| `element:settings-security-workspace-and-app:os-permissions` | Microphone, Speech Recognition, Local Network, Screen Recording, Accessibility; Request/System settings/Refresh | [Agreement](../settings/permissions.md) | [Entry point](../../UI-V2/src/components/settings/PermissionsSettings.tsx) |
| `element:settings-security-workspace-and-app:computer-policy` | Auto/Provider/UAM, allow requests, readiness, allowed apps, Only listed apps and refresh | [Agreement](../settings/computer-use.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |
| `element:settings-security-workspace-and-app:target-grant` | Application/window/display selection, grant/start, status, Pause/Resume/Stop and errors | [Agreement](../computer-use.md) | [Entry point](../../UI-V2/src/components/chat/ComputerUseModal.tsx) |
| `element:settings-security-workspace-and-app:git` | Git enablement, Automatic isolation, local base branch, Patch/Merge/Squash and pull-before-chat | [Agreement](../settings/version-control/git.md) | [Entry point](../../UI-V2/src/components/settings/VcsSettingsSection.tsx) |
| `element:settings-security-workspace-and-app:svn` | SVN enablement, update-before-chat, unsupported branch/merge choices and Local snapshot Git | [Agreement](../settings/version-control/svn.md) | [Entry point](../../UI-V2/src/components/settings/VcsSettingsSection.tsx) |
| `element:settings-security-workspace-and-app:plain-snapshot` | No VCS Enable local Git files and independent plain/SVN snapshot choices | [Agreement](../settings/version-control/local-snapshots.md) | [Entry point](../../UI-V2/src/components/settings/VcsSettingsSection.tsx) |
| `element:settings-security-workspace-and-app:editors` | Default preset, file-type group names/extensions/presets, add/remove and custom file explorer | [Agreement](../settings/editors.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |
| `element:settings-security-workspace-and-app:chat-bundle` | Export all chats, import bundle, destination, ID collisions and result messages | [Agreement](../settings/chat-data.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |
| `element:settings-security-workspace-and-app:cleanup` | Worktree policy/age; backup log retention; HTTP cache clearing/interval; independent Save/Discard | [Agreement](../settings/storage.md) | [Entry point](../../UI-V2/src/components/settings/StorageCleanupSettingsSection.tsx) |
| `element:settings-security-workspace-and-app:about` | Running version and automatic update checks | [Agreement](../settings/about.md) | [Entry point](../../UI-V2/src/components/settings/SettingsModal.tsx) |

## Phone browser

| Element ID | Controls and actions | Owning document | Source |
|---|---|---|---|
| `element:phone-browser:connection` | Token Connect, connected/disconnected status, refresh and Logout | [Agreement](../phone-browser.md) | [Entry point](../../UI-V2/src/components/companion/CompanionShell.tsx) |
| `element:phone-browser:activity` | Activity active/recent sections, project filter, refresh timestamp and open conversation | [Agreement](../phone-browser.md) | [Entry point](../../UI-V2/src/components/companion/CompanionShell.tsx) |
| `element:phone-browser:chats` | Chats search/folders, New chat, pin/unpin, back and shared permitted chat actions | [Agreement](../phone-browser.md) | [Entry point](../../UI-V2/src/components/companion/CompanionShell.tsx) |
| `element:phone-browser:terminal` | Conversation Chat/CLI choice, attached terminal, touch input and timeout status | [Agreement](../phone-browser.md) | [Entry point](../../UI-V2/src/components/companion/CompanionShell.tsx) |

## Provider features and engineering

Every provider's structured chat, terminal/continuity, lifecycle, concurrency, branching/editing, MCP, skills/attachments, compaction, computer use, side chats, models/agents/subagents, resume, reasoning/speed, permissions, usage, memory, central setup and SSH maps through the [24 required capabilities](../providers/providers.md#required-capabilities) and six complete checklists. Their statuses preserve implementation gaps and uncertainty.

Architecture, source-of-truth records, OS/provider/VCS boundaries, test expectations, logging, build exclusion and change evidence map through [Architecture](../architecture/architecture.md) and [Engineering](../engineering/engineering.md). No new runtime feature or broad module refactor is authorized by documenting its agreement.
