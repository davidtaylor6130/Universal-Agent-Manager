# Usage and accounting

Usage views explain tokens, context, estimated spend and provider account allowances without treating them as interchangeable. Retain provider and execution-environment identity, distinguish observed values from estimates and unknown values, and avoid counting the same native event more than once. Aggregate history must stay usable through bounded retrieval rather than loading the full history into one UI update.

[Provider figures](../providers/usage.md) · [Usage architecture](../architecture/usage-history.md) · [Visual presentation](../ui/visual-style.md).

**Implementation: Partial.** Token/history routes exist; complete account-allowance coverage for every provider needs verification against the V5 account-usage requirements.
