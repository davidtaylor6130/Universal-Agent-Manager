#include "common/platform/platform_application_macos.h"

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <Speech/Speech.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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
	MacPrivacyPermissions GetMacPrivacyPermissions()
	{
		MacPrivacyPermissions result;
		switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio])
		{
		case AVAuthorizationStatusAuthorized: result.microphone = "allowed"; break;
		case AVAuthorizationStatusDenied: result.microphone = "denied"; break;
		case AVAuthorizationStatusRestricted: result.microphone = "restricted"; break;
		default: result.microphone = "not_requested"; break;
		}
		switch ([SFSpeechRecognizer authorizationStatus])
		{
		case SFSpeechRecognizerAuthorizationStatusAuthorized: result.speech_recognition = "allowed"; break;
		case SFSpeechRecognizerAuthorizationStatusDenied: result.speech_recognition = "denied"; break;
		case SFSpeechRecognizerAuthorizationStatusRestricted: result.speech_recognition = "restricted"; break;
		default: result.speech_recognition = "not_requested"; break;
		}
		return result;
	}

	bool RequestMacPrivacyPermission(const std::string& permission)
	{
		if (permission == "localNetwork") RequestMacLocalNetworkAccess();
		else if (permission == "microphone")
			[AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL) {}];
		else if (permission == "speechRecognition")
			[SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus) {}];
		else return false;
		return true;
	}

	std::vector<std::string> MacLocalIpv4Addresses()
	{
		std::vector<std::string> addresses;
		ifaddrs* pInterfaces = nullptr;
		if (getifaddrs(&pInterfaces) != 0) return addresses;
		for (const ifaddrs* pInterface = pInterfaces; pInterface != nullptr; pInterface = pInterface->ifa_next)
		{
			if (pInterface->ifa_addr == nullptr || pInterface->ifa_addr->sa_family != AF_INET ||
			    (pInterface->ifa_flags & IFF_UP) == 0 || (pInterface->ifa_flags & IFF_LOOPBACK) != 0 ||
			    (pInterface->ifa_flags & IFF_BROADCAST) == 0) continue;
			const sockaddr_in* pAddress = reinterpret_cast<const sockaddr_in*>(pInterface->ifa_addr);
			char text[INET_ADDRSTRLEN]{};
			if (inet_ntop(AF_INET, &pAddress->sin_addr, text, sizeof(text)) != nullptr)
				addresses.emplace_back(text);
		}
		freeifaddrs(pInterfaces);
		return addresses;
	}

	void RequestMacLocalNetworkAccess()
	{
		// Apple TN3179: connecting UDP to a link-local address requests access
		// without transmitting a packet. Listening for TCP alone does not prompt.
		ifaddrs* pInterfaces = nullptr;
		if (getifaddrs(&pInterfaces) != 0) return;
		for (const ifaddrs* pInterface = pInterfaces; pInterface != nullptr; pInterface = pInterface->ifa_next)
		{
			if (pInterface->ifa_addr == nullptr || pInterface->ifa_addr->sa_family != AF_INET6 ||
			    (pInterface->ifa_flags & IFF_UP) == 0 || (pInterface->ifa_flags & IFF_LOOPBACK) != 0 ||
			    (pInterface->ifa_flags & IFF_BROADCAST) == 0) continue;
			sockaddr_in6 address = *reinterpret_cast<const sockaddr_in6*>(pInterface->ifa_addr);
			if (!IN6_IS_ADDR_LINKLOCAL(&address.sin6_addr)) continue;
			address.sin6_port = htons(9);
			address.sin6_scope_id = if_nametoindex(pInterface->ifa_name);
			const int iSocket = socket(AF_INET6, SOCK_DGRAM, 0);
			if (iSocket < 0) continue;
			(void)connect(iSocket, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
			close(iSocket);
		}
		freeifaddrs(pInterfaces);
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
			NSURL* application_url = bundle_id == nil ? nil : ([bundle_id isAbsolutePath] ? [NSURL fileURLWithPath:bundle_id] : [workspace URLForApplicationWithBundleIdentifier:bundle_id]);
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
