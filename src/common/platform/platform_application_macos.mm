#include "common/platform/platform_application_macos.h"

#import <AppKit/AppKit.h>

#include "include/cef_application_mac.h"

@interface UamApplication : NSApplication <CefAppProtocol>
{
	BOOL handlingSendEvent_;
}
@end

@implementation UamApplication

- (BOOL)isHandlingSendEvent
{
	return handlingSendEvent_;
}

- (void)setHandlingSendEvent:(BOOL)handlingSendEvent
{
	handlingSendEvent_ = handlingSendEvent;
}

- (void)sendEvent:(NSEvent*)event
{
	CefScopedSendingEvent sendingEvent;
	[super sendEvent:event];
}

@end

namespace uam::platform
{
	bool InitializeMacApplication()
	{
		[UamApplication sharedApplication];
		return [NSApp isKindOfClass:[UamApplication class]];
	}
}
