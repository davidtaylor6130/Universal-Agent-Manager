# Chat panes

Users can arrange multiple chats in a resizable split layout, with each pane retaining its own selected chat and Chat or Terminal view. Keyboard focus follows the intended pane and open dialogs take precedence over composer shortcuts. Reopening the application restores the saved layout without starting new provider work merely because a pane becomes visible.

A visible pane that is not focused stays live: while its turn runs, newly committed messages load as they arrive, and completion settles the final transcript without the user clicking the pane.

[Shared interaction](../ui/interaction.md) · [Persistence](../architecture/app-state-persistence.md).
