# Permission rules settings

The Permission rules page, under Security, holds the global [permission rules](../providers/permissions/rules.md) and the **Ask the AI reviewer** switch. The list starts with the built-in rule that makes pushes, merges and other [remote commits always ask](../providers/permissions/rules.md#remote-commits-always-ask); it has no controls, and its override is a hidden settings-file key rather than a Settings control. The rules list shows each rule's pattern or target with its action, provider and workspace, a switch to disable it without deleting it, and labelled Edit and Delete actions; Add rule and Edit open the shared editor dialog, and Delete asks for confirmation. Every change saves immediately; the backend validates the whole list again and an invalid rule is refused with its reason while the editor keeps the draft. Saved rules are validated again on load, and invalid entries are dropped rather than broadened.

**Test a command** checks a typed command against the saved rules with the same native matcher the permission flow uses and reports which rule decides it, including the built-in remote-commit rule, or that none does; a deciding rule limited to a provider or workspace is named with that scope, and the read-only allow is reported only when the command passes the same credential check as at run time. The read-only check there assumes relative paths sit inside a chat's workspace; at run time the chat's real workspace, permission level and ceilings still apply, so an allow result can still be refused for a high-risk request.

Rules are global and not [environment-scoped](environment-scope.md). The reviewer provider and model stay under [Chat defaults](permission-defaults.md).

[Settings](settings.md) · [UI settings](../ui/settings.md).
