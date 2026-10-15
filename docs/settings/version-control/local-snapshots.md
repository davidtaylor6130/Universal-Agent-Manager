# Local snapshots

UAM can use local Git files to snapshot plain folders and SVN workspaces without making them upstream Git projects. The No VCS “Enable local Git files” switch and SVN “Local snapshot Git” switch are separate and default on. Turning either off affects future automatic local preparation for that workspace kind and does not delete existing worktrees or change remote Git operations. Existing damaged or interrupted preparation must report its state rather than silently falling back.

[Git](git.md) · [SVN](svn.md).
