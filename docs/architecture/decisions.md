# Recorded product decisions

This page records decisions already agreed with David. It also states whether the current code implements them.

## One notification system

**Agreed 2026-10-12:** app-wide messages should use the notifications panel and one shared transient alert surface. Replace feature-specific popups, including the collection-move failure toast and bottom-right upgrade notice. Features must not add their own popup systems.

**Implementation:** Implemented. Verification, Claude review and user testing remain pending (`Single Notification System` in the [archived V5 change log](https://github.com/davidtaylor6130/Universal-Agent-Manager/blob/1d4865fcb/V5%20Improvement%20Change%20log.md)); [Notifications](../ui/notifications.md) owns the delivered behaviour.

## Remove organization undo

**Agreed 2026-10-12:** remove the organization undo feature, including its toast, store and backend undo payloads for workspace, collection, reference and resource reorder/move operations. Route failures through notifications.

**Implementation:** Implemented. The organization undo toast, store and payloads are removed; verification, code review and user testing remain pending (`Remove Organization Undo` in the [archived V5 change log](https://github.com/davidtaylor6130/Universal-Agent-Manager/blob/1d4865fcb/V5%20Improvement%20Change%20log.md)).

Do not mark either decision complete from documentation alone; completion requires the tracked review and user-test gates to clear.

## Existing VCS panel

**Decision, 2026-10-25:** David keeps the existing Git/SVN commit panel in V5 and asked for it to be fixed rather than removed (answer relayed through the V5 coordinator for the SVN support task). This resolves the 2026-10-16 discrepancy in which older release instructions excluded dedicated VCS panels. It keeps the existing panel only; new panel features such as branch, tag, revert, lock or changelist controls still need their own approval.

**Decision, 2026-11-11:** David asked for SVN to be a first-class option in the GUI: a log and history view with changed paths and per-revision diffs, plus status, diff, update, commit, revert, blame and branch or tag switching where they make sense. The first slice adds [history and update](../changes/commits.md#history-and-update) to the existing panel. After trying a mock the same day he asked for three additions in that slice: clearer separation of the panel's views, a per-repository setting for which folders hold trunk, branches and tags, and a comparison of a branch with its origin. After a second look he kept the branch graph and its one-line copy point with a List or Graph switch, and removed the rest from this slice: view headings other than Commit, ahead and behind counts and the diff against the origin. Queued as separate later slices: merging from the origin with a dry run (which brings back ahead, behind and the diff against it), revert and resolve, a branch or tag switcher with create, and blame with file history. Drawing merges in the graph and the Git-over-SVN mode also remain unbuilt; locks and changelists were not requested and still need approval. The Release Slice in `AGENTS.md` now names the kept commit panel and this V5.1 SVN work as the in-scope exceptions. Review and David's acceptance of the first slice are pending.
