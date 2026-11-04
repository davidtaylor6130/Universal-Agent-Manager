# Computer use

Computer use lets the selected provider observe and act on an explicitly granted local window or display through UAM’s packaged controller, or use an explicitly selected native provider controller. For UAM control, the user grants a target before observations are returned; subsequent actions stay within that grant, and Pause, Stop or disabling the feature revokes further work. Stop cannot retract input already delivered. Provider-native control has its own permission and stop model and must not inherit claims about UAM’s grants. SSH desktop control is outside the current implementation.

- [Provider routing](providers/computer-use.md)
- [Settings](settings/computer-use.md)
- [Engineering and platform reference](reference/computer-use.md)

**Implementation: Partial.** The packaged UAM route exists on macOS and Windows; provider-native Codex is an explicitly selected unprobed preview, while Auto uses UAM. Full-display grants are observation/pointer only; typing requires an exact window. Screenshots and accessibility data go to the selected provider/model. OS consent and cleanup failures must remain visible.

The UAM helper uses the effective application data root for policy, control/history and its macOS installed-helper cache. Independent storage writers retain a shared profile lease; a stopped folder transfer requires an exclusive writer freeze and refuses a busy helper or uncertain handoff. Even after the manager exits, an active/rolled-back root receipt grants no write admission without its matching final acknowledged bootstrap transaction. A durable root fence precedes staging, and an acknowledged armed-only cancellation refreshes authority for the current root while retaining historical/passive holds. This does not establish shutdown of legacy helpers, and live folder activation stays gated by the [storage agreement](settings/storage.md#application-data-folder).
