# Engineering agreements

Engineering rules describe how to change UAM without weakening product behaviour, data authority or module boundaries. Start with the owning feature and [architecture](../architecture/architecture.md), then choose verification proportional to the change. Keep evidence, implementation, review and David's acceptance distinct; existing failures and exceptions must remain visible.

- [Testing](testing.md): meaningful coverage and how much verification to run.
- [Logging](logging.md): useful diagnostics, noise and private data.
- [Durable writes](durable-writes.md): how saved files are replaced, including volumes without atomic exchange.
- [Change workflow](changes.md): documentation, ownership and commit evidence.
- [Code structure](../architecture/code-structure.md), [platform](../architecture/platform-boundaries.md), [provider](../architecture/provider-boundaries.md) and [VCS](../architecture/vcs-boundaries.md) placement rules.
