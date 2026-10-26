# Recorded product decisions

This page records decisions already agreed with David. It also states whether the current code implements them.

## One notification system

**Agreed 2026-10-12:** app-wide messages should use the notifications panel and one shared transient alert surface. Replace feature-specific popups, including the collection-move failure toast and bottom-right upgrade notice. Features must not add their own popup systems.

**Implementation:** Implemented. The V5 log row `Single Notification System` retains pending verification, Claude Review and user testing; [Notifications](../ui/notifications.md) owns the delivered behaviour.

## Remove organization undo

**Agreed 2026-10-12:** remove the organization undo feature, including its toast, store and backend undo payloads for workspace, collection, reference and resource reorder/move operations. Route failures through notifications.

**Implementation:** Implemented. The organization undo toast, store and payloads are removed; the V5 log row `Remove Organization Undo` retains pending verification, code review and user testing.

Do not mark either decision complete from documentation alone; completion requires the V5 log's review and user-test flags to clear.

## Existing VCS panel

**Decision, 2026-10-25:** David keeps the existing Git/SVN commit panel in V5 and asked for it to be fixed rather than removed (answer relayed through the V5 coordinator for the SVN support task). This resolves the 2026-10-16 discrepancy in which older release instructions excluded dedicated VCS panels. It keeps the existing panel only; new panel features such as branch, tag, revert, lock or changelist controls still need their own approval. The Release Slice wording in `AGENTS.md` still lists VCS panels as excluded and needs a matching instruction update.
