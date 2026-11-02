# Changes and review

Change review lets users inspect the current workspace or recorded chat edits before taking an explicit repository action. These are different views: a workspace diff can include human, shell and other-chat changes, while a recorded edit is evidence captured from a particular provider operation. Neither view alone proves who authored every file modification. Keep the selected chat, workspace, host and comparison baseline visible and stable across refreshes.

- [Diff viewer](diffs.md)
- [Recorded chat edits](recorded-edits.md)
- [Commit panel](commits.md)
- [File inspection](../files/inspection.md)
- [Version Control settings](../settings/version-control/version-control.md)

**Scope:** David keeps the existing Git/SVN commit panel in V5 (2026-10-25). This keeps the current surface and does not approve new panel features. See [recorded scope](../architecture/decisions.md#existing-vcs-panel).

[Checkpoints and rollback](../workspaces/checkpoints.md).

A new chat can capture its initial Git working-tree contents, including untracked files, before provider launch without changing the source index or branch. Chat changes compare current contents with that durable, workspace-and-host-bound baseline; an isolated Git worktree retains its explicit base comparison. Existing conversations without a valid original baseline report it as unavailable rather than inventing one. This comparison can include human or other-process edits made after capture and does not replace recorded provider edit receipts. The companion exposes only read-only saved-chat Git comparisons, without desktop commit or baseline-capture authority. The end-of-transcript **Changes from this chat** summary lists every file whose content or mode differs from that baseline, including files with no net line change, and each file opens its baseline patch in the [diff viewer](diffs.md). When the baseline is missing the summary says so rather than listing workspace changes; other read failures show the error with Retry. Capturing a baseline writes Git objects and one ref per chat under `refs/uam/chat-baselines/` in the user's repository (the index, branch and working tree are untouched). Remove them with `git for-each-ref --format='%(refname)' refs/uam/chat-baselines/ | xargs -n1 git update-ref -d`; the affected chats then report their baseline as unavailable. **Implementation note:** SVN workspaces have no chat baseline yet, so their summary is labelled **Working copy changes** and reflects the whole working copy.
