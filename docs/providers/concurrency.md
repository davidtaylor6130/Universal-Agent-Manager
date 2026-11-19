# Concurrent provider instances

Users can run multiple chats and CLI instances at the same time, including multiple instances of one provider on local or configured remote computers. Input, output, approval decisions, cancellation and saved session identity belong to the correct chat; stopping or changing one must not affect another. Background work must remain visible through the sidebar and must not block ordinary interaction. Optional [Compute Pools](../settings/compute-pools.md) limit how many Chat turns may run at once for a provider or model; without a configured pool there is no limit.

[Provider architecture](../architecture/provider-sessions.md).
