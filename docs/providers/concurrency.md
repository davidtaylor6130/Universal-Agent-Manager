# Concurrent provider instances

Users can run multiple chats and CLI instances at the same time, including multiple instances of one provider on local or configured remote computers. Input, output, approval decisions, cancellation and saved session identity belong to the correct chat; stopping or changing one must not affect another. Background work must remain visible through the sidebar and must not block ordinary interaction. Optional [Compute Pools](../settings/compute-pools.md) limit how many Chat turns may run at once for a provider or model; without a configured pool there is no pool limit. [Host limits](../settings/compute-pools.md#host-limits) always cap each computer: user turns and agents share its slots, background work has its own budget, and load over the soft or hard limit holds lower-priority work.

[Provider architecture](../architecture/provider-sessions.md).
