# Native session import

Native import discovers conversations from an included provider and lets the user import eligible history into the correct workspace and execution environment. Discovery is not execution or permission to resume work. Preserve supported text, tools, attachments and native identity, prevent duplicates/deleted-session resurrection and reject confirmed empty content. Authored text, attachments, reasoning, plans and tools qualify; setup-only text, whitespace and compaction/interruption markers alone do not. Unknown, failed or incomplete histories stay retryable, and confirmed-empty sessions can become eligible after receiving real conversation content. Ownership and deletion state must be rechecked before publication, including background startup choices.

[Imports and exports](imports.md) · [Portable bundles](../settings/chat-data.md) · [Source of truth](../architecture/source-of-truth.md).
