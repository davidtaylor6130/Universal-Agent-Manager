# Messages

Messages retain authored input, assistant output, reasoning, tools, attachments and interruption state without confusing generated metadata with user text. Provider/model labels appear when they change rather than adding noise to every turn; dates, timestamps and expandable work remain accessible. Editing delivered messages follows [branching](../providers/editing-messages.md), while compaction and lifecycle notices follow the shared [notification rules](../ui/notifications.md).

Review labels in main and read-only child histories retain each message’s recorded provider, model and local timestamp through stream updates. Reusing unchanged timestamp formatting must preserve the existing display and accessible labels, refresh changed timestamps or metadata, and omit unavailable or invalid times. Turn hover titles and screen-reader times also reflect changed timestamps and local timezone offsets on the next history render, while visible provider/model identities still appear only when they change.

Direct outgoing messages retain their presentation identity and position while admission, delivery or failure is resolved. A pending explanation does not claim delivery or grant Retry; native message or receipt identity and recovery controls remain authoritative, with exact authored input and context retained.

Incomplete ordered-block inventories do not erase separately retained answers, reasoning or tool records. Historical question and permission events remain readable evidence without live response controls, and missing tool records or unavailable stored output are labelled explicitly. Historical plans follow the same nonactionable visibility rule in main and child history while their saved data remains intact.

[Design language](../ui/design-language.md) · [Sending and recovery](sending.md).

[Tools, Markdown/code, copying, history paging and message navigation](tools-and-content.md).
