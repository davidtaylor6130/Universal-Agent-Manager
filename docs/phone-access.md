# Phone access

Phone access is a browser interface published by the main UAM device, not a separate installed phone app or a UAM cloud service. Users explicitly turn publication on or off and connect through the configured authenticated route; the main device remains responsible for chats and execution. The browser must use the permitted companion interface rather than acquiring the desktop’s unrestricted native bridge. Disabling publication stops access, and a build without phone support contains no phone entry points.

[Phone settings](settings/phone-access.md) · [Network boundaries](security-enterprise.md) · [Build flags](build-flags.md).

**Implementation: Needs verification** for full browser/device acceptance; the companion server and frontend exist. This documentation pass did not expose a server or change saved settings.

[Phone browser sections and connection state](phone-browser.md).
