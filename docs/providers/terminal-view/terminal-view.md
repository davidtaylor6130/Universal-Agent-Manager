# CLI terminal view

Terminal View is the actual provider CLI running interactively in UAM’s embedded xterm.js terminal, including its native terminal UI and input behaviour; it is not a textual rendering of structured chat. It runs in the chat’s selected workspace and execution environment. UAM owns launch, attachment, process lifetime and saved binding, while the CLI owns its internal interface and permission prompts.

- [Switching views](switching-views.md)
- [Session continuity](session-continuity.md)

[Provider sessions](../../architecture/provider-sessions.md).

Closing a CLI terminal never types into it. The composer may hold unsent text, and a typed quit command's Enter would submit that text as a live prompt. Every stop ends the process through the platform instead. Idle shutdown and Quit send a hangup on macOS, then terminate the CLI if it has not exited after 2.5 seconds; on Windows they terminate the process tree. Unsent text is therefore discarded, never submitted. UAM also guesses whether the composer holds a draft: input since the last Enter, including UAM's own unsent status query. While it may, a background terminal is not stopped for inactivity and shows no idle countdown. The guess only decides whether to keep a terminal open, and safety never depends on it, because Enter does not always submit (for example in a file picker or an approval overlay). Steering into a draft is a [known gap](../../documentation-review.md#known-gaps-retained).

Steering an idle CLI terminal delivers the steer at once; a busy terminal gets Ctrl-C and the steer once it is idle. A failed interrupt or delivery keeps the prompt pending for retry, and steering is refused while terminal activity is Unknown.

Busy and Ready must come from evidence owned by that terminal and session. Input to a running turn, such as accepting an approval with Enter, does not start a new turn. A submission the provider never records as a turn must not keep the terminal busy. When native activity or input admission cannot be proven, display Unknown; an idle-looking screen or an unrelated server must not imply completion, and an idle terminal should not keep background observation processes running.
