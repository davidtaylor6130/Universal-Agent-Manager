# SSH execution

SSH execution runs providers and workers through a verified UAM helper on a configured OpenSSH alias, keeping authentication and host verification with OpenSSH. Reconnect must recover the correct process and output without replaying delivered prompts. Instructions, skills, attachments and MCP configuration must be usable on that computer. Helper updates must respect active work and preserve recovery; local editor actions and remote desktop computer use are not implied by SSH support.

**Implementation: Partial.** Linux x86-64/ARM64 and Windows x86-64 helper routes exist; macOS remote-helper packaging and various live/provider combinations remain unverified. See [architecture](../architecture/execution-hosts.md), [provider checklists](../providers/providers.md#provider-checklists) and [build flags](../build-flags.md).
