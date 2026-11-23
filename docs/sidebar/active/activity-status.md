# Activity status

Status distinguishes running work, waiting for permission or input, completion, interruption, failure and unknown activity. Use actual runtime evidence; Connected must not pretend to mean Ready or Busy when a native terminal cannot expose that distinction. Working indicators explain real work and settle when it ends, while attention indicators remain available until acknowledged or snoozed. A chat held for automation approval shows Waiting for approval, and Working once the user has started it, while its worktree and provider come up. New-work detection compares a chat's last update with its last open as instants, so native imports stamped in UTC and selections stamped in local time do not count as new work.

Each chat row's accessible name says what its status icon shows (running; waiting, with the reason such as permission needed; waiting for approval; watching pull request; done, unread; failed), so the state never depends on colour or an icon alone. No row is a live region. The sidebar's single polite status region announces an Active chat that starts waiting, fails or finishes, by name (“Build fixes: waiting, permission needed”); starting work and the first render announce nothing, and the same region also reports keyboard reordering.

[Motion](../../ui/motion.md) · [Provider evidence](../../reference/provider-runtime-parity.md#runtime-paths).
