# Settings implementation history

> Historical notes from the trial base; the [settings guide](../settings/settings.md) defines behaviour and the [UI settings rules](../ui/settings.md) define presentation. These notes do not establish acceptance.

## SVN new-chat updates

The VCS settings page has a separate SVN “Update before new chat” row, selected from mock B by David on 2026-10-14. It defaults to off and cannot be edited while SVN is disabled. Changes remain in the draft until “Save VCS settings”; “Discard” restores saved values. Enabling this setting requests an SVN working-copy update before a new chat and does not authorize an SVN commit. Claude review and actual component visual/user acceptance remain pending.

## Grouped settings and instruction editing

David selected mock A on 2026-10-15: Shell Actions uses collapsible groups in sidebar order, Git and SVN have separate stacked sections with neutral switches, and shared instruction files open in an inline editor. Collapsing a group retains its drafts; choosing Go back from the unsaved-changes dialog reveals and focuses the invalid action. Boolean controls retain the existing explicit Save and Discard flow.

Central Setup edits only instruction paths already saved in its configuration. File edits have a separate Save file action, disabled while unchanged, and participate in the existing unsaved-changes guard. Read and save failures retain the draft and show the error next to the editor. Saving an unchanged disabled setup still enables shared instructions; an enabled setup with no changes has a disabled Save action.

Save actions use the existing dirty and validation state, including save-and-leave dialogs. Unchanged skill and theme drafts keep Save disabled; invalid Shell Actions, routing, MCP and memory drafts cannot be saved. Keep editing and Discard remain available. Validation appears alongside dismissible backend errors and notifications. MCP guided-setup navigation keeps its own step validation.

## Local snapshot Git

As clarified by David on 2026-10-15, SVN contains a Local snapshot Git switch, and a separate No VCS section contains Enable local Git files for plain folders. Both default on to preserve existing behavior. Turning either off makes future automatic chats of that workspace kind use the original folder; existing isolated workspaces remain intact. These controls use the existing draft Save and Discard flow and depend on Git being enabled, independently of SVN tracking. David explicitly requested the existing style without mock alternatives.
