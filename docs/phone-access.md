# Phone access

Phone access connects a browser or the [native iPhone client](phone-native.md) to the main UAM device through its shared authenticated companion interface. Users explicitly turn publication on or off and connect through the configured route; the main device remains responsible for chats, providers and execution. Both clients use the permitted companion interface rather than acquiring the desktop’s unrestricted native bridge. Disabling publication stops access, and a desktop build without phone support contains no serving entry points. Phone access does not require a UAM account or cloud service.

[Phone settings](settings/phone-access.md) · [Network boundaries](security-enterprise.md) · [Build flags](build-flags.md).

**Implementation: Partial.** The browser companion exists and its full physical-device acceptance remains pending; the native client and shared API additions are integrated into V5, with scoped critical GUI reviews recorded and physical installation/launch confirmed by David; broader physical-device acceptance and the failing full native gate remain pending.

[Phone browser sections and connection state](phone-browser.md).
