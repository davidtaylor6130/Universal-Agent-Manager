# Platform boundaries

OS-specific processes, PTYs, dialogs, paths, dictation, file-manager launches and application integration belong behind the platform interfaces. Shared application, runtime and bridge code asks for a capability through those interfaces instead of embedding macOS/Windows command strings, native handles or scattered OS conditionals. Put a missing operation into the appropriate interface and implement its supported platform routes; report unsupported capability explicitly. Keep process-tree ownership, cancellation and cleanup semantics consistent without pretending different OS guarantees are identical.

**Current structure:** [platform services](../../src/common/platform/platform_services.h) provide process/terminal and other interfaces with platform implementations. Computer use and some feature adapters have their own platform-specific files; they should retain narrow interfaces too. This is a placement rule, not a claim that every existing OS branch has been migrated. Test common contracts and the affected OS implementation; a macOS fixture pass does not establish Windows behaviour.

[Code structure](code-structure.md) · [Execution environments](execution-hosts.md) · [Testing](../engineering/testing.md).
