# Editors

Editors defines the application presets used to open workspaces and files, the default editor and file-type groups that override it. The built-in presets are VS Code (the initial default), Xcode, Visual Studio, CLion, Rider, WebStorm, PyCharm, IntelliJ IDEA, GoLand and RustRover. File-type groups initially map common language families to appropriate presets and can be edited; an unavailable installed application must produce a clear failure. These settings drive the composer’s [workspace actions](../chat/composer/workspace-actions.md) and file-opening choices rather than a second editor list. Unsupported remote actions remain explicit.

[Default presets and groups](../../src/common/config/editor_file_associations.h) · [Workspace actions](../chat/composer/workspace-actions.md).

File explorer selection can use the system default or a configured custom application path; saving that preference does not validate every remote opening route. File-type group naming, extensions, preset choice and explicit deletion keep their own drafts and preserve at least the supported valid configuration.
