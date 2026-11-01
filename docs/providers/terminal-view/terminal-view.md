# CLI terminal view

Terminal View is the actual provider CLI running interactively in UAM’s embedded xterm.js terminal, including its native terminal UI and input behaviour; it is not a textual rendering of structured chat. It runs in the chat’s selected workspace and execution environment. UAM owns launch, attachment, process lifetime and saved binding, while the CLI owns its internal interface and permission prompts.

- [Switching views](switching-views.md)
- [Session continuity](session-continuity.md)

[Provider sessions](../../architecture/provider-sessions.md).

Steering a CLI terminal sends Ctrl-C, delivers the steer once the terminal is idle, keeps the prompt pending for retry if delivery fails, and refuses to steer while terminal activity is Unknown.

Busy and Ready must come from evidence owned by that terminal and session. When native activity or input admission cannot be proven, display Unknown; an idle-looking screen or an unrelated server must not imply completion, and an idle terminal should not keep background observation processes running.
