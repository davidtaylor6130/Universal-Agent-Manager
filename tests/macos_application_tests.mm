#include "common/platform/platform_application_macos.h"
#include "cef/uam_cef_security.h"

#import <AppKit/AppKit.h>

#include "include/cef_application_mac.h"
#include <cstdio>
#include <string_view>

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

int main(int argc, char* argv[])
{
	@autoreleasepool
	{
		const bool terminate_probe = argc == 2 && std::string_view(argv[1]) == "--terminate-probe";
		if (!terminate_probe)
		{
			// AppKit's default terminate exits 0 before C++ teardown, so require a receipt too.
			NSTask* task = [[NSTask alloc] init];
			task.launchPath = [NSString stringWithUTF8String:argv[0]];
			task.arguments = @[@"--terminate-probe"];
			NSPipe* output = [NSPipe pipe];
			task.standardOutput = output;
			[task launch];
			NSData* data = [[output fileHandleForReading] readDataToEndOfFile];
			[task waitUntilExit];
			NSString* receipt = [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
			const bool passed = task.terminationStatus == 0 && [receipt isEqualToString:@"quit-close-returned\n"];
			[task release];
			if (!passed) return 13;
		}
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
		// Quit without a window must leave C++/CEF teardown in control.
		if (terminate_probe) [application terminate:nil];

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
		if (terminate_probe)
		{
			// This probe refuses to actually close, like CEF while TryCloseBrowser is pending.
			[application terminate:nil];
			if (!window.closeRequested) return 14;
			window.closeRequested = NO;
			[application terminate:nil];
		}
		else [application sendEvent:quit];
		const BOOL close_requested = window.closeRequested;
		[window orderOut:nil];
		[window release];
		if (terminate_probe && close_requested) std::puts("quit-close-returned");
		return close_requested ? 0 : 4;
	}
}
