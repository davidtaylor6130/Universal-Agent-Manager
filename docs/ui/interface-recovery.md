# Interface recovery

An interface or lazy-panel failure should remain contained so unrelated chats stay available and owned provider work is not silently stopped. The recovery surface distinguishes dismissing a failed panel from reloading the interface, warns about unsaved draft loss and moves focus to usable recovery actions. Reload refreshes the UI; it is not permission to replay prompts, restart a provider or discard durable recovery state. Loading, unavailable native bridge and network disconnection must not masquerade as a successful empty workspace.

[Navigation](../navigation/navigation.md) · [Runtime controls](../providers/runtime-lifecycle.md) · [Source of truth](../architecture/source-of-truth.md).
