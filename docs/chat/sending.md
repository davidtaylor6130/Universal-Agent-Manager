# Sending and recovery

An idle Send or Resend is direct input, even when startup or restart takes time; only input waiting behind active work belongs in the visible queue. A definitely failed direct input remains one Unsent item with explicit retry or removal, while uncertain delivery or partial provider output must not be automatically replayed. Preserve the authored text, attachments, skills and selected agent during recovery. Stopping work must distinguish an interrupted delivered turn from input known never to have reached the provider.

**Implementation: Partial.** Direct-admission changes are present, but the V5 log retains Send Flashes Unsent and other live recovery findings. See [provider-session reference](../reference/architecture/provider-sessions.md#undelivered-authored-input) and the [V5 change log](<../../V5 Improvement Change log.md>).
