# Memory policies

Memory policies define whether UAM may recall existing knowledge and create new memories. Default inherits the applicable app/provider policy; it is not a fixed creation level. Off blocks future recall and AI extraction; Read Only permits recall without extraction. Strict Creation saves only critical, high-confidence candidates, Balanced Creation saves useful medium/high-confidence candidates, and Remember Everything admits every valid non-sensitive candidate rather than indiscriminately saving raw conversation. All creation modes retain ownership, validation and sensitive-data checks. An explicit chat selection survives provider switching; inherited selections follow the destination defaults. No mode retracts memory already included in a provider conversation.

[Memory settings](memory.md) · [Memory store](../memory/store.md) · [Scanning](../memory/scanning.md).
