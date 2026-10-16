# Notifications

Chat lifecycle notices belong in the transcript; app-wide failures, warnings, completed system actions and update notices belong in the bell panel with one shared transient alert surface. Features must report through the shared path rather than adding independent popup systems. This is the agreed product direction; the base [decision record](../architecture/decisions.md) still marks consolidation and organization-undo removal as pending implementation.

- Chat status notices (compacting, waiting on subagents, stopped/timeout and response interrupted) are inline transcript dividers, never composer-docked or tagged on a message.
- The compacting divider shows its existing scene, label and elapsed time without a context-fill meter; verified summary text stays hidden during generation and appears only inside the collapsed completed or interrupted summary.
- Definitely undelivered input uses an Unsent recovery row outside the ordinary transcript; direct startup or restart input must not be labelled as future queued work. Legacy rows retain original message positions and offer Retry only when native recovery permits it. A real future queue entry referring to the same input replaces the duplicate recovery row. Partial-output or uncertain-delivery turns remain in the transcript with an interruption divider.
- One notifications panel (the bell rail) holds every app-wide message: failures, warnings, completed system actions and available updates.
- One shared transient alert (`TransientAlertSurface` in `AppShell`, bottom-left, `aria-live="polite"`) briefly shows each new message. It auto-dismisses and the message stays in the panel.
- No feature-specific popups or toasts. Features report failures through `reportAppFailure` (`utils/appFailure.ts`).
- Organisation operations offer no undo toast or organisation-undo shortcut; ordinary text-editor undo remains available. Failed changes are reported through the shared path.

The no-undo agreement concerns organization operations; it does not remove ordinary text-editor undo. [Sending](../chat/sending.md) and [compaction](../providers/compaction.md) own their behaviour; the notes here specify presentation. Do not describe the target notification system as universally shipped until the pending work is resolved.
