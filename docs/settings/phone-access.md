# Phone Access settings

Phone Access turns publication of the controller’s browser interface on or off and configures its connection and authentication options. Show the actual serving/connection state independently of the saved enablement setting; turning it off must stop access rather than just hiding the address. No phone application install is required. Build-excluded phone support leaves no phone settings or published interface.

[Phone access](../phone-access.md) · [Build flags](../build-flags.md).

Copy URL and Copy token expose distinct connection values; neither implies the browser is connected. A changed network address must report the current published route and preserve authentication rather than silently claiming the old URL still works.
