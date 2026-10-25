#include "common/platform/platform_application_macos.h"
#include "cef/uam_cef_security.h"

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include "include/cef_application_mac.h"
#include <cstdio>
#include <chrono>
#include <string_view>

static IMP s_originalPanelCancel = nullptr;
static bool s_ignoredInitialCancel = false;

/// <summary>Simulates a panel which has not yet accepted its first cancellation request.</summary>
static void IgnoreInitialPanelCancel(id panel, SEL selector, id sender)
{
	if (!s_ignoredInitialCancel)
	{
		s_ignoredInitialCancel = true;
		return;
	}
	reinterpret_cast<void (*)(id, SEL, id)>(s_originalPanelCancel)(panel, selector, sender);
}

@interface UamCloseProbeWindow : NSWindow
@property(nonatomic, assign) BOOL closeRequested;
@property(nonatomic, assign) NSUInteger closeRequestCount;
@end

@implementation UamCloseProbeWindow
- (void)performClose:(id)sender
{
	(void)sender;
	self.closeRequested = YES;
	++self.closeRequestCount;
}
@end

static NSEvent* QuitEvent(NSWindow* window, NSEventModifierFlags modifiers)
{
	return [NSEvent keyEventWithType:NSEventTypeKeyDown
	                     location:NSZeroPoint
	                modifierFlags:modifiers
	                    timestamp:0
	                 windowNumber:window.windowNumber
	                      context:nil
	                   characters:@"q"
	  charactersIgnoringModifiers:@"q"
	                    isARepeat:NO
	                      keyCode:12];
}

// A host that cannot present NSOpenPanel (for example an Intel CI VM) is reported, not failed.
constexpr int kPanelUnavailable = 77;

