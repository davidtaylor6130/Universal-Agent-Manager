# Provider settings

Provider pages configure CLI availability, launch preferences and supported provider-specific choices for the selected execution environment. Installed/running/version state is observed status, not a saved preference. Refresh and installation target the chosen computer and should not switch an existing conversation’s model merely because discovery finishes. Build-excluded providers must not appear as configurable choices.

On Windows, provider CLI checks and launches on the desktop and through the SSH helper also search user and machine PATH entries added since that process started, so a CLI, Node.js or PowerShell 7 installed while UAM is open is found without signing out. npm `.cmd` shims run through `cmd.exe`.

[Provider contracts](../providers/providers.md) · [Environment scope](environment-scope.md) · [Build flags](../build-flags.md).

[Overview and runtime availability](provider-overview.md).
