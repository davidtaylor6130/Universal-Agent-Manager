# Composer

The composer is the place to prepare and send input for the current chat, with its provider, model, agent, permissions and supported reasoning or speed choices. Draft content and context belong to the selected chat. Empty new chats centre the composer; after the first send it moves into the normal transcript layout. Controls must reflect actual capabilities and retained drafts must survive recoverable errors.

The bottom bar's account-allowance control is a [UsageGauge](../../ui/primitives.md) alone, without a separate bar or text: its needle points at the most-used session-reported window, and unavailable, mismatched-provider and credit-only (including unlimited) allowances show the neutral arc. Its accessible name keeps the `Account allowance: …% remaining` (or Cached/Unlimited/Unavailable) wording, and its tooltip keeps per-window remaining percentages and reset times. The composer gauge uses equal-third colour bands, unlike the Limits page's 80%/95% thresholds. [Account allowances](../../usage/allowances.md) own the data rules.

Composer controls live in its bottom bar and its tabs (see [terminology](../../ui/interaction.md)); actions are typed with `/` for commands and skills and `@` for context references rather than added as rows of buttons above the input.

A confirmed send clears only the submitted draft and resources, including when the chat was switched or remounted while acknowledgement was pending; newer text and resources remain. An unconfirmed send retains the draft so its saved state can be checked before retrying.

- [Attachments](attachments.md)
- [Voice input](voice-input.md)
- [Workspace actions](workspace-actions.md)
- [Sending and queues](../sending.md)
- [Models](../../providers/models.md) and [permissions](../../providers/permissions/permissions.md)

[Commands and completions](commands.md) · [Architect + worker](../architect-worker.md).
