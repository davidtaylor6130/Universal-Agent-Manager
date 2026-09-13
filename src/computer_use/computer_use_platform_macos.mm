#include "computer_use/computer_use_platform.h"

#include "common/utils/string_utils.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <ImageIO/ImageIO.h>
#include <dispatch/dispatch.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

constexpr CGFloat kUamCursorImageFrameX = 21.0;
constexpr CGFloat kUamCursorImageFrameY = 18.5;
constexpr CGFloat kUamCursorImageRenderSize = 18.0;
constexpr CGFloat kUamCursorSourceSize = 950.0;
constexpr CGFloat kUamCursorSourceHotspotX = 129.0;
constexpr CGFloat kUamCursorSourceHotspotY = 73.0;
constexpr CGFloat kUamCursorHotspotX = kUamCursorImageFrameX + kUamCursorImageRenderSize * kUamCursorSourceHotspotX / kUamCursorSourceSize;
constexpr CGFloat kUamCursorHotspotY = kUamCursorImageFrameY + kUamCursorImageRenderSize * (1.0 - kUamCursorSourceHotspotY / kUamCursorSourceSize);

@interface UAMComputerUseCursorView : NSView
{
	NSImageView* image_view_;
	NSTextField* identity_label_;
	NSView* accent_view_;
}
- (void)animateClick;
- (void)setIdentityLabel:(NSString*)label accentRGB:(uint32_t)rgb;
@end

@implementation UAMComputerUseCursorView
- (instancetype)initWithFrame:(NSRect)frame
{
	self = [super initWithFrame:frame];
	if (self == nil)
		return nil;

	self.wantsLayer = YES;
	self.layer.backgroundColor = NSColor.clearColor.CGColor;
	image_view_ = [[NSImageView alloc] initWithFrame:NSMakeRect(kUamCursorImageFrameX, kUamCursorImageFrameY, kUamCursorImageRenderSize, kUamCursorImageRenderSize)];
	NSString* cursor_path = [NSBundle.mainBundle pathForResource:@"SoftwareCursor" ofType:@"png"];
	image_view_.image = cursor_path == nil ? nil : [[NSImage alloc] initWithContentsOfFile:cursor_path];
	image_view_.imageScaling = NSImageScaleProportionallyUpOrDown;
	[self addSubview:image_view_];
	accent_view_ = [[NSView alloc] initWithFrame:NSMakeRect(44, 18, 3, 20)];
	accent_view_.wantsLayer = YES;
	[self addSubview:accent_view_];
	identity_label_ = [[NSTextField alloc] initWithFrame:NSMakeRect(52, 16, 180, 24)];
	identity_label_.bezeled = NO;
	identity_label_.drawsBackground = NO;
	identity_label_.editable = NO;
	identity_label_.selectable = NO;
	identity_label_.font = [NSFont systemFontOfSize:14 weight:NSFontWeightBold];
	identity_label_.lineBreakMode = NSLineBreakByTruncatingTail;
	[self addSubview:identity_label_];

	return self;
}

- (void)setIdentityLabel:(NSString*)label accentRGB:(uint32_t)rgb
{
	identity_label_.stringValue = label.length == 0 ? @"Chat" : label;
	const CGFloat red = ((rgb >> 16) & 0xff) / 255.0;
	const CGFloat green = ((rgb >> 8) & 0xff) / 255.0;
	const CGFloat blue = (rgb & 0xff) / 255.0;
	NSColor* color = [NSColor colorWithCalibratedRed:red green:green blue:blue alpha:1.0];
	identity_label_.textColor = color;
	[image_view_.image setTemplate:YES];
	image_view_.contentTintColor = color;
	NSMutableParagraphStyle* paragraph = [[NSMutableParagraphStyle alloc] init];
	paragraph.lineBreakMode = NSLineBreakByTruncatingTail;
	identity_label_.attributedStringValue = [[NSAttributedString alloc] initWithString:identity_label_.stringValue attributes:@{NSForegroundColorAttributeName: color, NSFontAttributeName: identity_label_.font, NSUnderlineStyleAttributeName: @(NSUnderlineStyleSingle), NSParagraphStyleAttributeName: paragraph}];
	accent_view_.layer.backgroundColor = color.CGColor;
}

- (BOOL)isOpaque
{
	return NO;
}

- (void)animateClick
{
	CAKeyframeAnimation* press = [CAKeyframeAnimation animationWithKeyPath:@"opacity"];
	press.values = @[ @1, @0.62, @1 ];
	press.keyTimes = @[ @0, @0.45, @1 ];
	press.duration = 0.18;
	press.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
	[self.layer addAnimation:press forKey:@"press"];
}
@end

namespace uam::computer_use
{
	namespace
	{
		int controller_lock_fd = -1;
		std::string virtual_cursor_label = "Chat";
		std::uint32_t virtual_cursor_accent_rgb = 0x14b8a6;

		std::uint32_t CursorAccentForChat(std::string_view chat_id)
		{
			constexpr std::uint32_t colors[] = {0xf97316, 0x0ea5e9, 0x7c3aed, 0x14b8a6, 0x22c55e, 0xe11d48};
			std::uint32_t hash = 2166136261u;
			for (const unsigned char value : chat_id)
				hash = (hash ^ value) * 16777619u;
			return colors[hash % (sizeof(colors) / sizeof(colors[0]))];
		}

		void ConfigureVirtualCursorIdentityImpl(const std::string& label, const std::string& chat_id)
		{
			virtual_cursor_label = label.empty() ? "Chat" : label;
			virtual_cursor_accent_rgb = CursorAccentForChat(chat_id);
		}

		ApplicationIdentity ApplicationForPid(pid_t pid)
		{
			@autoreleasepool
			{
				NSRunningApplication* application = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
				if (application == nil)
					return {};
				NSString* bundle = application.bundleIdentifier ?: @"";
				NSString* title = application.localizedName ?: application.executableURL.lastPathComponent ?: @"Application";
				return {
				    "pid:" + std::to_string(pid) + "|" + std::string(bundle.UTF8String ?: ""),
				    std::string(title.UTF8String ?: "Application"),
				};
			}
		}

		std::string Utf8(CFStringRef value)
		{
			if (value == nullptr)
				return {};
			const CFIndex length = CFStringGetLength(value);
			const CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
			std::string result(static_cast<std::size_t>(capacity), '\0');
			if (!CFStringGetCString(value, result.data(), capacity, kCFStringEncodingUTF8))
				return {};
			result.resize(std::char_traits<char>::length(result.c_str()));
			return result;
		}

