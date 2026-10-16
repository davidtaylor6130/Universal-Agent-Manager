# Computer Use settings

Computer Use settings control whether the feature is available and which supported backend a chat uses. Auto currently selects UAM; explicit provider-native selection is a distinct route with its own controls and limitations. Turning the feature off revokes UAM grants and attempts to stop its owned work, reporting incomplete cleanup instead of showing a false success. This setting is unavailable in a build that excludes computer use, and SSH desktop control is not currently supported.

[Computer-use agreement](../computer-use.md) · [Environment scope](environment-scope.md).

Readiness combines observed OS access/helper state with saved enablement; an allowlist limits which applications may be offered, and refreshing target applications does not grant control. Restrict-to-listed-apps policy is distinct from the per-session target grant. [OS permissions](permissions.md) remain separate from native provider approvals.
