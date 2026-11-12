# AI Review

AI Review maps to the provider’s supported automatic risk-review mode where UAM has an established mapping; otherwise UAM’s configured reviewer handles eligible requests. Routine allowed work can proceed, while risky, uncertain, failed or unsupported decisions return to the user. Residual native prompts stay user decisions. AI Review must not silently turn into YOLO or widen Plan/read-only restrictions, and any reviewer’s access to tools or request contents must be described honestly.

UAM classifies the typed native command, never its display text. High-risk requests go straight to the user and never reach UAM's reviewer: command chaining or redirection, network access (including web fetch and search), deletion, shells, interpreters and indirection such as `env` or `xargs`, package installation, Git or SVN commands outside the [admitted reads](permissions.md#captured-read-restrictions), paths outside the workspace, edits outside [Accept Edits' scope](auto-accept.md) and Codex permission escalations. The reviewer prompt marks the request as data written by the agent under review, escaped so it cannot close that marker, and tells the reviewer never to follow instructions inside it.

[Reviewer settings](../../settings/permission-defaults.md) · [Current native mappings](../../architecture/permissions-notifications.md).
