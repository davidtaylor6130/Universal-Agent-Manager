# Shared interaction

Interactions preserve the user’s context and make the next action clear: one primary action per area, readable state, keyboard access and progressive disclosure of technical details. Loading, empty, success and failure paths are part of the feature. Technical identifiers are secondary, and an async refresh must not erase usable content or unrelated drafts.

Patterns:
- Desktop window controls share an app-level strip with split-layout actions, while chat search stays in the sidebar and every split pane retains its own header. Empty strip space drags the window; interactive controls and modal overlays exclude dragging. macOS uses native traffic lights, and Windows caption buttons invoke native window actions.
- Settings pages follow [settings.md](settings.md): page title only, short section labels, one concise consequence per row, actions right-aligned and no nested decorative cards.
- Lists/rows: identity line (text), detail line (mono, --text-3, for ids/paths), status line (StatusDot + semantic colour); icon-only actions on the right with Tooltip + aria-label.
- Notices: tinted, calm, plain-language consequence; collapsible/dismissible where long.
- Sidebar: keep separate Active Chats and Pinned Chats sections above the workspace tree, including Active Chats collapse, status counts, Clear all, Done and Snooze controls; do not replace these sections with a filter.
- Changed-file review (end of an assistant turn): a directory browser, not a flat list. Single-child folder chains collapse, folders show aggregated +/- counts, files show a coloured status letter (M/A/D/R) and +/- counts, and the card scrolls inside a 280px max height. Folders beyond the first two levels start collapsed when there are more than 8 files. Keyboard: Up/Down move, Left/Right collapse/expand, Enter opens. Paths are mono. Split text diffs omit patch framing but retain rename and permission metadata; metadata-only changes keep their headers, and unified/copy views retain the original patch.
- Empty new chat: a short heading and the composer centre in the pane; the first send moves the composer down to its normal position (FLIP, transform only; reduced motion jumps).
- Message navigation reserves a padded local gutter outside transcript content, including narrow split panes; expanded marks and keyboard focus stay clear of messages and controls.
- Transcript turns are quiet: the provider and model show above a turn only when they change from the previous assistant turn (the full time is on hover and for screen readers), day dividers mark day changes, and the "Worked" toggle is hidden for brief turns with no tool or reasoning activity.
- Context chips (`components/chat/ContextChip.tsx`): referenced context (attachments, skills) is a 24px pill with a type icon, name and kind label on var(--surface-up). Full path is a Tooltip in mono; files open the preview on click; remove is an icon button on composer chips only. Kinds include file, folder, skill, image and branch (GitBranch icon). Assistant replies chip inline code that is clearly a file path, folder or branch; in prose the chip is vertical-align middle with negative block margin so the line height does not change.

- Custom icons (chats, workspaces, hosts, collections): pick from the searchable Lucide library first (stored as text `lucide:<name>`), with Emoji and PNG upload as secondary tabs; render through `components/shared/CustomIcon.tsx`.

Provider questions show each option’s consequence beside its label for sighted and screen-reader users; approval choices must not hide essential decision text in accessibility-only content.

UX rules (enforce):
1. One clear primary action per section; rest secondary/ghost.
2. Status ≠ configuration: show "what is happening" (badges/dots/state text) distinctly from "what you set" (fields/switches).
3. Plain-language labels stating consequence. Remove internal jargon from UI copy ("ownership", "admission", "scope", "revision", "guarded", "durable", "hydrate", "publication"). British/neutral English is fine.
4. Every async surface: designed loading, empty and error states (not blank, not raw error dumps).
5. Fully keyboard reachable, visible focus ring, aria-labels on icon buttons.
6. Technical detail (paths, PIDs, IDs, raw errors) collapsed or secondary mono — never the headline.
7. Density: no oversized cards/padding; align to a grid; consistent heights (controls 26/32px).
8. Don't invent features or data the code does not provide.

The sidebar and composer patterns above link to their owning [sidebar](../sidebar/sidebar.md), [composer](../chat/composer/composer.md) and [message](../chat/messages.md) agreements. [Settings](settings.md) is authoritative over older general settings examples.

[Diffs and change review](../changes/changes.md), [file inspection](../files/inspection.md) and [navigation](../navigation/navigation.md) own the behaviour of those surfaces; the patterns above govern presentation.

Subagent activity follows the [T3 alpha 0.1.0 workflow and compact-card reference](../chat/subagents.md), including its provider icon, title/status, single summary line and trailing time/chevron; working state uses UAM’s blue dashed spinner with no green dot. Using UAM primitives, a quiet collapsible group contains individually named children, children nest beneath their main chat in All Chats and stay out of Active, selecting a child displays only its conversation, a fixed top parent return control preserves context, and transcript previews are secondary and opt-in. Only reported child state contributes to aggregate progress; a tool’s completion is not child completion.
