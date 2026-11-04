# Model prices

Model prices let users inspect the pricing source and set or remove per-model/service-tier overrides for input, cached input, cache writes and output. They change estimates rather than observed tokens or reported provider charges. Missing rates stay unknown; reference fallback rates and their source/freshness must be disclosed. Overrides are local frontend preferences and are not synced by native chat settings or copied to every host. Validate nonnegative finite values, retain previous prices on save failure and distinguish removing an override from declaring a rate of zero.

[Costs](costs.md) · [History breakdown](history.md) · [Source of truth](../architecture/source-of-truth.md).
