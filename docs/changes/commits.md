# Commit panel

The existing commit panel operates on the selected chat's workspace and chosen Git/SVN backend. Users inspect changes, explicitly select files and supply a commit message; generated message suggestions remain editable and do not commit anything by themselves. Validate current workspace and selected paths before mutation, preserve unselected changes and report uncertain or failed publication honestly. A Git commit, push, SVN commit and UAM checkpoint are distinct operations; the panel must not imply one authorizes another. Changing chats while work completes must not publish the old result into the new panel.

David keeps this existing surface in V5 ([recorded scope](../architecture/decisions.md#existing-vcs-panel)); that is not approval to expand it. [Diffs](diffs.md) · [Recorded edits](recorded-edits.md) · [VCS boundaries](../architecture/vcs-boundaries.md).
