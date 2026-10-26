# VCS boundaries

Git and SVN commands, status parsing, revision semantics, path validation and publication rules belong behind VCS service/module interfaces. UI and bridge code request status, comparison or a selected operation without building repository command strings or duplicating Git/SVN rules. Keep shared workspace identity and ownership separate from backend details, and execute against the captured local/remote workspace. Never turn a Git checkpoint into an SVN publication or silently substitute a different repository operation.

**Current structure:** [VcsCommitService](../../src/app/vcs_commit_service.h) and [workspace context services](../../src/app/workspace_vcs_context_service.h) form existing boundaries; Git/SVN implementation is still consolidated in application service code. Separate backend modules are the placement agreement, not an already-completed refactor. OS execution goes through [platform services](platform-boundaries.md), and workspace mutation retains leases, selected-path checks and save/recovery barriers. Document unsupported backend operations rather than simulating success.

[SvnBranchService](../../src/app/svn_branch_service.h) owns bounded explicit-parent discovery, working-copy/foreign-user validation and revision-pinned switching through an injected platform command adapter. The existing composer bridge owns chat-incarnation/history checks, runtime settlement and its exclusive workspace lease; repository switching changes working-copy state without writing Git isolation metadata or changing the chat’s working directory.

The shared SVN scope builder projects every local chat’s active, worktree and retained source paths, with folder fallback, so bridge ownership checks are exercised through the service boundary. Full working-copy validation inventories recursive SVN metadata, including excluded nodes, before mutation and final confirmation; root depth and clean status alone are insufficient.

[Version Control settings](../settings/version-control/version-control.md) · [Changes and commits](../changes/changes.md) · [Testing](../engineering/testing.md).
