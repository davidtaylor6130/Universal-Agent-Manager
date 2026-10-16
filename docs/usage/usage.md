# Usage and accounting

Usage views explain tokens, context, estimated spend and provider account allowances without treating them as interchangeable. Retain provider and execution-environment identity, distinguish observed values from estimates and unknown values, and avoid counting the same native event more than once. Aggregate history must stay usable through bounded retrieval rather than loading the full history into one UI update.

[Account allowances](allowances.md) · [Costs](costs.md) · [Provider figures](../providers/usage.md) · [Usage architecture](../architecture/usage-history.md) · [Visual presentation](../ui/visual-style.md).

**Implementation: Partial.** History and independent local allowance readers are present; account eligibility, remote acquisition and platform acceptance remain explicit limits in the [provider checklists](../providers/providers.md#provider-checklists).
