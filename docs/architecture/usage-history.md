# Usage architecture

Provider collectors, transcript scans and UAM’s ledger contribute host-scoped usage records. Aggregation and paged transport must preserve one consistent snapshot, avoid duplicate events and keep estimated prices separate from observed tokens. A separate cached account-allowance service reads available local quotas independently of chats; remote feeds never inherit local account values. Missing account or model data stays unknown rather than being represented as zero.

[Usage](../usage/usage.md) · [Provider figures](../providers/usage.md).

[Engineering reference](../reference/architecture/usage-history.md).
