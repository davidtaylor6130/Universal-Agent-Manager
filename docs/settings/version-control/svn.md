# SVN

SVN integration is distinct from UAM’s local Git snapshots. “Update before new chat” defaults off and requests an update for an eligible new working-copy chat, not a commit or an update of every resumed conversation. Updates must surface conflicts or uncertain outcomes before provider work proceeds. Explicit SVN publication must respect the selected paths and preserve unselected descendants; remote scheduling remains a separate implementation boundary.

[Local snapshots](local-snapshots.md) · [Workspace reference](../../reference/architecture/chats-imports-workspaces.md#svn-before-first-launch).
