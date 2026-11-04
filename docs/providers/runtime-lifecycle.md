# Runtime controls

Runtime state distinguishes startup, ready, working, waiting, stopping, stopped and failed or unknown activity. Cancelling a turn asks the current work to stop; stopping a runtime owns process cleanup and is not permission to delete the conversation or replay input. Restart/resume must retain native identity and delivery evidence, and timeout or lost UI attachment alone must not prove the provider exited. Show pending or failed cleanup, preserve definitely unsent input and use explicit recovery choices when delivery is uncertain. Native terminal interrupt, detach and provider exit retain their own meanings.

Shutdown must preserve a saved, undispatched remote queue when an idle runtime has no corresponding queued input or active turn; an empty runtime queue does not prove that saved input was consumed. A retiring runtime and its late stop results must not overwrite a replacement chat's queue, authored state or delivery evidence. Failed stop-worker setup or pending remote-stop publication must retain the original process fields; the runtime switches from running to pending remote stop only after successful publication.

**Implementation: Partial.** The shared runtime has shutdown projection and stop-handoff guards; independent cold Retry process lifetime and cleanup remain incomplete. Verification, review and acceptance are recorded in the [V5 change log](<../../V5 Improvement Change log.md>).

[Sending and recovery](../chat/sending.md) · [Save/resume](resuming.md) · [Terminal View](terminal-view/terminal-view.md) · [Activity states](../sidebar/active/activity-status.md).
