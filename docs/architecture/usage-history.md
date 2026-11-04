# Usage architecture

Provider collectors, transcript scans and UAM’s ledger contribute host-scoped usage records. Aggregation and paged transport must preserve one consistent snapshot, avoid duplicate events and keep estimated prices separate from observed tokens. A separate cached account-allowance service reads available local quotas independently of chats; remote feeds never inherit local account values. Missing account or model data stays unknown rather than being represented as zero.

[Usage](../usage/usage.md) · [Provider figures](../providers/usage.md).

[Engineering reference](../reference/architecture/usage-history.md).

Provider adapters also supply validated reviewer usage receipts bound to the current turn and native session to the shared goal loop. GoalService applies those receipts before further goal admission; cumulative receipts use a session-bound starting total captured only from validated adapter receipts, separately from mutable display telemetry, while missing usage remains unknown. Replayed lower cumulative snapshots retain the highest observed usage for the same review receipt. These runtime receipts do not import history-ledger totals or replace native provider-managed goal accounting.
