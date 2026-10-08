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
		const bool picker_probe = argc == 2 && std::string_view(argv[1]).find("--picker-") == 0;
		if (!terminate_probe && !picker_probe)
		{
			for (NSString* argument in @[@"--picker-folder-terminate", @"--picker-file-terminate", @"--picker-folder-keyboard", @"--picker-file-keyboard", @"--picker-folder-cancel", @"--picker-file-cancel"])
			{
				NSTask* task = [[NSTask alloc] init];
				task.launchPath = [NSString stringWithUTF8String:argv[0]];
				task.arguments = @[argument];
				NSPipe* output = [NSPipe pipe];
				task.standardOutput = output;
				[task launch];
				NSData* data = [[output fileHandleForReading] readDataToEndOfFile];
				[task waitUntilExit];
				NSString* receipt = [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
				const int status = task.terminationStatus;
				const bool passed = status == 0 && [receipt isEqualToString:@"picker-close-routing-returned\n"];
				[task release];
				if (!passed)
				{
					std::fprintf(stderr, "%s failed: exit %d\n", argument.UTF8String, status);
					return 15;
				}
				std::printf("[PASS] %s\n", argument.UTF8String);
			}
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
		if (picker_probe)
		{
			const bool directory = std::string_view(argv[1]).find("folder") != std::string_view::npos;
			const bool keyboard = std::string_view(argv[1]).find("keyboard") != std::string_view::npos;
			const bool cancel = std::string_view(argv[1]).find("cancel") != std::string_view::npos;
			__block BOOL saw_picker = NO;
			__block BOOL timed_out = NO;
			__block BOOL closed_inside_modal = NO;
			NSTimer* request = [NSTimer timerWithTimeInterval:0.1 repeats:NO block:^(NSTimer*)
			{
				saw_picker = [application.modalWindow isKindOfClass:[NSOpenPanel class]];
				if (cancel) [(NSOpenPanel*)application.modalWindow cancel:nil];
				else if (keyboard) [application postEvent:quit atStart:NO];
				else [application terminate:nil];
				closed_inside_modal = window.closeRequested;
			}];
			NSTimer* watchdog = [NSTimer timerWithTimeInterval:2.0 repeats:NO block:^(NSTimer*)
			{
				timed_out = YES;
				[(NSOpenPanel*)application.modalWindow cancel:nil];
			}];
			[[NSRunLoop currentRunLoop] addTimer:request forMode:NSModalPanelRunLoopMode];
			[[NSRunLoop currentRunLoop] addTimer:watchdog forMode:NSModalPanelRunLoopMode];
			std::string selected = "must-clear";
			std::string error;
			const bool accepted = uam::platform::BrowsePath(directory, std::filesystem::temp_directory_path(), &selected, &error);
			[request invalidate];
			[watchdog invalidate];
			[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
			const BOOL expected_close = cancel ? NO : YES;
			if (!saw_picker || timed_out || closed_inside_modal || accepted || !selected.empty() || !error.empty() || window.closeRequested != expected_close) return 16;
			std::puts("picker-close-routing-returned");
		}
		else if (terminate_probe)
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
		if (picker_probe) return 0;
		return close_requested ? 0 : 4;
	}
}
