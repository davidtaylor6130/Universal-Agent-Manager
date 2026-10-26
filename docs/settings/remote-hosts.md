# Remote Hosts settings

Remote Hosts configures execution environments using existing SSH aliases and a compatible installed UAM helper. Saved installation/startup preferences are distinct from observed connection, version and startup registration state. Installation and updates must respect active provider work and show failed or uncertain activation honestly; selecting a host does not authorize silently replaying a prompt or changing a chat’s workspace.

[SSH execution](../remote-environments/ssh.md) · [Routing](../remote-environments/routing.md).

Host options separately configure start-at-login, supported CLI updates with elevated privileges and host-specific instructions (inline guidance or the legacy instruction file). These settings must not elevate ordinary chat execution. Preview the helper install location/command, retain active-work/version guards and distinguish removing the saved host from uninstalling remote helper files. Helper setup runs on its own native worker so other app requests keep completing; cancelling the setup request or closing UAM stops its SSH steps promptly. A rollback already under way, and the bounded helper handshake, still finish first; an interrupted install keeps its saved fence until setup is retried to reconcile it.
