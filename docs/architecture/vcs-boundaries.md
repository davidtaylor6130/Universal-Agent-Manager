# VCS boundaries

Git and SVN commands, status parsing, revision semantics, path validation and publication rules belong behind VCS service/module interfaces. UI and bridge code request status, comparison or a selected operation without building repository command strings or duplicating Git/SVN rules. Keep shared workspace identity and ownership separate from backend details, and execute against the captured local/remote workspace. Never turn a Git checkpoint into an SVN publication or silently substitute a different repository operation.

**Current structure:** [VcsCommitService](../../src/app/vcs_commit_service.h) and [workspace context services](../../src/app/workspace_vcs_context_service.h) form existing boundaries; Git/SVN implementation is still consolidated in application service code. Separate backend modules are the placement agreement, not an already-completed refactor. OS execution goes through [platform services](platform-boundaries.md), and workspace mutation retains leases, selected-path checks and save/recovery barriers. Document unsupported backend operations rather than simulating success.

[Version Control settings](../settings/version-control/version-control.md) · [Changes and commits](../changes/changes.md) · [Testing](../engineering/testing.md).
