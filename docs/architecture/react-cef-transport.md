# React and CEF

React renders the application inside CEF and invokes native services through a privileged, validated bridge. Native state is authoritative for process, permission and persistence outcomes; frontend projections must not invent delivery or completion. Large histories are retrieved in bounded pages and asynchronous responses must still belong to the selected chat and current request. The phone interface has a separate, restricted transport.

Desktop window controls and draggable regions are restricted to the trusted bundled main frame; they are unavailable through the phone transport. CEF retains native window close handling and owns minimize, maximize and fullscreen state.

Application data-folder queries and choice/cancellation are likewise desktop-only. The native bridge supplies override provenance, revision and publication state; the frontend cannot choose the current root or submit an activation receipt. A saved pending preference is distinct from completed migration or restart readiness. Native restart-to-apply and rollback routes are also trusted-desktop-only and remain validation-gated; the frontend cannot supply a stopped-owner acknowledgement or infer completed migration from a restart reply. See [Storage](../settings/storage.md#application-data-folder).

[Chat](../chat/chat.md) · [Phone access](../phone-access.md) · [Security](../security-enterprise.md).

[Engineering reference](../reference/architecture/react-cef-transport.md).
