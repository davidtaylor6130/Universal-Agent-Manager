# React and CEF

React renders the application inside CEF and invokes native services through a privileged, validated bridge. Native state is authoritative for process, permission and persistence outcomes; frontend projections must not invent delivery or completion. Large histories are retrieved in bounded pages and asynchronous responses must still belong to the selected chat and current request. The phone interface has a separate, restricted transport.

Desktop window controls and draggable regions are restricted to the trusted bundled main frame; they are unavailable through the phone transport. CEF retains native window close handling and owns minimize, maximize and fullscreen state.

[Chat](../chat/chat.md) · [Phone access](../phone-access.md) · [Security](../security-enterprise.md).

[Engineering reference](../reference/architecture/react-cef-transport.md).
