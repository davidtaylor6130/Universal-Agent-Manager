# Version Control

Version Control settings configure the supported Git and SVN connections and UAM’s optional local snapshot behaviour. These systems have separate purposes and switches: provider conversations, Git worktree isolation, local checkpoints and publication to an upstream repository are not the same operation. Local snapshot settings must not silently change remote Git behaviour or authorize an SVN commit. Dedicated VCS panels remain outside this release slice.

Settings edits remain a draft until Save succeeds. An unsaved-changes leave prompt clears when a successful save makes the draft clean, without resuming the earlier navigation request; a subsequent navigation request leaves directly. Discard and leave is unavailable while a save is in progress. A failed save retains the draft and any open leave prompt, and makes Discard and leave available again.

- [Git](git.md)
- [SVN](svn.md)
- [Local snapshots](local-snapshots.md)
- [Workspaces](../../workspaces/workspaces.md)
