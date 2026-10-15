# Permission settings

Permission settings configure the default safety choice and any UAM reviewer used by [the four permission modes](../providers/permissions/permissions.md). Provider-native automatic review may replace the UAM review route only through an established mapping; failures and uncertainty remain visible user decisions. Configuration must distinguish the intended mode from the provider’s effective capabilities, keep Plan/read-only restrictions intact and eventually support the applicable provider/environment overrides.

**Implementation: Partial.** Native Claude auto and confirmed Codex auto-review routes coexist with UAM reviewer handling; broader environment scoping is outstanding. See [current architecture](../architecture/permissions-notifications.md) and [scope](environment-scope.md).
