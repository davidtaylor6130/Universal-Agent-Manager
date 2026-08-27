#include "common/platform/platform_application_macos.h"

#import <AppKit/AppKit.h>

#include "include/cef_application_mac.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <libproc.h>
#include <signal.h>
#include <sys/event.h>
#include <unistd.h>

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
	const NSEventModifierFlags modifiers =
		[event modifierFlags] & NSEventModifierFlagDeviceIndependentFlagsMask;
	if ([event type] == NSEventTypeKeyDown && modifiers == NSEventModifierFlagCommand &&
	    [[event charactersIgnoringModifiers] caseInsensitiveCompare:@"q"] == NSOrderedSame)
	{
		NSWindow* window = [self keyWindow];
		if (window == nil) window = [self mainWindow];
		if (window == nil) window = [[self windows] firstObject];
		if (window != nil) [window performClose:self];
		return;
	}
	CefScopedSendingEvent sendingEvent;
	[super sendEvent:event];
}

@end

namespace uam::platform
{
	namespace
	{
		bool ParseWatchdogInteger(const char* text, long long& value)
		{
			if (text == nullptr || *text == '\0') return false;
			char* end = nullptr;
			errno = 0;
			value = std::strtoll(text, &end, 10);
			return errno == 0 && end != text && *end == '\0' && value >= 0;
		}
	}

	std::optional<int> RunMacParentDeathWatchdogIfRequested(const int argc, char* argv[])
	{
		if (argc < 2 || std::strcmp(argv[1], kMacParentDeathWatchdogArgument) != 0) return std::nullopt;
		long long parent_value = 0;
		long long child_value = 0;
		long long start_seconds = 0;
		long long start_microseconds = 0;
		if (argc != 6 || !ParseWatchdogInteger(argv[2], parent_value) || !ParseWatchdogInteger(argv[3], child_value) ||
		    !ParseWatchdogInteger(argv[4], start_seconds) || !ParseWatchdogInteger(argv[5], start_microseconds) ||
		    parent_value <= 0 || child_value <= 0)
		{
			return 2;
		}

		const pid_t parent_pid = static_cast<pid_t>(parent_value);
		const pid_t child_pid = static_cast<pid_t>(child_value);
		const int queue = kqueue();
		struct kevent change
		{
		};
		EV_SET(&change, child_pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
		const bool armed = queue >= 0 && kevent(queue, &change, 1, nullptr, 0, nullptr) == 0;
		const char ready = armed ? '1' : '0';
		(void)write(3, &ready, 1);
		(void)close(3);
		if (!armed) return 1;

		for (;;)
		{
			struct kevent event
			{
			};
			const struct timespec timeout = {0, 100 * 1000 * 1000};
			const int event_count = kevent(queue, nullptr, 0, &event, 1, &timeout);
			if (event_count > 0) return 0;
			if (event_count < 0 && errno != EINTR)
			{
				(void)kill(-child_pid, SIGKILL);
				(void)kill(child_pid, SIGKILL);
				return 1;
			}
			if (getppid() != parent_pid)
			{
				struct proc_bsdinfo current_child_info
				{
				};
				const bool same_process = proc_pidinfo(child_pid, PROC_PIDTBSDINFO, 0, &current_child_info, sizeof(current_child_info)) == sizeof(current_child_info) &&
				                          current_child_info.pbi_start_tvsec == start_seconds &&
				                          current_child_info.pbi_start_tvusec == start_microseconds;
				if (same_process)
				{
					(void)kill(-child_pid, SIGKILL);
					(void)kill(child_pid, SIGKILL);
				}
				return 0;
			}
		}
	}

	bool InitializeMacApplication()
	{
		[UamApplication sharedApplication];
		return [NSApp isKindOfClass:[UamApplication class]];
	}

	bool BrowsePath(const bool choose_directory, const std::filesystem::path& initial_path, std::string* selected_path_out, std::string* error_out)
	{
		@autoreleasepool
		{
			if (selected_path_out != nullptr) selected_path_out->clear();
			NSOpenPanel* panel = [NSOpenPanel openPanel];
			panel.canChooseDirectories = choose_directory;
			panel.canChooseFiles = !choose_directory;
			panel.canCreateDirectories = YES;
			panel.allowsMultipleSelection = NO;
			if (!initial_path.empty())
			{
				NSString* initial = [NSString stringWithUTF8String:initial_path.c_str()];
				if (initial != nil) panel.directoryURL = [NSURL fileURLWithPath:initial];
			}
			if ([panel runModal] != NSModalResponseOK) return false;
			const char* selected = panel.URL.fileSystemRepresentation;
			if (selected == nullptr || *selected == '\0')
			{
				if (error_out != nullptr) *error_out = "macOS returned an empty selected path.";
				return false;
			}
			if (selected_path_out != nullptr) *selected_path_out = selected;
			return true;
		}
	}

	bool OpenExternalUrl(const std::string& url, std::string* error_out)
	{
		@autoreleasepool
		{
			NSString* value = [[[NSString alloc] initWithBytes:url.data() length:url.size() encoding:NSUTF8StringEncoding] autorelease];
			NSURL* external_url = value == nil ? nil : [NSURL URLWithString:value];
			if (external_url != nil && [[NSWorkspace sharedWorkspace] openURL:external_url])
			{
				return true;
			}
			if (error_out != nullptr) *error_out = "macOS could not open the external URL.";
			return false;
		}
	}

	bool OpenPath(const std::filesystem::path& path, std::string* error_out)
	{
		@autoreleasepool
		{
			NSString* value = [NSString stringWithUTF8String:path.c_str()];
			NSURL* file_url = value == nil ? nil : [NSURL fileURLWithPath:value];
			if (file_url != nil && [[NSWorkspace sharedWorkspace] openURL:file_url]) return true;
			if (error_out != nullptr) *error_out = "macOS could not open the path.";
			return false;
		}
	}

	bool OpenPathWithApplication(const std::filesystem::path& path, const std::string& application_bundle_id, std::string* error_out)
	{
		@autoreleasepool
		{
			NSString* file = [NSString stringWithUTF8String:path.c_str()];
			NSString* bundle_id = [NSString stringWithUTF8String:application_bundle_id.c_str()];
			NSWorkspace* workspace = [NSWorkspace sharedWorkspace];
			NSURL* application_url = bundle_id == nil ? nil : [workspace URLForApplicationWithBundleIdentifier:bundle_id];
			if (file == nil || application_url == nil)
			{
				if (error_out != nullptr) *error_out = "macOS could not find the requested application.";
				return false;
			}
			[workspace openURLs:@[[NSURL fileURLWithPath:file]]
			   withApplicationAtURL:application_url
			          configuration:[NSWorkspaceOpenConfiguration configuration]
			      completionHandler:nil];
			return true;
		}
	}

	bool RevealPath(const std::filesystem::path& path, std::string* error_out)
	{
		@autoreleasepool
		{
			NSString* value = [NSString stringWithUTF8String:path.c_str()];
			if (value == nil)
			{
				if (error_out != nullptr) *error_out = "The path is not valid UTF-8.";
				return false;
			}
			[[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:value]]];
			return true;
		}
	}
}