		bool Number(CFDictionaryRef dictionary, CFStringRef key, std::int64_t* value_out)
		{
			const auto value = static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, key));
			return value != nullptr && CFGetTypeID(value) == CFNumberGetTypeID() && CFNumberGetValue(value, kCFNumberSInt64Type, value_out);
		}

		bool WindowBounds(CGWindowID window_id, CGRect* bounds_out, pid_t* pid_out = nullptr, std::string* title_out = nullptr)
		{
			const CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, window_id);
			if (windows == nullptr || CFArrayGetCount(windows) == 0)
			{
				if (windows != nullptr)
					CFRelease(windows);
				return false;
			}
			const auto dictionary = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, 0));
			const auto bounds = static_cast<CFDictionaryRef>(CFDictionaryGetValue(dictionary, kCGWindowBounds));
			const bool ok = bounds != nullptr && CGRectMakeWithDictionaryRepresentation(bounds, bounds_out);
			if (pid_out != nullptr)
			{
				std::int64_t pid = 0;
				*pid_out = Number(dictionary, kCGWindowOwnerPID, &pid) ? static_cast<pid_t>(pid) : 0;
			}
			if (title_out != nullptr)
			{
				const std::string owner = Utf8(static_cast<CFStringRef>(CFDictionaryGetValue(dictionary, kCGWindowOwnerName)));
				const std::string name = Utf8(static_cast<CFStringRef>(CFDictionaryGetValue(dictionary, kCGWindowName)));
				*title_out = name.empty() ? owner : owner + " — " + name;
			}
			CFRelease(windows);
			return ok;
		}

		CGImageRef ScaledImage(CGImageRef source, int max_width, int max_height)
		{
			const std::size_t source_width = CGImageGetWidth(source);
			const std::size_t source_height = CGImageGetHeight(source);
			if (source_width == 0 || source_height == 0)
				return nullptr;
			const double scale = std::min({1.0, static_cast<double>(max_width) / source_width, static_cast<double>(max_height) / source_height});
			const std::size_t width = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(source_width * scale)));
			const std::size_t height = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(source_height * scale)));
			if (width == source_width && height == source_height)
				return CGImageRetain(source);

			CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
			const CGBitmapInfo bitmap_info = static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Big);
			CGContextRef context = CGBitmapContextCreate(nullptr, width, height, 8, width * 4, color_space, bitmap_info);
			CGColorSpaceRelease(color_space);
			if (context == nullptr)
				return nullptr;
			CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
			CGContextDrawImage(context, CGRectMake(0, 0, width, height), source);
			CGImageRef result = CGBitmapContextCreateImage(context);
			CGContextRelease(context);
			return result;
		}

		std::string Png(CGImageRef image)
		{
			CFMutableDataRef data = CFDataCreateMutable(kCFAllocatorDefault, 0);
			if (data == nullptr)
				return {};
			CGImageDestinationRef destination = CGImageDestinationCreateWithData(data, CFSTR("public.png"), 1, nullptr);
			if (destination == nullptr)
			{
				CFRelease(data);
				return {};
			}
			CGImageDestinationAddImage(destination, image, nullptr);
			const bool ok = CGImageDestinationFinalize(destination);
			CFRelease(destination);
			std::string result;
			if (ok)
			{
				result.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(data)), static_cast<std::size_t>(CFDataGetLength(data)));
			}
			CFRelease(data);
			return result;
		}

		CGPoint DesktopPoint(const Action& action, const Capture& reference)
		{
			const double x = reference.desktop_x + action.x * reference.desktop_width / std::max(1, reference.width);
			const double y = reference.desktop_y + action.y * reference.desktop_height / std::max(1, reference.height);
			return CGPointMake(x, y);
		}

		NSPanel* cursor_panel = nil;
		UAMComputerUseCursorView* cursor_view = nil;

		void EnsureCursorPanel()
		{
			if (cursor_panel != nil)
				return;
			[NSApplication sharedApplication];
			[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
			[NSApp finishLaunching];
			cursor_panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 240, 56) styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel backing:NSBackingStoreBuffered defer:NO];
			cursor_panel.opaque = NO;
			cursor_panel.backgroundColor = [NSColor clearColor];
			cursor_panel.hasShadow = NO;
			cursor_panel.hidesOnDeactivate = NO;
			cursor_panel.ignoresMouseEvents = YES;
			cursor_panel.level = CGWindowLevelForKey(kCGCursorWindowLevelKey) - 1;
			cursor_panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorFullScreenAuxiliary;
			cursor_view = [[UAMComputerUseCursorView alloc] initWithFrame:NSMakeRect(0, 0, 240, 56)];
			[cursor_view setIdentityLabel:[NSString stringWithUTF8String:virtual_cursor_label.c_str()] accentRGB:virtual_cursor_accent_rgb];
			cursor_panel.contentView = cursor_view;
		}

		void ShowCursorPanel()
		{
			[cursor_panel orderFrontRegardless];
			[cursor_panel displayIfNeeded];
		}

		void ShowVirtualCursor(const Action& action, const Capture& reference)
		{
			if (action.kind != "move" && action.kind != "click" && action.kind != "drag" && action.kind != "scroll")
				return;
			CGPoint point = DesktopPoint(action, reference);
			if (action.kind == "drag")
			{
				Action end = action;
				end.x = action.end_x;
				end.y = action.end_y;
				point = DesktopPoint(end, reference);
			}
			dispatch_semaphore_t movement_complete = dispatch_semaphore_create(0);
			void (^show_cursor)() = ^{
			EnsureCursorPanel();
			const CGRect primary = CGDisplayBounds(CGMainDisplayID());
			const NSPoint origin = NSMakePoint(point.x - kUamCursorHotspotX, primary.size.height - point.y - kUamCursorHotspotY);
			NSPoint previous = cursor_panel.frame.origin;
			if (!cursor_panel.visible)
			{
				const CGPoint center = CGPointMake(reference.desktop_x + reference.desktop_width / 2, reference.desktop_y + reference.desktop_height / 2);
				previous = NSMakePoint(center.x - kUamCursorHotspotX, primary.size.height - center.y - kUamCursorHotspotY);
				[cursor_panel setFrameOrigin:previous];
			}
			ShowCursorPanel();
			const CGFloat distance = std::hypot(origin.x - previous.x, origin.y - previous.y);
			const double duration = std::clamp(static_cast<double>(distance) / 1200, 0.18, 0.45);
			const int steps = std::max(1, static_cast<int>(std::ceil(duration * 60)));
			for (int index = 1; index <= steps; ++index)
			{
				const int step = index;
				dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<std::int64_t>(duration * NSEC_PER_SEC * step / steps)), dispatch_get_main_queue(), ^{
					const CGFloat t = static_cast<CGFloat>(step) / steps;
					const CGFloat eased = t * t * (3 - 2 * t);
					[cursor_panel setFrameOrigin:NSMakePoint(previous.x + (origin.x - previous.x) * eased, previous.y + (origin.y - previous.y) * eased)];
					if (step == steps)
					{
						if (action.kind == "click")
							[cursor_view animateClick];
						dispatch_semaphore_signal(movement_complete);
					}
				  });
			  }
			};
			if (NSThread.isMainThread)
				show_cursor();
			else
			{
				dispatch_sync(dispatch_get_main_queue(), show_cursor);
				(void)dispatch_semaphore_wait(movement_complete, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC));
			}
		}

		bool PostEvent(CGEventRef event, const Capture& reference, bool* input_applied_out = nullptr)
		{
			const CGEventType type = CGEventGetType(event);
			const bool is_mouse_event = type == kCGEventMouseMoved || type == kCGEventLeftMouseDown || type == kCGEventLeftMouseUp || type == kCGEventLeftMouseDragged || type == kCGEventRightMouseDown || type == kCGEventRightMouseUp || type == kCGEventRightMouseDragged || type == kCGEventOtherMouseDown || type == kCGEventOtherMouseUp || type == kCGEventOtherMouseDragged || type == kCGEventScrollWheel;
			if (reference.target_kind == "window" && reference.process_id != 0 && is_mouse_event)
			{
				using SetWindowLocation = void (*)(CGEventRef, CGPoint);
				static const SetWindowLocation set_window_location = reinterpret_cast<SetWindowLocation>(dlsym(RTLD_DEFAULT, "CGEventSetWindowLocation"));
				NSEvent* original = [NSEvent eventWithCGEvent:event];
				if (original == nil)
					return false;
				const NSEventType appkit_type = type == kCGEventScrollWheel ? NSEventTypeMouseMoved : original.type;
				const CGPoint global_location = CGEventGetLocation(event);
				// Keep desktop and top-left window coordinates as separate payloads after AppKit conversion.
				const NSPoint local_location = NSMakePoint(global_location.x - reference.desktop_x, global_location.y - reference.desktop_y);
				const NSEvent* appkit_event = [NSEvent mouseEventWithType:appkit_type
												location:local_location
												modifierFlags:original.modifierFlags
												timestamp:static_cast<NSTimeInterval>(CGEventGetTimestamp(event)) / 1000000000.0
												windowNumber:static_cast<NSInteger>(reference.target_id)
												context:nil
												eventNumber:static_cast<NSInteger>(CGEventGetIntegerValueField(event, kCGMouseEventNumber))
												clickCount:static_cast<NSInteger>(CGEventGetIntegerValueField(event, kCGMouseEventClickState))
												pressure:static_cast<float>(CGEventGetDoubleValueField(event, kCGMouseEventPressure))];
				if (appkit_event == nil || appkit_event.CGEvent == nullptr)
					return false;
				CGEventRef converted = appkit_event.CGEvent;
				if (set_window_location == nullptr)
					return false;
				CGEventSetLocation(converted, global_location);
				set_window_location(converted, CGPointMake(local_location.x, local_location.y));
				CGEventSetIntegerValueField(converted, kCGMouseEventWindowUnderMousePointer, static_cast<int64_t>(reference.target_id));
				CGEventSetIntegerValueField(converted, kCGMouseEventWindowUnderMousePointerThatCanHandleThisEvent, static_cast<int64_t>(reference.target_id));
				CGEventSetIntegerValueField(converted, kCGMouseEventButtonNumber, CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber));
				if (type == kCGEventScrollWheel)
				{
					CGEventSetType(converted, kCGEventScrollWheel);
					for (const CGEventField field : {kCGScrollWheelEventDeltaAxis1, kCGScrollWheelEventDeltaAxis2, kCGScrollWheelEventDeltaAxis3, kCGScrollWheelEventPointDeltaAxis1, kCGScrollWheelEventPointDeltaAxis2, kCGScrollWheelEventPointDeltaAxis3, kCGScrollWheelEventScrollPhase, kCGScrollWheelEventScrollCount, kCGScrollWheelEventMomentumPhase, kCGScrollWheelEventIsContinuous})
						CGEventSetIntegerValueField(converted, field, CGEventGetIntegerValueField(event, field));
					for (const CGEventField field : {kCGScrollWheelEventFixedPtDeltaAxis1, kCGScrollWheelEventFixedPtDeltaAxis2, kCGScrollWheelEventFixedPtDeltaAxis3})
						CGEventSetDoubleValueField(converted, field, CGEventGetDoubleValueField(event, field));
				}
				CGEventPostToPid(static_cast<pid_t>(reference.process_id), converted);
			}
			else if (reference.target_kind == "screen" && is_mouse_event)
			{
				CGEventPost(kCGHIDEventTap, event);
			}
			else if (reference.process_id != 0)
			{
				CGEventPostToPid(static_cast<pid_t>(reference.process_id), event);
			}
			else
				CGEventPost(kCGHIDEventTap, event);
			if (input_applied_out != nullptr)
				*input_applied_out = true;
			return true;
		}

		bool PostKey(CGKeyCode code, bool down, CGEventFlags flags, const Capture& reference, bool* input_applied_out)
		{
			CGEventRef event = CGEventCreateKeyboardEvent(nullptr, code, down);
			if (event == nullptr)
				return false;
			CGEventSetFlags(event, flags);
			const bool posted = PostEvent(event, reference, input_applied_out);
			CFRelease(event);
			return posted;
		}

		const std::unordered_map<std::string, CGKeyCode>& KeyCodes()
		{
			static const std::unordered_map<std::string, CGKeyCode> codes = {
			    {"a", kVK_ANSI_A}, {"b", kVK_ANSI_B}, {"c", kVK_ANSI_C}, {"d", kVK_ANSI_D}, {"e", kVK_ANSI_E}, {"f", kVK_ANSI_F}, {"g", kVK_ANSI_G}, {"h", kVK_ANSI_H}, {"i", kVK_ANSI_I}, {"j", kVK_ANSI_J}, {"k", kVK_ANSI_K}, {"l", kVK_ANSI_L}, {"m", kVK_ANSI_M}, {"n", kVK_ANSI_N}, {"o", kVK_ANSI_O}, {"p", kVK_ANSI_P}, {"q", kVK_ANSI_Q}, {"r", kVK_ANSI_R}, {"s", kVK_ANSI_S}, {"t", kVK_ANSI_T}, {"u", kVK_ANSI_U}, {"v", kVK_ANSI_V}, {"w", kVK_ANSI_W}, {"x", kVK_ANSI_X}, {"y", kVK_ANSI_Y}, {"z", kVK_ANSI_Z}, {"0", kVK_ANSI_0}, {"1", kVK_ANSI_1}, {"2", kVK_ANSI_2}, {"3", kVK_ANSI_3}, {"4", kVK_ANSI_4}, {"5", kVK_ANSI_5}, {"6", kVK_ANSI_6}, {"7", kVK_ANSI_7}, {"8", kVK_ANSI_8}, {"9", kVK_ANSI_9}, {"enter", kVK_Return}, {"return", kVK_Return}, {"tab", kVK_Tab}, {"space", kVK_Space}, {"escape", kVK_Escape}, {"esc", kVK_Escape}, {"backspace", kVK_Delete}, {"delete", kVK_ForwardDelete}, {"left", kVK_LeftArrow}, {"right", kVK_RightArrow}, {"up", kVK_UpArrow}, {"down", kVK_DownArrow}, {"home", kVK_Home}, {"end", kVK_End}, {"pageup", kVK_PageUp}, {"pagedown", kVK_PageDown}, {"cmd", kVK_Command}, {"command", kVK_Command}, {"meta", kVK_Command}, {"ctrl", kVK_Control}, {"control", kVK_Control}, {"alt", kVK_Option}, {"option", kVK_Option}, {"shift", kVK_Shift}, {"fn", kVK_Function},
			};
			return codes;
		}

		CGEventFlags ModifierFlag(std::string_view key)
		{
			if (key == "cmd" || key == "command" || key == "meta")
				return kCGEventFlagMaskCommand;
			if (key == "ctrl" || key == "control")
				return kCGEventFlagMaskControl;
			if (key == "alt" || key == "option")
				return kCGEventFlagMaskAlternate;
			if (key == "shift")
				return kCGEventFlagMaskShift;
			if (key == "fn")
				return kCGEventFlagMaskSecondaryFn;
			return 0;
		}

		std::string Lower(std::string value)
		{
			std::ranges::transform(value, value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			return value;
		}

		std::string AxString(AXUIElementRef element, CFStringRef attribute)
		{
			CFTypeRef value = nullptr;
			if (AXUIElementCopyAttributeValue(element, attribute, &value) != kAXErrorSuccess || value == nullptr)
				return {};
			std::string result;
			if (CFGetTypeID(value) == CFStringGetTypeID())
			{
				result = Utf8(static_cast<CFStringRef>(value));
			}
			CFRelease(value);
			return uam::strings::SafeLine(result, 200, true);
		}

		bool AxBounds(AXUIElementRef element, CGRect* bounds_out)
		{
			CFTypeRef position_value = nullptr;
			CFTypeRef size_value = nullptr;
			CGPoint position{};
			CGSize size{};
			const bool ok = AXUIElementCopyAttributeValue(element, kAXPositionAttribute, &position_value) == kAXErrorSuccess && AXUIElementCopyAttributeValue(element, kAXSizeAttribute, &size_value) == kAXErrorSuccess && position_value != nullptr && size_value != nullptr && CFGetTypeID(position_value) == AXValueGetTypeID() && CFGetTypeID(size_value) == AXValueGetTypeID() && AXValueGetValue(static_cast<AXValueRef>(position_value), static_cast<AXValueType>(kAXValueCGPointType), &position) && AXValueGetValue(static_cast<AXValueRef>(size_value), static_cast<AXValueType>(kAXValueCGSizeType), &size);
			if (position_value != nullptr)
				CFRelease(position_value);
			if (size_value != nullptr)
				CFRelease(size_value);
			if (ok)
				*bounds_out = CGRectMake(position.x, position.y, size.width, size.height);
			return ok;
		}

		constexpr double kAxWindowMatchTolerance = 24;

		double AxWindowBoundsDifference(const CGRect& candidate, const CGRect& reference)
		{
			return std::max({
			    std::abs(candidate.origin.x - reference.origin.x),
			    std::abs(candidate.origin.y - reference.origin.y),
			    std::abs(candidate.size.width - reference.size.width),
			    std::abs(candidate.size.height - reference.size.height),
			});
		}

		AXUIElementRef MatchingAxWindow(AXUIElementRef application, const CGRect& reference_bounds)
		{
			if (application == nullptr)
				return nullptr;
			// Some applications initialize their accessibility tree when a reader requests the app role.
			(void)AxString(application, kAXRoleAttribute);
			CFTypeRef windows_value = nullptr;
			if (AXUIElementCopyAttributeValue(application, kAXWindowsAttribute, &windows_value) != kAXErrorSuccess || windows_value == nullptr || CFGetTypeID(windows_value) != CFArrayGetTypeID())
			{
				if (windows_value != nullptr)
					CFRelease(windows_value);
				return nullptr;
			}
			AXUIElementRef match = nullptr;
			bool ambiguous = false;
			const CFArrayRef windows = static_cast<CFArrayRef>(windows_value);
			for (CFIndex index = 0; index < CFArrayGetCount(windows); ++index)
			{
				AXUIElementRef candidate = static_cast<AXUIElementRef>(CFArrayGetValueAtIndex(windows, index));
				CGRect candidate_bounds{};
				if (!AxBounds(candidate, &candidate_bounds))
					continue;
				const double difference = AxWindowBoundsDifference(candidate_bounds, reference_bounds);
				if (difference <= kAxWindowMatchTolerance)
				{
					if (match != nullptr)
					{
						ambiguous = true;
						break;
					}
					match = candidate;
				}
			}
			AXUIElementRef result = match != nullptr && !ambiguous ? static_cast<AXUIElementRef>(CFRetain(match)) : nullptr;
			CFRelease(windows_value);
			return result;
		}

		struct AxWindowTarget
		{
			AXUIElementRef application = nullptr;
			AXUIElementRef window = nullptr;

			AxWindowTarget() = default;
			AxWindowTarget(const AxWindowTarget&) = delete;
			AxWindowTarget& operator=(const AxWindowTarget&) = delete;

			~AxWindowTarget()
			{
				if (window != nullptr)
					CFRelease(window);
				if (application != nullptr)
					CFRelease(application);
			}
		};

		bool IsAxWindowFocused(const AxWindowTarget& target, const CGRect& reference_bounds)
		{
			if (target.application == nullptr || target.window == nullptr)
				return false;
			CFTypeRef focused_window_value = nullptr;
			const bool focused_window_available = AXUIElementCopyAttributeValue(target.application, kAXFocusedWindowAttribute, &focused_window_value) == kAXErrorSuccess && focused_window_value != nullptr && CFGetTypeID(focused_window_value) == AXUIElementGetTypeID();
			const bool exact_window_focused = focused_window_available && CFEqual(focused_window_value, target.window);
			CGRect focused_bounds{};
			const bool bounds_match = exact_window_focused && AxBounds(static_cast<AXUIElementRef>(focused_window_value), &focused_bounds) && AxWindowBoundsDifference(focused_bounds, reference_bounds) <= kAxWindowMatchTolerance;
			if (focused_window_value != nullptr)
				CFRelease(focused_window_value);
			return exact_window_focused && bounds_match;
		}

		bool IsAxWindowTargetCurrent(const AxWindowTarget& target, const Capture& reference)
		{
			if (target.window == nullptr)
				return false;
			const CGRect reference_bounds = CGRectMake(reference.desktop_x, reference.desktop_y, reference.desktop_width, reference.desktop_height);
			CGRect native_bounds{};
			pid_t current_pid = 0;
			// AX matching tolerates frame decoration differences; input must use unchanged native geometry.
			if (!WindowBounds(static_cast<CGWindowID>(reference.target_id), &native_bounds, &current_pid) || current_pid != static_cast<pid_t>(reference.process_id) || AxWindowBoundsDifference(native_bounds, reference_bounds) > 0.5)
				return false;
			CGRect current_bounds{};
			return AxBounds(target.window, &current_bounds) && AxWindowBoundsDifference(current_bounds, reference_bounds) <= kAxWindowMatchTolerance;
		}

		bool FocusAndVerifyAxWindow(const AxWindowTarget& target, const CGRect& reference_bounds)
		{
			if (target.application == nullptr || target.window == nullptr)
				return false;
			if (IsAxWindowFocused(target, reference_bounds))
				return true;
			const AXError main_result = AXUIElementSetAttributeValue(target.window, kAXMainAttribute, kCFBooleanTrue);
			const AXError focused_result = AXUIElementSetAttributeValue(target.window, kAXFocusedAttribute, kCFBooleanTrue);
			if (main_result != kAXErrorSuccess && focused_result != kAXErrorSuccess)
				return false;
			const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
			do
			{
				if (IsAxWindowFocused(target, reference_bounds))
					return true;
				if (std::chrono::steady_clock::now() >= deadline)
					break;
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			} while (true);
			return false;
		}

		bool PostAppKitDefined(pid_t process_id, CGWindowID window_id, short subtype)
		{
			NSEvent* event = [NSEvent otherEventWithType:NSEventTypeAppKitDefined
								location:NSZeroPoint
								modifierFlags:0xc0000
								timestamp:0
								windowNumber:static_cast<NSInteger>(window_id)
								context:nil
								subtype:subtype
								data1:0
								data2:0];
			if (event == nil || event.CGEvent == nullptr)
				return false;
			CGEventRef cg_event = event.CGEvent;
			if (cg_event == nullptr)
				return false;
			CGEventPostToPid(process_id, cg_event);
			return true;
		}

		// Establish native key/main focus before content input without raising the target app.
		class SyntheticPointerFocusScope
		{
		public:
			SyntheticPointerFocusScope() = default;
			SyntheticPointerFocusScope(const SyntheticPointerFocusScope&) = delete;
			SyntheticPointerFocusScope& operator=(const SyntheticPointerFocusScope&) = delete;

			~SyntheticPointerFocusScope()
			{
				if (!active_)
					return;
				const NSRunningApplication* frontmost = NSWorkspace.sharedWorkspace.frontmostApplication;
				if (frontmost == nil || frontmost.processIdentifier != process_id_)
					(void)PostAppKitDefined(process_id_, window_id_, 2);
			}

			bool Begin(const AxWindowTarget& target, const Capture& reference, const std::function<bool()>& cancelled, bool* input_applied_out, std::string* error_out)
			{
				const pid_t process_id = static_cast<pid_t>(reference.process_id);
				if (NSRunningApplication* frontmost = NSWorkspace.sharedWorkspace.frontmostApplication;
					frontmost != nil && frontmost.processIdentifier == process_id)
					return true;
				if (cancelled && cancelled())
					return Fail("Computer action interrupted before pointer focus.", error_out);
				if (!IsAxWindowTargetCurrent(target, reference))
					return Fail("The selected window changed before pointer focus.", error_out);

				const CGRect reference_bounds = CGRectMake(reference.desktop_x, reference.desktop_y, reference.desktop_width, reference.desktop_height);
				CGRect current_bounds{};
				if (!AxBounds(target.window, &current_bounds) || AxWindowBoundsDifference(current_bounds, reference_bounds) > kAxWindowMatchTolerance)
					return Fail("The selected window bounds are not stable enough for pointer focus.", error_out);
				if (AxString(target.window, kAXRoleAttribute) != "AXWindow")
					return Fail("The selected target is not a standard application window.", error_out);
				const std::string subrole = AxString(target.window, kAXSubroleAttribute);
				if (subrole != "AXStandardWindow" && subrole != "AXDialog")
					return Fail("The selected window has no verified titled frame for pointer focus.", error_out);
				CFTypeRef close_value = nullptr;
				const bool close_available = AXUIElementCopyAttributeValue(target.window, kAXCloseButtonAttribute, &close_value) == kAXErrorSuccess && close_value != nullptr && CFGetTypeID(close_value) == AXUIElementGetTypeID();
				CGRect close_bounds{};
				const bool close_valid = close_available && AxBounds(static_cast<AXUIElementRef>(close_value), &close_bounds) && CGRectContainsRect(reference_bounds, close_bounds) && CGRectContainsRect(current_bounds, close_bounds) && close_bounds.origin.x > reference_bounds.origin.x + 1 && close_bounds.origin.y > reference_bounds.origin.y + 1;
				if (close_value != nullptr)
					CFRelease(close_value);
				if (!close_valid)
					return Fail("The selected window frame could not be verified for safe pointer focus.", error_out);

				if (!PostAppKitDefined(process_id, static_cast<CGWindowID>(reference.target_id), 1))
					return Fail("The selected window could not be activated for pointer input.", error_out);
				process_id_ = process_id;
				window_id_ = static_cast<CGWindowID>(reference.target_id);
				active_ = true;
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				if (cancelled && cancelled())
					return Fail("Computer action interrupted before pointer focus input.", error_out);
				if (!IsAxWindowTargetCurrent(target, reference))
					return Fail("The selected window changed before pointer focus input.", error_out);

				const CGPoint corner = CGPointMake(reference.desktop_x + 1, reference.desktop_y + 1);
				if (!CGRectContainsPoint(reference_bounds, corner) || !CGRectContainsPoint(current_bounds, corner))
					return Fail("The selected window has no verified neutral frame corner for pointer focus.", error_out);
				CGEventRef down = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseDown, corner, kCGMouseButtonLeft);
				CGEventRef up = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseUp, corner, kCGMouseButtonLeft);
				if (down == nullptr || up == nullptr)
				{
					if (down != nullptr) CFRelease(down);
					if (up != nullptr) CFRelease(up);
					return Fail("Pointer focus events could not be created.", error_out);
				}
				const bool down_posted = PostEvent(down, reference, input_applied_out);
				const bool up_posted = !down_posted || PostEvent(up, reference);
				CFRelease(down);
				CFRelease(up);
				if (!down_posted || !up_posted)
					return Fail("Pointer focus events could not be posted safely.", error_out);
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				return true;
			}

		private:
			static bool Fail(const char* message, std::string* error_out)
			{
				if (error_out != nullptr)
					*error_out = message;
				return false;
			}

			pid_t process_id_ = 0;
			CGWindowID window_id_ = 0;
			bool active_ = false;
		};

		bool ResolveAxWindow(pid_t pid, const CGRect& reference_bounds, AxWindowTarget* target_out, std::string* error_out)
		{
			AXUIElementRef application = AXUIElementCreateApplication(pid);
			AXUIElementRef window = MatchingAxWindow(application, reference_bounds);
			if (application == nullptr || window == nullptr)
			{
				if (window != nullptr)
					CFRelease(window);
				if (application != nullptr)
					CFRelease(application);
				if (error_out != nullptr)
					*error_out = "The selected window could not be matched uniquely to an accessibility window for input.";
				return false;
			}
			target_out->application = application;
			target_out->window = window;
			return true;
		}

		bool FocusAxWindow(pid_t pid, const CGRect& reference_bounds, AxWindowTarget* target_out, std::string* error_out)
		{
			AxWindowTarget target;
			if (!ResolveAxWindow(pid, reference_bounds, &target, error_out))
				return false;
			if (FocusAndVerifyAxWindow(target, reference_bounds))
			{
				target_out->application = target.application;
				target_out->window = target.window;
				target.application = nullptr;
				target.window = nullptr;
				return true;
			}
			if (error_out != nullptr)
				*error_out = "The selected window could not be focused and verified for input.";
			return false;
		}

		std::optional<Element> DescribeAxElement(AXUIElementRef element, const CGRect& window_bounds)
		{
			const std::string role = AxString(element, kAXRoleAttribute);
			std::string label = AxString(element, kAXTitleAttribute);
			if (label.empty())
				label = AxString(element, kAXDescriptionAttribute);
			if (label.empty() && role == "AXStaticText")
				label = AxString(element, kAXValueAttribute);
			if (label.empty() && (role == "AXTextField" || role == "AXTextArea" || role == "AXComboBox"))
				label = AxString(element, CFSTR("AXPlaceholderValue"));
			CGRect bounds{};
			CFTypeRef enabled_value = nullptr;
			bool enabled = true;
			if (AXUIElementCopyAttributeValue(element, kAXEnabledAttribute, &enabled_value) == kAXErrorSuccess && enabled_value != nullptr && CFGetTypeID(enabled_value) == CFBooleanGetTypeID())
			{
				enabled = CFBooleanGetValue(static_cast<CFBooleanRef>(enabled_value));
			}
			if (enabled_value != nullptr)
				CFRelease(enabled_value);
			const bool useful_without_label = role == "AXButton" || role == "AXTextField" || role == "AXTextArea" || role == "AXCheckBox" || role == "AXRadioButton" || role == "AXPopUpButton" || role == "AXComboBox" || role == "AXLink" || role == "AXSlider" || role == "AXTab" || role == "AXMenuItem" || role == "AXDisclosureTriangle" || role == "AXStaticText";
			if (AxBounds(element, &bounds) && bounds.size.width > 0 && bounds.size.height > 0 && CGRectIntersectsRect(bounds, window_bounds) && (!label.empty() || useful_without_label))
				{
				const CGRect visible_bounds = CGRectIntersection(bounds, window_bounds);
				return Element{0, role.empty() ? "element" : role, label, visible_bounds.origin.x, visible_bounds.origin.y, visible_bounds.size.width, visible_bounds.size.height, enabled};
			}
			return std::nullopt;
		}

		void NormalizeElement(Element& element, const CGRect& window_bounds, int width, int height)
		{
			element.x = (element.x - window_bounds.origin.x) * width / window_bounds.size.width;
			element.y = (element.y - window_bounds.origin.y) * height / window_bounds.size.height;
			element.width *= width / window_bounds.size.width;
			element.height *= height / window_bounds.size.height;
		}

		constexpr int kAxTraversalNodeBudget = 4096;
		constexpr int kAxTraversalDepthBudget = 64;

		void AppendAxElements(AXUIElementRef element, const CGRect& window_bounds, std::vector<Element>& result, int depth, int& visited_nodes, bool& truncated)
		{
			if (visited_nodes >= kAxTraversalNodeBudget)
			{
				truncated = true;
				return;
			}
			++visited_nodes;
			if (depth > kAxTraversalDepthBudget || result.size() >= 200)
			{
				truncated = true;
				return;
			}
			if (std::optional<Element> described = DescribeAxElement(element, window_bounds))
			{
				described->id = static_cast<int>(result.size() + 1);
				result.push_back(std::move(*described));
			}

			CFTypeRef children_value = nullptr;
			if (AXUIElementCopyAttributeValue(element, kAXChildrenAttribute, &children_value) != kAXErrorSuccess || children_value == nullptr || CFGetTypeID(children_value) != CFArrayGetTypeID())
			{
				if (children_value != nullptr)
					CFRelease(children_value);
				return;
			}
			const CFArrayRef children = static_cast<CFArrayRef>(children_value);
			const CFIndex child_count = CFArrayGetCount(children);
			for (CFIndex index = 0; index < child_count; ++index)
			{
				if (result.size() >= 200 || visited_nodes >= kAxTraversalNodeBudget)
				{
					truncated = true;
					break;
				}
				AppendAxElements(static_cast<AXUIElementRef>(CFArrayGetValueAtIndex(children, index)), window_bounds, result, depth + 1, visited_nodes, truncated);
			}
			CFRelease(children_value);
		}

		AXUIElementRef FindAxElement(AXUIElementRef element, const CGRect& window_bounds, int target_id, int& current_id, int depth, int& visited_nodes)
		{
			if (visited_nodes >= kAxTraversalNodeBudget)
				return nullptr;
			++visited_nodes;
			if (depth > kAxTraversalDepthBudget || current_id >= 200)
				return nullptr;
			if (DescribeAxElement(element, window_bounds))
			{
				++current_id;
				if (current_id == target_id)
					return static_cast<AXUIElementRef>(CFRetain(element));
			}

			CFTypeRef children_value = nullptr;
			if (AXUIElementCopyAttributeValue(element, kAXChildrenAttribute, &children_value) != kAXErrorSuccess || children_value == nullptr || CFGetTypeID(children_value) != CFArrayGetTypeID())
			{
				if (children_value != nullptr)
					CFRelease(children_value);
				return nullptr;
			}
			AXUIElementRef found = nullptr;
			const CFArrayRef children = static_cast<CFArrayRef>(children_value);
			for (CFIndex index = 0; index < CFArrayGetCount(children) && found == nullptr && visited_nodes < kAxTraversalNodeBudget; ++index)
				found = FindAxElement(static_cast<AXUIElementRef>(CFArrayGetValueAtIndex(children, index)), window_bounds, target_id, current_id, depth + 1, visited_nodes);
			CFRelease(children_value);
			return found;
		}

		std::vector<Element> AccessibilityElements(pid_t pid, const CGRect& window_bounds, bool* truncated_out)
		{
			std::vector<Element> result;
			bool truncated = false;
			AXUIElementRef application = AXUIElementCreateApplication(pid);
			AXUIElementRef window = MatchingAxWindow(application, window_bounds);
			if (window != nullptr)
			{
				int visited_nodes = 0;
				AppendAxElements(window, window_bounds, result, 0, visited_nodes, truncated);
				CFRelease(window);
			}
			if (application != nullptr)
				CFRelease(application);
			if (truncated_out != nullptr)
				*truncated_out = truncated;
			return result;
		}

		AXUIElementRef AccessibilityElement(pid_t pid, const CGRect& window_bounds, int element_id)
		{
			AXUIElementRef application = AXUIElementCreateApplication(pid);
			int current_id = 0;
			int visited_nodes = 0;
			AXUIElementRef window = MatchingAxWindow(application, window_bounds);
			AXUIElementRef result = window == nullptr ? nullptr : FindAxElement(window, window_bounds, element_id, current_id, 0, visited_nodes);
			if (window != nullptr)
				CFRelease(window);
			if (application != nullptr)
				CFRelease(application);
			return result;
		}

		CGImageRef LegacyCapture(const std::string& kind, std::uint64_t id)
		{
			if (kind == "screen")
			{
				using Function = CGImageRef (*)(CGDirectDisplayID);
				const auto function = reinterpret_cast<Function>(dlsym(RTLD_DEFAULT, "CGDisplayCreateImage"));
				return function == nullptr ? nullptr : function(static_cast<CGDirectDisplayID>(id));
			}
			using Function = CGImageRef (*)(CGRect, CGWindowListOption, CGWindowID, CGWindowImageOption);
			const auto function = reinterpret_cast<Function>(dlsym(RTLD_DEFAULT, "CGWindowListCreateImage"));
			return function == nullptr ? nullptr : function(CGRectNull, kCGWindowListOptionIncludingWindow, static_cast<CGWindowID>(id), kCGWindowImageBoundsIgnoreFraming);
		}

		CGImageRef ModernCapture(const std::string& kind, std::uint64_t id, const CGRect& bounds, int max_width, int max_height)
		{
			if (@available(macOS 14.0, *))
			{
				__block CGImageRef result = nullptr;
				__block bool accepting_result = true;
				NSLock* result_lock = [[NSLock alloc] init];
				dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
				[SCShareableContent getShareableContentExcludingDesktopWindows:NO
				                                           onScreenWindowsOnly:YES
				                                             completionHandler:^(SCShareableContent* content, NSError* error) {
					                                           if (content == nil || error != nil)
					                                           {
						                                           dispatch_semaphore_signal(semaphore);
						                                           return;
					                                           }
					                                           SCContentFilter* filter = nil;
					                                           if (kind == "screen")
					                                           {
						                                           for (SCDisplay* display in content.displays)
						                                           {
							                                           if (display.displayID == static_cast<CGDirectDisplayID>(id))
							                                           {
								                                           filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
								                                           break;
							                                           }
						                                           }
					                                           }
					                                           else
					                                           {
						                                           for (SCWindow* window in content.windows)
						                                           {
							                                           if (window.windowID == static_cast<CGWindowID>(id))
							                                           {
								                                           filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:window];
								                                           break;
							                                           }
						                                           }
					                                           }
					                                           if (filter == nil)
					                                           {
						                                           dispatch_semaphore_signal(semaphore);
						                                           return;
					                                           }
					                                           const double scale = std::min({1.0, static_cast<double>(max_width) / bounds.size.width, static_cast<double>(max_height) / bounds.size.height});
					                                           SCStreamConfiguration* configuration = [[SCStreamConfiguration alloc] init];
					                                           configuration.width = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(bounds.size.width * scale)));
					                                           configuration.height = std::max<std::size_t>(1, static_cast<std::size_t>(std::floor(bounds.size.height * scale)));
					                                           configuration.showsCursor = YES;
					                                           configuration.ignoreShadowsSingleWindow = YES;
					                                           [SCScreenshotManager captureImageWithFilter:filter
					                                                                         configuration:configuration
					                                                                     completionHandler:^(CGImageRef image, NSError*) {
						                                                                   [result_lock lock];
						                                                                   if (accepting_result && image != nullptr && result == nullptr)
							                                                                   result = CGImageRetain(image);
						                                                                   [result_lock unlock];
						                                                                   dispatch_semaphore_signal(semaphore);
					                                                                     }];
				                                             }];
				(void)dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
				[result_lock lock];
				accepting_result = false;
				CGImageRef accepted_result = result;
				result = nullptr;
				[result_lock unlock];
				return accepted_result;
			}
			return nullptr;
		}
	} // namespace

	void ConfigureVirtualCursorIdentity(const std::string& label, const std::string& chat_id)
	{
		ConfigureVirtualCursorIdentityImpl(label, chat_id);
	}

	bool AcquireControllerLock(std::string* error_out)
	{
		if (controller_lock_fd >= 0)
		{
			if (error_out != nullptr)
				*error_out = "Another UAM computer-use action is in progress. Observe again, then retry.";
			return false;
		}
		const std::string lock_file = "/tmp/universal-agent-manager-computer-use-" + std::to_string(getuid()) + ".lock";
		const int descriptor = open(lock_file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (descriptor < 0)
		{
			if (error_out != nullptr)
				*error_out = "Computer-use MCP could not open its controller lock.";
			return false;
		}
		if (flock(descriptor, LOCK_EX | LOCK_NB) != 0)
		{
			close(descriptor);
			if (error_out != nullptr)
				*error_out = "Another UAM computer-use action is in progress. Observe again, then retry.";
			return false;
		}
		controller_lock_fd = descriptor;
		return true;
	}

	void ReleaseControllerLock()
	{
		if (controller_lock_fd < 0)
			return;
		close(controller_lock_fd);
		controller_lock_fd = -1;
	}

	std::vector<Target> ListTargets(std::string* error_out)
	{
		std::vector<Target> result;
		uint32_t display_count = 0;
		if (CGGetActiveDisplayList(0, nullptr, &display_count) == kCGErrorSuccess && display_count > 0)
		{
			std::vector<CGDirectDisplayID> displays(display_count);
			if (CGGetActiveDisplayList(display_count, displays.data(), &display_count) == kCGErrorSuccess)
			{
				for (uint32_t index = 0; index < display_count; ++index)
				{
					const CGRect bounds = CGDisplayBounds(displays[index]);
					result.push_back({"screen", displays[index], "Full display " + std::to_string(displays[index]), bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height, displays[index] == CGMainDisplayID(), "foreground", 0});
				}
			}
		}

		const CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
		if (windows != nullptr)
		{
			const std::int64_t current_process_id = static_cast<std::int64_t>(getpid());
			for (CFIndex index = 0; index < CFArrayGetCount(windows); ++index)
			{
				const auto dictionary = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(windows, index));
				std::int64_t id = 0;
				std::int64_t layer = 0;
				std::int64_t process_id = 0;
				CGRect bounds{};
				const auto bounds_value = static_cast<CFDictionaryRef>(CFDictionaryGetValue(dictionary, kCGWindowBounds));
				if (!Number(dictionary, kCGWindowNumber, &id) || !Number(dictionary, kCGWindowLayer, &layer) || !Number(dictionary, kCGWindowOwnerPID, &process_id) || process_id == current_process_id || layer != 0 || bounds_value == nullptr || !CGRectMakeWithDictionaryRepresentation(bounds_value, &bounds) || bounds.size.width < 2 || bounds.size.height < 2)
				{
					continue;
				}
				const std::string owner = Utf8(static_cast<CFStringRef>(CFDictionaryGetValue(dictionary, kCGWindowOwnerName)));
				const std::string name = Utf8(static_cast<CFStringRef>(CFDictionaryGetValue(dictionary, kCGWindowName)));
				result.push_back({"window", static_cast<std::uint64_t>(id), name.empty() ? owner : owner + " — " + name, bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height, false, "background", static_cast<std::uint64_t>(std::max<std::int64_t>(0, process_id))});
			}
			CFRelease(windows);
		}
		if (result.empty() && error_out != nullptr)
			*error_out = "No visible screens or windows were found.";
		return result;
	}

	Capture CaptureTarget(const std::string& kind, std::uint64_t raw_id, int max_width, int max_height)
	{
		Capture result;
		result.target_kind = kind;
		result.target_id = raw_id;
		CGImageRef source = nullptr;
		CGRect bounds{};
		if (kind == "screen")
		{
			result.input_mode = "foreground";
			const CGDirectDisplayID display = raw_id == 0 ? CGMainDisplayID() : static_cast<CGDirectDisplayID>(raw_id);
			result.target_id = display;
			bounds = CGDisplayBounds(display);
			if (@available(macOS 14.0, *))
				source = ModernCapture(kind, display, bounds, max_width, max_height);
			else
				source = LegacyCapture(kind, display);
		}
		else if (kind == "window")
		{
			const CGWindowID window = static_cast<CGWindowID>(raw_id);
			pid_t pid = 0;
			if (!WindowBounds(window, &bounds, &pid))
			{
				result.error = "The selected window is no longer available.";
				return result;
			}
			const ApplicationIdentity application = ApplicationForPid(pid);
			result.process_id = static_cast<std::uint64_t>(std::max<pid_t>(0, pid));
			result.application_id = application.id;
			result.application_title = application.title;
			result.input_mode = "background";
			if (@available(macOS 14.0, *))
				source = ModernCapture(kind, window, bounds, max_width, max_height);
			else
				source = LegacyCapture(kind, window);
		}
		if (source == nullptr && !CGPreflightScreenCaptureAccess())
		{
			result.error = "macOS denied Screen Recording. Enable UAM Computer Use in System Settings > Privacy & Security > Screen & System Audio Recording, then retry.";
			return result;
		}
		if (source == nullptr)
		{
			result.error = "The selected target could not be captured.";
			return result;
		}

		CGImageRef scaled = ScaledImage(source, max_width, max_height);
		CGImageRelease(source);
		if (scaled == nullptr)
		{
			result.error = "The screenshot could not be scaled.";
			return result;
		}
		result.width = static_cast<int>(CGImageGetWidth(scaled));
		result.height = static_cast<int>(CGImageGetHeight(scaled));
		result.png = Png(scaled);
		CGImageRelease(scaled);
		if (result.png.empty())
		{
			result.error = "The screenshot could not be encoded as PNG.";
			return result;
		}
		result.desktop_x = bounds.origin.x;
		result.desktop_y = bounds.origin.y;
		result.desktop_width = bounds.size.width;
			result.desktop_height = bounds.size.height;
			if (kind == "window")
			{
				result.elements = AccessibilityElements(static_cast<pid_t>(result.process_id), bounds, &result.elements_truncated);
				for (Element& element : result.elements)
					NormalizeElement(element, bounds, result.width, result.height);
		}
		result.ok = true;
		return result;
	}

	bool ExecuteAction(const Action& action, const Capture& reference, const std::function<bool()>& cancelled, std::string* error_out, bool* input_applied_out)
	{
		if (input_applied_out != nullptr)
			*input_applied_out = false;
		AxWindowTarget window_target;
		SyntheticPointerFocusScope pointer_focus;
		const CGRect window_bounds = CGRectMake(reference.desktop_x, reference.desktop_y, reference.desktop_width, reference.desktop_height);
		bool cancellation_seen = false;
		const auto interrupted = [&]()
		{
			if (!cancellation_seen && (!cancelled || !cancelled()))
				return false;
			cancellation_seen = true;
			if (error_out != nullptr)
				*error_out = "Computer action interrupted because UAM was paused or stopped.";
			return true;
		};
		if (interrupted())
			return false;

		if (action.kind != "wait" && !AXIsProcessTrusted())
		{
			if (error_out != nullptr)
				*error_out = "UAM Computer Use lost Accessibility permission. Grant it in System Settings > Privacy & Security > Accessibility, then retry.";
			return false;
		}

		ShowVirtualCursor(action, reference);
		if (interrupted())
			return false;
		const bool uses_input = action.kind == "move" || action.kind == "click" || action.kind == "drag" || action.kind == "scroll" || action.kind == "type" || action.kind == "hotkey";
		if (uses_input && reference.target_kind == "window")
		{
			const bool requires_keyboard_focus = action.kind == "type" || action.kind == "hotkey";
			const bool resolved = requires_keyboard_focus
				? FocusAxWindow(static_cast<pid_t>(reference.process_id), window_bounds, &window_target, error_out)
				: ResolveAxWindow(static_cast<pid_t>(reference.process_id), window_bounds, &window_target, error_out);
			if (!resolved)
				return false;
			if (interrupted())
				return false;
		}
		const auto keyboard_target_is_focused = [&]()
		{
			if (reference.target_kind != "window" || IsAxWindowFocused(window_target, window_bounds))
				return true;
			if (error_out != nullptr)
				*error_out = "The selected window lost exact keyboard focus before input.";
			return false;
		};
		const auto prepare_pointer_input = [&]()
		{
			if (interrupted())
				return false;
			if (reference.target_kind == "window" && !IsAxWindowTargetCurrent(window_target, reference))
			{
				if (error_out != nullptr)
					*error_out = "The selected window changed before pointer input.";
				return false;
			}
			return !interrupted();
		};
		const auto verify_pointer_target = [&]()
		{
			if (interrupted())
				return false;
			if (reference.target_kind == "window" && !IsAxWindowTargetCurrent(window_target, reference))
			{
				if (error_out != nullptr)
					*error_out = "The selected window changed during pointer input.";
				return false;
			}
			return !interrupted();
		};

		int click_element_id = action.element_id;
		if (action.kind == "click" && click_element_id == 0 && reference.target_kind == "window" && action.button == "left" && action.click_count == 1)
		{
			const Element* hit = nullptr;
			for (const Element& candidate : reference.elements)
			{
				const bool pressable = candidate.role == "AXButton" || candidate.role == "AXLink" || candidate.role == "AXCheckBox" || candidate.role == "AXRadioButton" || candidate.role == "AXPopUpButton" || candidate.role == "AXDisclosureTriangle" || candidate.role == "AXMenuItem" || candidate.role == "AXTextField" || candidate.role == "AXTextArea" || candidate.role == "AXComboBox";
				if (candidate.enabled && pressable && action.x >= candidate.x && action.y >= candidate.y && action.x < candidate.x + candidate.width && action.y < candidate.y + candidate.height &&
					(hit == nullptr || candidate.width * candidate.height < hit->width * hit->height))
					hit = &candidate;
			}
			if (hit != nullptr)
				click_element_id = hit->id;
		}

		if (action.kind == "click" && click_element_id > 0 && action.button == "left" && action.click_count == 1)
		{
			const pid_t target_process_id = static_cast<pid_t>(reference.process_id);
			AXUIElementRef element = AccessibilityElement(target_process_id, window_bounds, click_element_id);
			if (element == nullptr)
			{
				if (error_out != nullptr)
					*error_out = "The selected accessibility element is no longer available. Observe again before retrying.";
				return false;
			}
			const auto reference_element = std::ranges::find_if(reference.elements, [id = click_element_id](const Element& candidate) { return candidate.id == id; });
			const std::optional<Element> reacquired_description = DescribeAxElement(element, window_bounds);
			if (reference_element == reference.elements.end() || !reacquired_description.has_value())
			{
				CFRelease(element);
				if (error_out != nullptr)
					*error_out = "The selected accessibility element changed or is no longer describable. Observe again before retrying.";
				return false;
			}
			Element normalized_description = *reacquired_description;
			NormalizeElement(normalized_description, window_bounds, reference.width, reference.height);
			const auto close = [](double left, double right) { return std::abs(left - right) <= 1.0; };
			const bool same_element = normalized_description.role == reference_element->role &&
				normalized_description.label == reference_element->label && normalized_description.enabled == reference_element->enabled &&
				close(normalized_description.x, reference_element->x) && close(normalized_description.y, reference_element->y) &&
				close(normalized_description.width, reference_element->width) && close(normalized_description.height, reference_element->height);
			if (!same_element)
			{
				CFRelease(element);
				if (error_out != nullptr)
					*error_out = "The selected accessibility element changed before input. Observe again before retrying.";
				return false;
			}
			if (!prepare_pointer_input())
			{
				CFRelease(element);
				return false;
			}
			if (interrupted())
			{
				CFRelease(element);
				return false;
			}
			const std::string role = AxString(element, kAXRoleAttribute);
			const bool is_text_control = role == "AXTextField" || role == "AXTextArea" || role == "AXComboBox";
			Boolean focused_attribute_settable = false;
			const AXError settable_result = is_text_control ? AXUIElementIsAttributeSettable(element, kAXFocusedAttribute, &focused_attribute_settable) : kAXErrorAttributeUnsupported;
			if (is_text_control && settable_result == kAXErrorSuccess && focused_attribute_settable)
			{
				if (interrupted())
				{
					CFRelease(element);
					return false;
				}
				if (input_applied_out != nullptr)
					*input_applied_out = true;
				const AXError focus_result = AXUIElementSetAttributeValue(element, kAXFocusedAttribute, kCFBooleanTrue);
				CFRelease(element);
				// Inactive apps can accept control focus while AXFocused remains false.
				// Keyboard actions separately validate the destination window.
				if (focus_result == kAXErrorSuccess)
					return true;
				if (error_out != nullptr)
					*error_out = "The selected text control rejected focus (AX error " + std::to_string(static_cast<int>(focus_result)) + ").";
				return false;
			}
			CFArrayRef action_names = nullptr;
			const AXError action_names_result = AXUIElementCopyActionNames(element, &action_names);
			if (action_names_result != kAXErrorSuccess || action_names == nullptr || CFGetTypeID(action_names) != CFArrayGetTypeID())
			{
				if (action_names != nullptr)
					CFRelease(action_names);
				CFRelease(element);
				if (error_out != nullptr)
					*error_out = "The selected accessibility element could not be queried safely for actions.";
				return false;
			}
			const bool supports_press = CFArrayContainsValue(action_names, CFRangeMake(0, CFArrayGetCount(action_names)), kAXPressAction);
			CFRelease(action_names);
			if (!supports_press)
			{
				CFRelease(element);
				element = nullptr;
			}
			else
			{
				if (interrupted())
				{
					CFRelease(element);
					return false;
				}
				if (input_applied_out != nullptr)
					*input_applied_out = true;
				const AXError result = AXUIElementPerformAction(element, kAXPressAction);
				CFRelease(element);
				if (result == kAXErrorSuccess)
					return true;
				if (error_out != nullptr)
					*error_out = "The selected accessibility element press failed after input began.";
				return false;
			}
		}

		if (action.kind == "move" || action.kind == "click" || action.kind == "drag" || action.kind == "scroll")
		{
			if (reference.target_kind == "window" && !pointer_focus.Begin(window_target, reference, interrupted, input_applied_out, error_out))
				return false;
		}

		if (action.kind == "move" || action.kind == "click")
		{
			const CGPoint point = DesktopPoint(action, reference);
			CGMouseButton button = kCGMouseButtonLeft;
			CGEventType down = kCGEventLeftMouseDown;
			CGEventType up = kCGEventLeftMouseUp;
			if (action.button == "right")
			{
				button = kCGMouseButtonRight;
				down = kCGEventRightMouseDown;
				up = kCGEventRightMouseUp;
			}
			else if (action.button == "middle")
			{
				button = kCGMouseButtonCenter;
				down = kCGEventOtherMouseDown;
				up = kCGEventOtherMouseUp;
			}
			CGEventRef move = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, point, button);
			if (move == nullptr)
			{
				if (error_out != nullptr)
					*error_out = "Mouse move event could not be created.";
				return false;
			}
			if (!prepare_pointer_input())
			{
				CFRelease(move);
				return false;
			}
			if (!PostEvent(move, reference, action.kind == "move" ? input_applied_out : nullptr))
			{
				CFRelease(move);
				if (error_out != nullptr)
					*error_out = "Mouse move event could not be posted safely.";
				return false;
			}
			CFRelease(move);
			if (action.kind == "move")
				return true;
			if (interrupted())
				return false;

			for (int click = 1; click <= action.click_count; ++click)
			{
				if (interrupted())
					return false;
				CGEventRef down_event = CGEventCreateMouseEvent(nullptr, down, point, button);
				CGEventRef up_event = CGEventCreateMouseEvent(nullptr, up, point, button);
				if (down_event == nullptr || up_event == nullptr)
				{
					if (down_event != nullptr)
						CFRelease(down_event);
					if (up_event != nullptr)
						CFRelease(up_event);
					if (error_out != nullptr)
						*error_out = "Mouse events could not be created.";
					return false;
				}
				CGEventSetIntegerValueField(down_event, kCGMouseEventClickState, click);
				CGEventSetIntegerValueField(up_event, kCGMouseEventClickState, click);
				if (!prepare_pointer_input())
				{
					CFRelease(down_event);
					CFRelease(up_event);
					return false;
				}
				if (!PostEvent(down_event, reference, input_applied_out))
				{
					CFRelease(down_event);
					CFRelease(up_event);
					if (error_out != nullptr)
						*error_out = "Mouse button-down event could not be posted safely.";
					return false;
				}
				if (interrupted())
				{
					(void)PostEvent(up_event, reference);
					CFRelease(down_event);
					CFRelease(up_event);
					return false;
				}
				const bool release_target_ready = verify_pointer_target();
				const bool release_posted = PostEvent(up_event, reference);
				CFRelease(down_event);
				CFRelease(up_event);
				if (!release_posted)
				{
					if (error_out != nullptr)
						*error_out = "Mouse button-up event could not be posted safely.";
					return false;
				}
				if (!release_target_ready)
					return false;
			}
			return true;
		}
		if (action.kind == "drag")
		{
			const CGPoint start = DesktopPoint(action, reference);
			Action end_action = action;
			end_action.x = action.end_x;
			end_action.y = action.end_y;
			const CGPoint end = DesktopPoint(end_action, reference);
			CGMouseButton button = action.button == "right" ? kCGMouseButtonRight : action.button == "middle" ? kCGMouseButtonCenter : kCGMouseButtonLeft;
			CGEventType down = button == kCGMouseButtonRight ? kCGEventRightMouseDown : button == kCGMouseButtonCenter ? kCGEventOtherMouseDown : kCGEventLeftMouseDown;
			CGEventType dragged = button == kCGMouseButtonRight ? kCGEventRightMouseDragged : button == kCGMouseButtonCenter ? kCGEventOtherMouseDragged : kCGEventLeftMouseDragged;
			CGEventType up = button == kCGMouseButtonRight ? kCGEventRightMouseUp : button == kCGMouseButtonCenter ? kCGEventOtherMouseUp : kCGEventLeftMouseUp;
			CGEventRef move = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved, start, button);
			CGEventRef down_event = CGEventCreateMouseEvent(nullptr, down, start, button);
			CGEventRef drag_event = CGEventCreateMouseEvent(nullptr, dragged, start, button);
			CGEventRef up_event = CGEventCreateMouseEvent(nullptr, up, start, button);
			CGPoint last_posted = start;
			bool pressed = false;
			const auto release_events = [&]()
			{
				bool release_posted = true;
				if (pressed)
				{
					CGEventSetLocation(up_event, last_posted);
					release_posted = PostEvent(up_event, reference);
				}
				if (move != nullptr) CFRelease(move);
				if (down_event != nullptr) CFRelease(down_event);
				if (drag_event != nullptr) CFRelease(drag_event);
				if (up_event != nullptr) CFRelease(up_event);
				return release_posted;
			};
			if (move == nullptr || down_event == nullptr || drag_event == nullptr || up_event == nullptr)
			{
				release_events();
				if (error_out != nullptr) *error_out = "Drag events could not be created.";
				return false;
			}
			if (!prepare_pointer_input())
			{
				release_events();
				return false;
			}
			if (!PostEvent(move, reference))
			{
				release_events();
				if (error_out != nullptr)
					*error_out = "Drag move event could not be posted safely.";
				return false;
			}
			if (!prepare_pointer_input())
			{
				release_events();
				return false;
			}
			if (!PostEvent(down_event, reference, input_applied_out))
			{
				release_events();
				if (error_out != nullptr)
					*error_out = "Drag button-down event could not be posted safely.";
				return false;
			}
			pressed = true;
			const int steps = std::max(1, static_cast<int>(std::ceil(action.duration_ms / (1000.0 / 60.0))));
			const auto started = std::chrono::steady_clock::now();
			for (int step = 1; step <= steps; ++step)
			{
				std::this_thread::sleep_until(started + std::chrono::milliseconds(action.duration_ms * step / steps));
				if (!verify_pointer_target())
				{
					release_events();
					return false;
				}
				const CGFloat progress = static_cast<CGFloat>(step) / steps;
				const CGPoint next_point = CGPointMake(start.x + (end.x - start.x) * progress,
				    start.y + (end.y - start.y) * progress);
				CGEventSetLocation(drag_event, next_point);
				if (!PostEvent(drag_event, reference))
				{
					release_events();
					if (error_out != nullptr)
						*error_out = "Drag event could not be posted safely.";
					return false;
				}
				last_posted = next_point;
			}
			const bool release_target_ready = verify_pointer_target();
			const bool release_posted = release_events();
			if (!release_posted)
			{
				if (error_out != nullptr)
					*error_out = "Drag button-up event could not be posted safely.";
				return false;
			}
			return release_target_ready;
		}

		if (action.kind == "scroll")
		{
			CGEventRef event = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitPixel, 2, static_cast<std::int32_t>(std::llround(action.delta_y)), static_cast<std::int32_t>(std::llround(action.delta_x)));
			if (event == nullptr)
			{
				if (error_out != nullptr)
					*error_out = "Scroll event could not be created.";
				return false;
			}
			CGEventSetLocation(event, DesktopPoint(action, reference));
			if (!prepare_pointer_input())
			{
				CFRelease(event);
				return false;
			}
			if (!PostEvent(event, reference, input_applied_out))
			{
				CFRelease(event);
				if (error_out != nullptr)
					*error_out = "Scroll event could not be posted safely.";
				return false;
			}
			CFRelease(event);
			return true;
		}
		if (action.kind == "type")
		{
			CFStringRef text = CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(action.text.data()), static_cast<CFIndex>(action.text.size()), kCFStringEncodingUTF8, false);
			if (text == nullptr)
			{
				if (error_out != nullptr)
					*error_out = "Text is not valid UTF-8.";
				return false;
			}
			const CFIndex length = CFStringGetLength(text);
			std::vector<UniChar> characters(static_cast<std::size_t>(length));
			CFStringGetCharacters(text, CFRangeMake(0, length), characters.data());
			CFRelease(text);
			for (CFIndex offset = 0; offset < length; offset += 20)
			{
				if (interrupted())
					return false;
				if (!keyboard_target_is_focused())
					return false;
				const UniCharCount count = static_cast<UniCharCount>(std::min<CFIndex>(20, length - offset));
				CGEventRef down_event = CGEventCreateKeyboardEvent(nullptr, 0, true);
				CGEventRef up_event = CGEventCreateKeyboardEvent(nullptr, 0, false);
				if (down_event == nullptr || up_event == nullptr)
				{
					if (down_event != nullptr)
						CFRelease(down_event);
					if (up_event != nullptr)
						CFRelease(up_event);
					if (error_out != nullptr)
						*error_out = "Text input events could not be created.";
					return false;
				}
				CGEventKeyboardSetUnicodeString(down_event, count, characters.data() + offset);
				CGEventKeyboardSetUnicodeString(up_event, count, characters.data() + offset);
				if (!PostEvent(down_event, reference, input_applied_out))
				{
					CFRelease(down_event);
					CFRelease(up_event);
					if (error_out != nullptr)
						*error_out = "Text key-down event could not be posted safely.";
					return false;
				}
				if (interrupted())
				{
					(void)PostEvent(up_event, reference);
					CFRelease(down_event);
					CFRelease(up_event);
					return false;
				}
				const bool release_posted = PostEvent(up_event, reference);
				CFRelease(down_event);
				CFRelease(up_event);
				if (!release_posted)
				{
					if (error_out != nullptr)
						*error_out = "Text key-up event could not be posted safely.";
					return false;
				}
			}
			return true;
		}
		if (action.kind == "hotkey")
		{
			struct HotkeyKey
			{
				CGKeyCode code;
				CGEventFlags modifier;
				bool is_modifier;
			};
			std::vector<HotkeyKey> modifiers;
			std::vector<HotkeyKey> ordinary_keys;
			for (const std::string& raw_key : action.keys)
			{
				const std::string key = Lower(raw_key);
				const auto found = KeyCodes().find(key);
				if (found == KeyCodes().end())
				{
					if (error_out != nullptr)
						*error_out = "Unsupported hotkey key: " + raw_key;
					return false;
				}
				const CGEventFlags modifier = ModifierFlag(key);
				HotkeyKey parsed{found->second, modifier, modifier != 0};
				if (parsed.is_modifier)
				{
					modifiers.push_back(parsed);
				}
				else
					ordinary_keys.push_back(parsed);
			}
			modifiers.insert(modifiers.end(), ordinary_keys.begin(), ordinary_keys.end());
			const std::vector<HotkeyKey>& keys = modifiers;
			std::size_t posted = 0;
			CGEventFlags pressed_flags = 0;
			bool post_failed = false;
			bool focus_lost = false;
			for (; posted < keys.size();)
			{
				if (interrupted())
					break;
				if (!keyboard_target_is_focused())
				{
					focus_lost = true;
					break;
				}
				const HotkeyKey& key = keys[posted];
				CGEventFlags next_flags = pressed_flags;
				if (key.is_modifier)
					next_flags |= key.modifier;
				if (!PostKey(key.code, true, next_flags, reference, input_applied_out))
				{
					post_failed = true;
					break;
				}
				pressed_flags = next_flags;
				++posted;
				if (interrupted())
					break;
			}
			if (!cancellation_seen && !keyboard_target_is_focused())
				focus_lost = true;
			bool release_failed = false;
			CGEventFlags release_flags = pressed_flags;
			for (std::size_t index = posted; index > 0; --index)
			{
				(void)interrupted();
				const HotkeyKey& key = keys[index - 1];
				if (key.is_modifier)
					release_flags &= ~key.modifier;
				release_failed = !PostKey(key.code, false, release_flags, reference, nullptr) || release_failed;
			}
			if (cancellation_seen)
				return false;
			if (focus_lost)
				return false;
			if (post_failed || release_failed || posted != keys.size())
			{
				if (error_out != nullptr)
					*error_out = "Hotkey events could not be posted safely; all pressed keys were released.";
				return false;
			}
			return true;
		}
		return action.kind == "wait" && !interrupted();
	}

	bool EnsureActionPermission(std::string* error_out)
	{
		if (AXIsProcessTrusted())
			return true;
		if (error_out != nullptr)
			*error_out = "macOS denied Accessibility input. Enable UAM Computer Use in System Settings > Privacy & Security > Accessibility, then restart UAM. If it is already enabled after an app update, remove the stale entry and add the current UAM Computer Use app again.";
		return false;
	}

	bool EnsureCapturePermission(std::string* error_out)
	{
		if (CGPreflightScreenCaptureAccess())
			return true;
		if (error_out != nullptr)
			*error_out = "macOS denied Screen Recording. Enable UAM Computer Use in System Settings > Privacy & Security > Screen & System Audio Recording, then restart UAM.";
		return false;
	}

	bool RequestCapturePermission(std::string* error_out)
	{
		if (CGPreflightScreenCaptureAccess()) return true;
		if (CGRequestScreenCaptureAccess()) return true;
		return EnsureCapturePermission(error_out);
	}

	bool RequestActionPermission(std::string* error_out)
	{
		NSDictionary* options = @{(__bridge NSString*)kAXTrustedCheckOptionPrompt : @YES};
		if (AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options)) return true;
		return EnsureActionPermission(error_out);
	}

	ApplicationIdentity ApplicationIdentityForTarget(const std::string& kind, std::uint64_t, std::uint64_t process_id)
	{
		if (kind != "window" || process_id == 0) return {};
		return ApplicationForPid(static_cast<pid_t>(process_id));
	}

	bool OpenPermissionSettings(const std::string& permission, std::string* error_out)
	{
		NSString* url_text = permission == "accessibility"
		    ? @"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility"
		    : @"x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture";
		NSURL* url = [NSURL URLWithString:url_text];
		if ([[NSWorkspace sharedWorkspace] openURL:url]) return true;
		if (error_out != nullptr) *error_out = "Open System Settings manually and enable UAM Computer Use under Privacy & Security.";
		return false;
	}

	int RunWithUi(const std::function<int()>& work)
	{
		@autoreleasepool
		{
			if (![NSBundle.mainBundle.bundleIdentifier isEqualToString:@"com.universalagentmanager.desktop.computer-use"])
				return work();
			[NSApplication sharedApplication];
			[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
			[NSApp finishLaunching];
			int result = 1;
			std::thread worker(
			    [&]()
			    {
				    result = work();
				    dispatch_async(dispatch_get_main_queue(), ^{
					  [NSApp stop:nil];
					  NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
					  [NSApp postEvent:wake atStart:NO];
				    });
			    });
			[NSApp run];
			worker.join();
			return result;
		}
	}
} // namespace uam::computer_use
