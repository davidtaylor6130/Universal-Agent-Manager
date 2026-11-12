# Accept Edits

Accept Edits automatically approves workspace file edits while commands, broader access and other permission requests retain their normal approval boundaries. It is not approval for every tool or action. UAM must classify or map the provider’s edit mode without weakening Plan, read-only agents or managed restrictions.

UAM's own Accept Edits approves an edit only when every file the request names resolves inside the chat's workspace, using the same path rules as [captured reads](permissions.md#captured-read-restrictions). Each named file, including a rename's destination, is checked on its resolved path, so a link inside the workspace cannot reach a protected file. It never approves an edit that names no file, writes to UAM's data root, repository metadata (`.git`, `.gitmodules`, `.gitattributes`), agent or editor configuration that grants permissions, hooks or tasks (`.claude`, `.gemini`, `.codex`, `.opencode`, `.copilot`, `.vscode`, `.agents`, `.mcp.json`, `opencode.json`, `.envrc`) or a shell startup file such as `.zshrc` or a PowerShell profile, or also grants an extra writable root (Codex `grantRoot`); those go to the user. A provider's native edit mode keeps its own boundary.

[All permission modes](permissions.md).
