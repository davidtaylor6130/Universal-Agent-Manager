# Execution environments

An execution environment is the computer on which provider and worker processes run, not a different provider or a copy of the UAM product. The local controller owns its chat catalogue and user interface; a remote helper manages remote processes and host-local files. Resource delivery, model discovery, permissions and settings must use the intended environment and report unsupported routes explicitly.

- [SSH execution](ssh.md)
- [Routing](routing.md)
- [Settings scope](../settings/environment-scope.md)
- [Remote Hosts](../settings/remote-hosts.md)

An opt-in [headless management node](../management-node.md) runs the same native state, provider runtimes and authenticated companion API through the CEF event loop without a desktop BrowserView. Native discovery publishes untrusted address hints only; authentication and explicit enrollment remain required. The separate [processing-node deployment](../../deploy/processing-node/README.md) packages an SSH worker, rather than another management UI. These restored routes require validation against the consolidated source.
