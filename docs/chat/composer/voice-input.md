# Voice input

Voice input turns speech into editable composer text using the selected system dictation, local AI or server AI mode. System uses the operating system’s dictation; local AI uses a configured local transcription engine; server AI sends the recording to the configured transcription service. Mode, recording, cancellation and failure must be clear, and an old transcription must not overwrite a newer recording or another chat’s draft. Transcription does not itself authorize sending the message.

[Voice settings](../../settings/voice-input.md).

The desktop also offers an explicit voice mode and native speech playback. Voice mode retains owned recording/playback IDs, pauses input during playback and stops when its selected chat or runtime changes. Ordinary spoken text remains an editable draft; in voice mode, the explicit commands “send message”, “stop response”, “next chat”, “previous chat” and “stop voice mode” invoke the corresponding existing actions. Playback can read a completed response without starting a provider turn; unsupported native services and failures remain visible. The companion uses its keyboard microphone rather than desktop audio capture. Consolidated macOS/Windows operation and user acceptance remain unverified.
