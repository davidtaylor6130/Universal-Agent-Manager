# Voice Input settings

Voice Input selects system dictation, local AI or server AI and configures the corresponding engine or service, rather than changing the conversation’s language model. Local mode uses a configured transcription command; server mode uses a configured endpoint, transcription model and credential reference. Unsupported OS facilities, missing commands and failed network transcription must be visible. Audio goes to the selected transcription route, and the resulting text is returned to the [composer](../chat/composer/voice-input.md).

**Implementation: Needs verification** for end-to-end operation on each platform; all three configuration routes exist. See [source map](../reference/source-map.md).

Native playback settings select the activation shortcut, language, installed voice, speed, volume and whether voice mode reads new responses. These device-local preferences do not change provider models, permissions or the selected transcription route. Voice input and playback do not run at the same time; missing installed voices and unavailable desktop speech services are reported explicitly.
