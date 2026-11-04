# Recorded chat edits

Recorded chat edits show captured completed provider file-change receipts, their message/tool association and stored patch. They remain historical evidence even if the workspace later changes; viewing them must not recompute a current workspace diff and claim it is the original edit. Preserve distinct receipts and source/destination paths, and disclose missing, unsupported or failed capture. Shell changes and unattributed edits must not be assigned to a chat merely because they share its workspace.

**Implementation: Partial.** The current view exposes completed Codex edit receipts; shell changes and other providers are not fully attributed. Inspect file reads current content, while the displayed patch comes from the receipt. [Diffs](diffs.md) · [Commit panel](commits.md).
