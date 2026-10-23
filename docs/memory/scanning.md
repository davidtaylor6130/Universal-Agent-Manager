# Memory scans

A manual memory scan lets the user select saved conversations as candidates for extraction; it is separate from normal background extraction and from merely browsing the library. Show eligible scope, selection, progress, cancellation and partial failure without starting provider work on behalf of unselected chats. Results must pass the current memory policy, destination ownership and content validation before publication. Retrying a scan must not overwrite user-authored memories or duplicate already accepted output, and cancellation must not be presented as successful completion.

A scan must prepare its complete selected input before starting extraction; it must not silently replace a requested range with a shorter tail or advance progress after incomplete preparation. If the input exceeds supported memory limits or cannot be read safely, report the failure and retain the scan intent and previous progress. Saved conversations, queued input and recovery records remain unchanged. Large-history extraction beyond the current input limit remains an implementation gap, not a successful empty result.

[Memory policies](../settings/memory-policies.md) · [Memory store](store.md) · [Settings](../settings/memory.md).
