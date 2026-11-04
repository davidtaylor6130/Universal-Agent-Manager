# Memory settings

Memory settings control extraction, recall, storage, worker selection and budgets for UAM’s [durable memory](../memory/memory.md). Off prevents future recall and AI extraction; Read Only allows recall but prevents AI extraction. App, provider and explicit chat choices must follow their documented inheritance, and switching provider preserves an explicit chat selection. Turning memory off cannot retract memory already included in an existing provider conversation.

[Memory store](../memory/store.md) · [Memory reference](../reference/architecture/durable-memory.md).

[Policy choices](memory-policies.md) · [Manual scans](../memory/scanning.md).

[Worker bindings, idle delay, recall budget and last-run diagnostics](memory-workers.md).

Immediate memory-setting saves preserve newer requests while earlier writes settle. UAM confirms a save only from the native committed outcome. If a save request was not sent, UAM restores the latest accepted native display values for those fields, or their previous displayed or validated baseline when no update arrived, while retaining other unresolved edits. An ordinary native update can refresh this display fallback but cannot confirm a save or settle an unconfirmed draft. If a dispatched write cannot be confirmed, UAM retains the requested values as visibly unconfirmed drafts that may differ from the running app, without automatically retrying. This warning remains visible in Settings across navigation and ordinary native refreshes until those drafts are settled. A later confirmed memory-settings snapshot settles older unconfirmed edits while preserving newer pending edits; a known refusal restores validated saved values. Ordinary native refreshes do not erase pending or unconfirmed edits.
