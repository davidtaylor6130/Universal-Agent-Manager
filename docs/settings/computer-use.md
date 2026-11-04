# Computer Use settings

Computer Use settings control whether the feature is available and which supported backend a chat uses. Auto currently selects UAM; explicit provider-native selection is a distinct route with its own controls and limitations. Turning the feature off revokes UAM grants and attempts to stop its owned work, reporting incomplete cleanup instead of showing a false success. This setting is unavailable in a build that excludes computer use, and SSH desktop control is not currently supported.

The global status-row switch appears on Computer Use and the all-environments MCP Built in section, saves immediately and waits for native confirmation. Requests remain serialized when navigating between these surfaces, and older responses cannot replace newer native status observations. On allows target requests; each chat still needs its exact approved target. Saved Off with cleanup pending stays visibly pending, blocks re-enable and offers Retry disabling through the same global action. A failed change re-reads native state because Off may already have persisted; an unconfirmed outcome offers Check status and still allows Turn off, without implying cleared grants or offering enablement. These controls neither grant nor revoke local OS permissions and do not select a target or change provider-native permission guarantees.

[Computer-use agreement](../computer-use.md) · [Environment scope](environment-scope.md).

Readiness combines observed OS access/helper state with saved enablement; an allowlist limits which applications may be offered, and refreshing target applications does not grant control. Restrict-to-listed-apps policy is distinct from the per-session target grant. [OS permissions](permissions.md) remain separate from native provider approvals.

Cleanup can also remain pending after a chat is turned off while global Computer Use stays On. Both settings surfaces and the provider overview distinguish that state and explain that turning Computer Use off for all chats retries cleanup, without claiming the chat always offers a retry. A newer native status observation supersedes obsolete failure feedback; navigation alone retains it. If a state change makes the focused switch or recovery action unavailable, keyboard focus moves to an available control in the same row unless the user has moved elsewhere.

The provider overview's read-only availability indicator also retains unknown or cleanup-pending global status and directs recovery to Computer Use settings instead of suggesting re-enable during pending revocation.
