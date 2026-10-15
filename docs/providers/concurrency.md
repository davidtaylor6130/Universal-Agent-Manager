# Concurrent provider instances

Users can run multiple chats and CLI instances at the same time, including multiple instances of one provider on local or configured remote computers. Input, output, approval decisions, cancellation and saved session identity belong to the correct chat; stopping or changing one must not affect another. Background work must remain visible through the sidebar and must not block ordinary interaction.

[Provider architecture](../architecture/provider-sessions.md).
