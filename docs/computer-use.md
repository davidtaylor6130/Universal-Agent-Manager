# Computer use

Computer use lets the selected provider observe and act on an explicitly granted local window or display through UAM’s packaged controller, or use an explicitly selected native provider controller. For UAM control, the user grants a target before observations are returned; subsequent actions stay within that grant, and Pause, Stop or disabling the feature revokes further work. Stop cannot retract input already delivered. Provider-native control has its own permission and stop model and must not inherit claims about UAM’s grants. SSH desktop control is outside the current implementation.

- [Provider routing](providers/computer-use.md)
- [Settings](settings/computer-use.md)
- [Engineering and platform reference](reference/computer-use.md)

**Implementation: Partial.** The packaged UAM route exists on macOS and Windows; provider-native Codex is an explicitly selected unprobed preview, while Auto uses UAM. Full-display grants are observation/pointer only; typing requires an exact window. Screenshots and accessibility data go to the selected provider/model. OS consent and cleanup failures must remain visible.
