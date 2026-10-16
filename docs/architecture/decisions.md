# Recorded product decisions

This page records decisions already agreed with David. It also states whether the current code implements them.

## One notification system

**Agreed 2026-10-12:** app-wide messages should use the notifications panel and one shared transient alert surface. Replace feature-specific popups, including the collection-move failure toast and bottom-right upgrade notice. Features must not add their own popup systems.

**Implementation:** pending. The V5 log tracks `Single Notification System` as Open with Claude Review and user testing outstanding. Until implementation lands, existing separate notices remain in the product.

## Remove organization undo

**Agreed 2026-10-12:** remove the organization undo feature, including its toast, store and backend undo payloads for workspace, collection, reference and resource reorder/move operations. Route failures through notifications.

**Implementation:** pending. The V5 log tracks `Remove Organization Undo` as Open with Claude Review and user testing outstanding. Existing `organizationUndo` code and tests still describe current behavior.

When either decision is implemented, update this page and its matching V5 log row in the same change. Do not mark either decision complete from documentation alone.

## Existing VCS panel

**Unresolved scope discrepancy, 2026-10-16:** release instructions exclude dedicated VCS panels, but the branch exposes a working Git/SVN commit panel with workspace and recorded-edit views. The [Changes guide](../changes/changes.md) documents that existing surface so it is not omitted from coverage. Preserve existing code and record the disagreement; this documentation task neither removes the panel nor approves expanding release scope. David's scope decision remains outstanding.
