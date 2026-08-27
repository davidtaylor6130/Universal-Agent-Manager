#include "common/platform/platform_application_macos.h"
#include "cef/uam_cef_security.h"

#import <AppKit/AppKit.h>

#include "include/cef_application_mac.h"

@interface UamCloseProbeWindow : NSWindow
@property(nonatomic, assign) BOOL closeRequested;
@end

@implementation UamCloseProbeWindow
- (void)performClose:(id)sender
{
	(void)sender;
	self.closeRequested = YES;
}
@end

int main()
{
	@autoreleasepool
	{
		constexpr std::string_view policy = uam::cef::kContentSecurityPolicy;
		if (policy.find("https://api.github.com") == std::string_view::npos) return 10;
		if (policy.find("https://registry.npmjs.org") == std::string_view::npos) return 11;
		if (policy.find("https://formulae.brew.sh") == std::string_view::npos) return 12;

		if (!uam::platform::InitializeMacApplication()) return 1;

		NSApplication<CefAppProtocol>* application =
			static_cast<NSApplication<CefAppProtocol>*>(NSApp);
		[application setHandlingSendEvent:YES];
		if (![application isHandlingSendEvent]) return 2;
		[application setHandlingSendEvent:NO];
		if ([application isHandlingSendEvent]) return 3;

		UamCloseProbeWindow* window = [[UamCloseProbeWindow alloc]
			initWithContentRect:NSMakeRect(0, 0, 320, 200)
			          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
			            backing:NSBackingStoreBuffered
			              defer:NO];
		[window makeKeyAndOrderFront:nil];
		NSEvent* quit = [NSEvent keyEventWithType:NSEventTypeKeyDown
			                         location:NSZeroPoint
			                    modifierFlags:NSEventModifierFlagCommand
			                        timestamp:0
			                     windowNumber:window.windowNumber
			                          context:nil
			                       characters:@"q"
			      charactersIgnoringModifiers:@"q"
			                        isARepeat:NO
			                          keyCode:12];
		[application sendEvent:quit];
		const BOOL close_requested = window.closeRequested;
		[window orderOut:nil];
		[window release];
		return close_requested ? 0 : 4;
	}
}
