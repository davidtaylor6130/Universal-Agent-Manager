# Editors

Editors defines the application presets used to open workspaces and files, the default editor and file-type groups that override it. The built-in presets are VS Code (the initial default), Xcode, Visual Studio, CLion, Rider, WebStorm, PyCharm, IntelliJ IDEA, GoLand and RustRover. File-type groups initially map common language families to appropriate presets and can be edited; an unavailable installed application must produce a clear failure. These settings drive the composer’s [workspace actions](../chat/composer/workspace-actions.md) and file-opening choices rather than a second editor list. Unsupported remote actions remain explicit.

[Default presets and groups](../../src/common/config/editor_file_associations.h) · [Workspace actions](../chat/composer/workspace-actions.md).

File explorer selection can use the system default or a configured custom application path; saving that preference does not validate every remote opening route. File-type group naming, extensions, preset choice and explicit deletion keep their own drafts and preserve at least the supported valid configuration.

On Windows, PowerShell chooses which PowerShell hosts the terminal windows UAM opens (SSH workspace terminals and interactive update terminals): Automatic (default) uses built-in Windows PowerShell 5.1 (`powershell.exe`) and falls back to PowerShell 7 (`pwsh.exe`) only when 5.1 is absent; Windows PowerShell 5.1 and PowerShell 7 force that executable. A forced choice that is not installed fails the launch with a message naming the missing executable instead of substituting the other one. UAM never assumes `pwsh`; its generated PowerShell scripts target 5.1 syntax. The row is hidden on other platforms. [Selection rule](../../src/common/platform/powershell_selection.h).
