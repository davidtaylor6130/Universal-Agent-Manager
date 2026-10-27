# Structured chat

Structured chat presents assistant text, reasoning when available, tools and results, questions, permissions and lifecycle state through the common UAM conversation. Providers can use different protocols, but their events must have consistent user meanings. Starting, stopping, errors and interrupted responses must be visible without exposing raw protocol output as conversation text. All six providers, including Antigravity, are in scope; a native stream JSON adapter is valid structured delivery and does not need to speak ACP on the wire.

[Provider architecture](../architecture/provider-sessions.md).

[Runtime controls](runtime-lifecycle.md) define cancellation, stopping and recovery independently of display state.

## Input delivery

Each submitted user input follows the shared [admission and delivery agreement](../chat/sending.md), independently of later provider activity. Saved input identity and delivery facts remain historical data; they do not grant retry or execution permission. **Implementation note — Partial:** exact per-input confirmation is being completed; a local write observation does not establish remote delivery or provider acceptance, and confirmation across every supported provider and execution environment remains unverified.

Eligible ordinary or separately goal-owned v3 direct native steering retains its immutable request envelope through the full-save receipt and rechecks the exact parent turn, host, process and token against saved process authority before transport. Pre-write refusal retains unsent input; an uncertain remote write retains the committed envelope and recovery authority. Small-model direct steering also retains the external goal owner and iteration; fresh raw queued input now has an owned promotion stage and refusal rollback under the same transport scope, with focused native verification; referenced recovery carriers retain distinct ownership barriers; see the [durability inventory](../reference/asynchronous-send-durability.md), with broader provider and live verification pending.

Cold process and remote control identities share a full save with the bound retained input before one captured launch attempt. The launch owner rechecks configuration, native identity, Stop/cancel and data-root authority after the receipt; the receipt supplies neither process creation nor provider readiness. New launch and existing-process attachment use the owned stage; focused native verification passed, while explicit restart/control transitions and unsafe cold Retry retain their separate boundaries in the [durability inventory](../reference/asynchronous-send-durability.md).
