# Structured chat

Structured chat presents assistant text, reasoning when available, tools and results, questions, permissions and lifecycle state through the common UAM conversation. Providers can use different protocols, but their events must have consistent user meanings. Starting, stopping, errors and interrupted responses must be visible without exposing raw protocol output as conversation text. All six providers, including Antigravity, are in scope; a native stream JSON adapter is valid structured delivery and does not need to speak ACP on the wire.

[Provider architecture](../architecture/provider-sessions.md).

[Runtime controls](runtime-lifecycle.md) define cancellation, stopping and recovery independently of display state.

## Input delivery

Each submitted user input follows the shared [admission and delivery agreement](../chat/sending.md), independently of later provider activity. Saved input identity and delivery facts remain historical data; they do not grant retry or execution permission. **Implementation note — Partial:** exact per-input confirmation is being completed; a local write observation does not establish remote delivery or provider acceptance, and confirmation across every supported provider and execution environment remains unverified.
