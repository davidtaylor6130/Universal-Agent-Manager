# Voice input

Voice input turns speech into editable composer text using the selected system dictation, local AI or server AI mode. System uses the operating system’s dictation; local AI uses a configured local transcription engine; server AI sends the recording to the configured transcription service. Mode, recording, cancellation and failure must be clear, and an old transcription must not overwrite a newer recording or another chat’s draft. Transcription does not itself authorize sending the message.

[Voice settings](../../settings/voice-input.md).

The companion uses its keyboard microphone rather than desktop audio capture; its composer placeholder and microphone tooltip say so. Consolidated macOS/Windows dictation operation, the companion route and user acceptance remain unverified.

The composer does not offer voice mode or read-aloud playback: David removed them on 2026-10-26 ([decision](../../documentation-review.md#composer-actions-voice-mode-and-terminology)). Dictation through the composer microphone is the voice feature. The unused native speech-output services remain to be deleted in a backend follow-up recorded in the [archived V5 change log](https://github.com/davidtaylor6130/Universal-Agent-Manager/blob/1d4865fcb/V5%20Improvement%20Change%20log.md).
