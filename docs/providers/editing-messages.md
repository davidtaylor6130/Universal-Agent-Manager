# Editing messages

Editing an already delivered user message creates a branch from that point, preserving the original conversation and using the revised message in the new path. Editing a definitely unsent input changes that local input without claiming to retract a provider turn. Attachments and selected context must follow the edited input, and uncertain delivery must not be treated as definitely unsent.

Files and skills are selected independently. Retained skills use their original captured prompt text and order; removing a skill removes its file reference and prompt snapshot together. Earlier conversation context stays intact. Explicitly removing all selected context differs from leaving the selection unchanged; a blank revised message needs a retained file or explicitly retained skill context. A selected skill whose captured prompt text is unavailable must be refused without changing the original chat.

**Implementation: Partial** across providers: Implemented for Codex, Claude Code, OpenCode and Copilot, where unsupported recorded-source delivery refuses explicitly; Partial for Gemini and Antigravity, where native-original input delivery and provider continuation of a delivered edit are not yet established. Guarded local saves keep definitely-unsent input and its retained file and skill snapshots in the original chat. Delivered edits now retain independently selected files and captured skill context in the new branch, including explicit removal of all target skills when older snapshots are missing. Retaining unavailable skill snapshots refuses. Native-original input delivery, provider continuation and visual/platform acceptance still need verification. See the [V5 change log](<../../V5 Improvement Change log.md>).

[Sending and recovery](../chat/sending.md).