int main(int argc, char* argv[])
{
	@autoreleasepool
	{
		const bool terminate_probe = argc == 2 && std::string_view(argv[1]) == "--terminate-probe";
		const bool picker_probe = argc == 2 && std::string_view(argv[1]).find("--picker-") == 0;
		if (!terminate_probe && !picker_probe)
		{
			for (NSString* argument in @[@"--picker-folder-terminate", @"--picker-file-terminate", @"--picker-folder-delayed-terminate", @"--picker-file-delayed-terminate", @"--picker-folder-keyboard", @"--picker-file-keyboard", @"--picker-folder-caps-keyboard", @"--picker-file-caps-keyboard", @"--picker-folder-repeat-terminate", @"--picker-file-repeat-terminate", @"--picker-folder-repeat-keyboard", @"--picker-file-repeat-keyboard", @"--picker-folder-cancel", @"--picker-file-cancel"])
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
				if (status == kPanelUnavailable)
				{
					// The host cannot present NSOpenPanel at all; remaining picker probes would each wait it out.
					std::printf("SKIP: macOS open panel unavailable on this host (%s)\n", argument.UTF8String);
					[task release];
					break;
				}
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
		const bool caps = argc == 2 && std::string_view(argv[1]).find("caps") != std::string_view::npos;
		NSEvent* quit = QuitEvent(window, NSEventModifierFlagCommand | (caps ? NSEventModifierFlagCapsLock : 0));
		if (picker_probe)
		{
			const bool directory = std::string_view(argv[1]).find("folder") != std::string_view::npos;
			const bool keyboard = std::string_view(argv[1]).find("keyboard") != std::string_view::npos;
			const bool cancel = std::string_view(argv[1]).find("cancel") != std::string_view::npos;
			const bool repeat = std::string_view(argv[1]).find("repeat") != std::string_view::npos;
			const bool delayed_cancel = std::string_view(argv[1]).find("delayed") != std::string_view::npos;
			__block BOOL saw_picker = NO;
			__block BOOL request_fired = NO;
			__block BOOL timed_out = NO;
			__block BOOL closed_inside_modal = NO;
			__block NSTimer* watchdog = nil;
			NSTimer* request = [NSTimer timerWithTimeInterval:0.1 repeats:NO block:^(NSTimer*)
			{
				request_fired = YES;
				// Time the 2 s close budget from the request; a loaded host can first service the panel loop after 2 s.
				watchdog = [NSTimer timerWithTimeInterval:2.0 repeats:NO block:^(NSTimer*)
				{
					timed_out = YES;
					[(NSOpenPanel*)application.modalWindow cancel:nil];
				}];
				[[NSRunLoop currentRunLoop] addTimer:watchdog forMode:NSModalPanelRunLoopMode];
				saw_picker = [application.modalWindow isKindOfClass:[NSOpenPanel class]];
				if (delayed_cancel && saw_picker)
				{
					const Class panel_class = object_getClass(application.modalWindow);
					const Method cancel_method = class_getInstanceMethod(panel_class, @selector(cancel:));
					if (cancel_method != nullptr)
					{
						s_originalPanelCancel = method_getImplementation(cancel_method);
						class_replaceMethod(panel_class, @selector(cancel:), reinterpret_cast<IMP>(IgnoreInitialPanelCancel), method_getTypeEncoding(cancel_method));
					}
				}
				if (cancel) [(NSOpenPanel*)application.modalWindow cancel:nil];
				else if (keyboard) [application postEvent:quit atStart:NO];
				else [application terminate:nil];
				if (repeat && !keyboard)
				{
					[application terminate:nil];
					[application terminate:nil];
				}
				if (repeat && keyboard)
				{
					[application postEvent:quit atStart:NO];
					[application postEvent:quit atStart:NO];
				}
				closed_inside_modal = window.closeRequested;
			}];
			[[NSRunLoop currentRunLoop] addTimer:request forMode:NSModalPanelRunLoopMode];
			std::string selected = "must-clear";
			std::string error;
			const auto browse_started = std::chrono::steady_clock::now();
			const bool accepted = uam::platform::BrowsePath(directory, std::filesystem::temp_directory_path(), &selected, &error);
			[request invalidate];
			if (!request_fired && !accepted)
			{
				// runModal returned without ever running its modal loop: the panel service never presented it.
				const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - browse_started).count();
				std::fprintf(stderr, "macOS open panel never became modal after %lld ms\n", static_cast<long long>(waited));
				return kPanelUnavailable;
			}
			[watchdog invalidate];
			if (delayed_cancel && !s_ignoredInitialCancel) return 21;
			if (repeat)
			{
				// A second Quit can arrive in the query callback before default-mode close.
				[application terminate:nil];
				closed_inside_modal = closed_inside_modal || window.closeRequested;
			}
			[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
			const BOOL expected_close = cancel ? NO : YES;
			if (!saw_picker || timed_out || closed_inside_modal || accepted || !selected.empty() || !error.empty() || window.closeRequested != expected_close)
			{
				std::fprintf(stderr, "picker=%d timeout=%d inside=%d accepted=%d close=%d count=%lu\n", saw_picker, timed_out, closed_inside_modal, accepted, window.closeRequested, static_cast<unsigned long>(window.closeRequestCount));
				return 16;
			}
			if (repeat)
			{
				if (window.closeRequestCount != 1) return 19;
				// Like CEF while TryCloseBrowser is pending, this window refuses to close.
				[application terminate:nil];
				[application terminate:nil];
				if (window.closeRequestCount != 3) return 20;
			}
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
		else
		{
			[application sendEvent:QuitEvent(window, NSEventModifierFlagCommand | NSEventModifierFlagCapsLock)];
			if (!window.closeRequested) return 17;
			window.closeRequested = NO;
			for (NSEventModifierFlags extra : {NSEventModifierFlagShift, NSEventModifierFlagOption, NSEventModifierFlagControl})
			{
				[application sendEvent:QuitEvent(window, NSEventModifierFlagCommand | NSEventModifierFlagCapsLock | extra)];
				if (window.closeRequested) return 18;
			}
			[application sendEvent:quit];
		}
		const BOOL close_requested = window.closeRequested;
		[window orderOut:nil];
		[window release];
		if (terminate_probe && close_requested) std::puts("quit-close-returned");
		if (picker_probe) return 0;
		return close_requested ? 0 : 4;
	}
}
