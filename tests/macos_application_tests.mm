#include "common/platform/platform_application_macos.h"

#import <AppKit/AppKit.h>

#include "include/cef_application_mac.h"

int main()
{
	@autoreleasepool
	{
		if (!uam::platform::InitializeMacApplication()) return 1;

		NSApplication<CefAppProtocol>* application =
			static_cast<NSApplication<CefAppProtocol>*>(NSApp);
		[application setHandlingSendEvent:YES];
		if (![application isHandlingSendEvent]) return 2;
		[application setHandlingSendEvent:NO];
		return [application isHandlingSendEvent] ? 3 : 0;
	}
}
