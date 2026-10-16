# Memory workers and budgets

Memory Settings maps each conversation provider to a supported extraction worker provider/model; this can differ from the model answering the chat. Changing worker provider clears an incompatible model selection. Idle delay controls when eligible chat memory is processed, and recall budget bounds how much saved memory is added to a prompt; neither setting expands the memory policy or authorizes a manual scan. Last run, worker status and bounded diagnostic output describe an observed extraction attempt, not proof that valid memory was saved. Failures must retain actionable state without exposing sensitive conversation or memory contents in routine logs.

[Memory Settings](memory.md) · [Policies](memory-policies.md) · [Scans](../memory/scanning.md) · [Logging](../engineering/logging.md).
