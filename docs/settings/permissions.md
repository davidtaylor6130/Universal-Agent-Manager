# App Permissions settings

The Permissions page reports operating-system access used by UAM: microphone, speech recognition, local network, screen recording and accessibility where the build/platform supports them. Refresh observes current status; Request invokes supported OS consent, and System settings opens the relevant OS controls. Requested, allowed, denied, restricted, system-managed, unsupported and failed checks are different outcomes; opening a prompt does not establish that access was granted. These permissions belong to the local application or helper, not a provider/environment override. Build-excluded features leave no corresponding permission controls.

Chat approval modes and reviewer defaults are configured separately under [permission defaults](permission-defaults.md). [Voice](voice-input.md) · [Computer Use](computer-use.md) · [Phone](phone-access.md) · [Source](../../UI-V2/src/components/settings/PermissionsSettings.tsx).
