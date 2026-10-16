# Changes and review

Change review lets users inspect the current workspace or recorded chat edits before taking an explicit repository action. These are different views: a workspace diff can include human, shell and other-chat changes, while a recorded edit is evidence captured from a particular provider operation. Neither view alone proves who authored every file modification. Keep the selected chat, workspace, host and comparison baseline visible and stable across refreshes.

- [Diff viewer](diffs.md)
- [Recorded chat edits](recorded-edits.md)
- [Commit panel](commits.md)
- [File inspection](../files/inspection.md)
- [Version Control settings](../settings/version-control/version-control.md)

**Scope discrepancy:** this branch exposes an existing Git/SVN commit panel although release guidance excludes dedicated VCS panels. Document the existing surface and preserve it while that scope is unresolved; this documentation does not authorize removal or new panel expansion. See [recorded scope](../architecture/decisions.md#existing-vcs-panel).

[Checkpoints and rollback](../workspaces/checkpoints.md).
