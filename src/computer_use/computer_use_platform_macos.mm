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
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

@interface UAMComputerUseCursorView : NSView
{
	NSImageView* image_view_;
}
- (void)animateClick;
- (void)animateMotionFrom:(NSPoint)from to:(NSPoint)to;
@end

@implementation UAMComputerUseCursorView
- (instancetype)initWithFrame:(NSRect)frame
{
	self = [super initWithFrame:frame];
	if (self == nil)
		return nil;

	self.wantsLayer = YES;
	self.layer.backgroundColor = NSColor.clearColor.CGColor;
	image_view_ = [[NSImageView alloc] initWithFrame:NSMakeRect(21, 17, 18, 21)];
	NSString* cursor_path = [NSBundle.mainBundle pathForResource:@"SoftwareCursor" ofType:@"png"];
	image_view_.image = cursor_path == nil ? nil : [[NSImage alloc] initWithContentsOfFile:cursor_path];
	image_view_.imageScaling = NSImageScaleProportionallyUpOrDown;
	[self addSubview:image_view_];

	return self;
}

- (BOOL)isOpaque
{
	return NO;
}

- (void)animateClick
{
	CAKeyframeAnimation* press = [CAKeyframeAnimation animationWithKeyPath:@"transform.scale"];
	press.values = @[ @1, @0.82, @1.04, @1 ];
	press.keyTimes = @[ @0, @0.28, @0.68, @1 ];
	press.duration = 0.24;
	press.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseInEaseOut];
	[self.layer addAnimation:press forKey:@"press"];
}

- (void)animateMotionFrom:(NSPoint)from to:(NSPoint)to
{
	const CGFloat dx = to.x - from.x;
	const CGFloat dy = to.y - from.y;
	const CGFloat distance = std::hypot(dx, dy);
	if (distance < 2)
		return;
	const CGFloat angle = std::clamp(dx / std::max<CGFloat>(distance, 1) * -0.13, -0.13, 0.13);
	const CGFloat stretch = 1 + std::min<CGFloat>(0.12, distance / 1800);
	CGAffineTransform scoot = CGAffineTransformMakeRotation(angle);
	scoot = CGAffineTransformScale(scoot, stretch, 1 / std::sqrt(stretch));
	CABasicAnimation* settle = [CABasicAnimation animationWithKeyPath:@"transform"];
	settle.fromValue = [NSValue valueWithCATransform3D:CATransform3DMakeAffineTransform(scoot)];
	settle.toValue = [NSValue valueWithCATransform3D:CATransform3DIdentity];
	settle.duration = 0.34;
	settle.timingFunction = [CAMediaTimingFunction functionWithName:kCAMediaTimingFunctionEaseOut];
	[self.layer addAnimation:settle forKey:@"scoot"];
}
@end

namespace uam::computer_use
{
	namespace
	{
		int controller_lock_fd = -1;

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
		constexpr CGFloat kCursorHotspotX = 39;
		constexpr CGFloat kCursorHotspotY = 30;

		void EnsureCursorPanel()
		{
			if (cursor_panel != nil)
				return;
			[NSApplication sharedApplication];
			[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
			[NSApp finishLaunching];
			cursor_panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 56, 56) styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel backing:NSBackingStoreBuffered defer:NO];
			cursor_panel.opaque = NO;
			cursor_panel.backgroundColor = [NSColor clearColor];
			cursor_panel.hasShadow = NO;
			cursor_panel.hidesOnDeactivate = NO;
			cursor_panel.ignoresMouseEvents = YES;
			cursor_panel.level = CGWindowLevelForKey(kCGCursorWindowLevelKey) - 1;
			cursor_panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorFullScreenAuxiliary;
			cursor_view = [[UAMComputerUseCursorView alloc] initWithFrame:NSMakeRect(0, 0, 56, 56)];
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
			  const NSPoint origin = NSMakePoint(point.x - kCursorHotspotX, primary.size.height - point.y - kCursorHotspotY);
			  NSPoint previous = cursor_panel.frame.origin;
			  if (!cursor_panel.visible)
			  {
				  const CGPoint center = CGPointMake(reference.desktop_x + reference.desktop_width / 2, reference.desktop_y + reference.desktop_height / 2);
				  previous = NSMakePoint(center.x - kCursorHotspotX, primary.size.height - center.y - kCursorHotspotY);
				  [cursor_panel setFrameOrigin:previous];
			  }
			  ShowCursorPanel();
			  const CGFloat distance = std::hypot(origin.x - previous.x, origin.y - previous.y);
			  [cursor_view animateMotionFrom:previous to:origin];
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

		void PostEvent(CGEventRef event, const Capture& reference, bool* input_applied_out = nullptr)
		{
			if (reference.process_id != 0)
			{
				const CGEventType type = CGEventGetType(event);
				if (type == kCGEventMouseMoved || type == kCGEventLeftMouseDown || type == kCGEventLeftMouseUp || type == kCGEventLeftMouseDragged || type == kCGEventRightMouseDown || type == kCGEventRightMouseUp || type == kCGEventRightMouseDragged || type == kCGEventOtherMouseDown || type == kCGEventOtherMouseUp || type == kCGEventOtherMouseDragged || type == kCGEventScrollWheel)
				{
					CGEventSetIntegerValueField(event, kCGMouseEventWindowUnderMousePointer, static_cast<std::int64_t>(reference.target_id));
					CGEventSetIntegerValueField(event, kCGMouseEventWindowUnderMousePointerThatCanHandleThisEvent, static_cast<std::int64_t>(reference.target_id));
				}
				CGEventPostToPid(static_cast<pid_t>(reference.process_id), event);
			}
			else
				CGEventPost(kCGHIDEventTap, event);
			if (input_applied_out != nullptr)
				*input_applied_out = true;
		}

		bool PostKey(CGKeyCode code, bool down, CGEventFlags flags, const Capture& reference, bool* input_applied_out)
		{
			CGEventRef event = CGEventCreateKeyboardEvent(nullptr, code, down);
			if (event == nullptr)
				return false;
			CGEventSetFlags(event, flags);
			PostEvent(event, reference, input_applied_out);
			CFRelease(event);
			return true;
		}

		const std::unordered_map<std::string, CGKeyCode>& KeyCodes()
		{
			static const std::unordered_map<std::string, CGKeyCode> codes = {
			    {"a", kVK_ANSI_A}, {"b", kVK_ANSI_B}, {"c", kVK_ANSI_C}, {"d", kVK_ANSI_D}, {"e", kVK_ANSI_E}, {"f", kVK_ANSI_F}, {"g", kVK_ANSI_G}, {"h", kVK_ANSI_H}, {"i", kVK_ANSI_I}, {"j", kVK_ANSI_J}, {"k", kVK_ANSI_K}, {"l", kVK_ANSI_L}, {"m", kVK_ANSI_M}, {"n", kVK_ANSI_N}, {"o", kVK_ANSI_O}, {"p", kVK_ANSI_P}, {"q", kVK_ANSI_Q}, {"r", kVK_ANSI_R}, {"s", kVK_ANSI_S}, {"t", kVK_ANSI_T}, {"u", kVK_ANSI_U}, {"v", kVK_ANSI_V}, {"w", kVK_ANSI_W}, {"x", kVK_ANSI_X}, {"y", kVK_ANSI_Y}, {"z", kVK_ANSI_Z}, {"0", kVK_ANSI_0}, {"1", kVK_ANSI_1}, {"2", kVK_ANSI_2}, {"3", kVK_ANSI_3}, {"4", kVK_ANSI_4}, {"5", kVK_ANSI_5}, {"6", kVK_ANSI_6}, {"7", kVK_ANSI_7}, {"8", kVK_ANSI_8}, {"9", kVK_ANSI_9}, {"enter", kVK_Return}, {"return", kVK_Return}, {"tab", kVK_Tab}, {"space", kVK_Space}, {"escape", kVK_Escape}, {"esc", kVK_Escape}, {"backspace", kVK_Delete}, {"delete", kVK_ForwardDelete}, {"left", kVK_LeftArrow}, {"right", kVK_RightArrow}, {"up", kVK_UpArrow}, {"down", kVK_DownArrow}, {"home", kVK_Home}, {"end", kVK_End}, {"pageup", kVK_PageUp}, {"pagedown", kVK_PageDown}, {"cmd", kVK_Command}, {"command", kVK_Command}, {"ctrl", kVK_Control}, {"control", kVK_Control}, {"alt", kVK_Option}, {"option", kVK_Option}, {"shift", kVK_Shift}, {"fn", kVK_Function},
			};
			return codes;
		}

		CGEventFlags ModifierFlag(std::string_view key)
		{
			if (key == "cmd" || key == "command")
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
			CFTypeRef frontmost_value = nullptr;
			const bool frontmost = AXUIElementCopyAttributeValue(target.application, kAXFrontmostAttribute, &frontmost_value) == kAXErrorSuccess && frontmost_value != nullptr && CFGetTypeID(frontmost_value) == CFBooleanGetTypeID() && CFBooleanGetValue(static_cast<CFBooleanRef>(frontmost_value));
			CFTypeRef focused_window_value = nullptr;
			const bool focused_window_available = AXUIElementCopyAttributeValue(target.application, kAXFocusedWindowAttribute, &focused_window_value) == kAXErrorSuccess && focused_window_value != nullptr && CFGetTypeID(focused_window_value) == AXUIElementGetTypeID();
			const bool exact_window_focused = focused_window_available && CFEqual(focused_window_value, target.window);
			CGRect focused_bounds{};
			const bool bounds_match = exact_window_focused && AxBounds(static_cast<AXUIElementRef>(focused_window_value), &focused_bounds) && AxWindowBoundsDifference(focused_bounds, reference_bounds) <= kAxWindowMatchTolerance;
			if (focused_window_value != nullptr)
				CFRelease(focused_window_value);
			if (frontmost_value != nullptr)
				CFRelease(frontmost_value);
			return frontmost && exact_window_focused && bounds_match;
		}

		bool FocusAndVerifyAxWindow(const AxWindowTarget& target, const CGRect& reference_bounds)
		{
			if (target.application == nullptr || target.window == nullptr)
				return false;
			const AXError frontmost_result = AXUIElementSetAttributeValue(target.application, kAXFrontmostAttribute, kCFBooleanTrue);
			const AXError raise_result = AXUIElementPerformAction(target.window, kAXRaiseAction);
			(void)AXUIElementSetAttributeValue(target.window, kAXMainAttribute, kCFBooleanTrue);
			(void)AXUIElementSetAttributeValue(target.window, kAXFocusedAttribute, kCFBooleanTrue);
			return frontmost_result == kAXErrorSuccess && raise_result == kAXErrorSuccess && IsAxWindowFocused(target, reference_bounds);
		}

		bool FocusAxWindow(pid_t pid, const CGRect& reference_bounds, AxWindowTarget* target_out, std::string* error_out)
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
			AxWindowTarget target;
			target.application = application;
			target.window = window;
			if (FocusAndVerifyAxWindow(target, reference_bounds))
			{
				target_out->application = application;
				target_out->window = window;
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
				return Element{0, role.empty() ? "element" : role, label, bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height, enabled};
			return std::nullopt;
		}

		void AppendAxElements(AXUIElementRef element, const CGRect& window_bounds, std::vector<Element>& result, int depth)
		{
			if (depth > 12 || result.size() >= 200)
				return;
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
			for (CFIndex index = 0; index < CFArrayGetCount(children) && result.size() < 200; ++index)
			{
				AppendAxElements(static_cast<AXUIElementRef>(CFArrayGetValueAtIndex(children, index)), window_bounds, result, depth + 1);
			}
			CFRelease(children_value);
		}

		AXUIElementRef FindAxElement(AXUIElementRef element, const CGRect& window_bounds, int target_id, int& current_id, int depth)
		{
			if (depth > 12 || current_id >= 200)
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
			for (CFIndex index = 0; index < CFArrayGetCount(children) && found == nullptr; ++index)
				found = FindAxElement(static_cast<AXUIElementRef>(CFArrayGetValueAtIndex(children, index)), window_bounds, target_id, current_id, depth + 1);
			CFRelease(children_value);
			return found;
		}

		std::vector<Element> AccessibilityElements(pid_t pid, const CGRect& window_bounds)
		{
			std::vector<Element> result;
			AXUIElementRef application = AXUIElementCreateApplication(pid);
			AXUIElementRef window = MatchingAxWindow(application, window_bounds);
			if (window != nullptr)
			{
				AppendAxElements(window, window_bounds, result, 0);
				CFRelease(window);
			}
			if (application != nullptr)
				CFRelease(application);
			return result;
		}

		AXUIElementRef AccessibilityElement(pid_t pid, const CGRect& window_bounds, int element_id)
		{
			AXUIElementRef application = AXUIElementCreateApplication(pid);
			int current_id = 0;
			AXUIElementRef window = MatchingAxWindow(application, window_bounds);
			AXUIElementRef result = window == nullptr ? nullptr : FindAxElement(window, window_bounds, element_id, current_id, 0);
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
					result.push_back({"screen", displays[index], "Full display " + std::to_string(index + 1), bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height, displays[index] == CGMainDisplayID(), "foreground", 0});
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
				result.push_back({"window", static_cast<std::uint64_t>(id), name.empty() ? owner : owner + " — " + name, bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height, false, "foreground", static_cast<std::uint64_t>(std::max<std::int64_t>(0, process_id))});
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
			source = ModernCapture(kind, display, bounds, max_width, max_height);
			if (source == nullptr)
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
			result.input_mode = "foreground";
			source = ModernCapture(kind, window, bounds, max_width, max_height);
			if (source == nullptr)
				source = LegacyCapture(kind, window);
		}
		if (source == nullptr && !CGPreflightScreenCaptureAccess())
		{
			result.error = "macOS denied Screen Recording. Enable the current UAM app in System Settings > Privacy & Security > Screen & System Audio Recording, then restart UAM. If it is already enabled after an app update, remove the stale entry and add the current UAM app again.";
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
			result.elements = AccessibilityElements(static_cast<pid_t>(result.process_id), bounds);
			for (Element& element : result.elements)
			{
				element.x = (element.x - bounds.origin.x) * result.width / bounds.size.width;
				element.y = (element.y - bounds.origin.y) * result.height / bounds.size.height;
				element.width *= result.width / bounds.size.width;
				element.height *= result.height / bounds.size.height;
			}
		}
		result.ok = true;
		return result;
	}

	bool ExecuteAction(const Action& action, const Capture& reference, const std::function<bool()>& cancelled, std::string* error_out, bool* input_applied_out)
	{
		if (input_applied_out != nullptr)
			*input_applied_out = false;
		AxWindowTarget window_target;
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
			if (!FocusAxWindow(static_cast<pid_t>(reference.process_id), window_bounds, &window_target, error_out))
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
			if (reference.target_kind == "window" && !FocusAndVerifyAxWindow(window_target, window_bounds))
			{
				if (error_out != nullptr)
					*error_out = "The selected window could not be focused and verified immediately before pointer input.";
				return false;
			}
			return !interrupted();
		};

		if (action.kind == "click" && action.element_id > 0 && action.button == "left" && action.click_count == 1)
		{
			const pid_t target_process_id = static_cast<pid_t>(reference.process_id);
			AXUIElementRef element = AccessibilityElement(target_process_id, window_bounds, action.element_id);
			if (element != nullptr)
			{
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
				if (input_applied_out != nullptr)
					*input_applied_out = true;
				const AXError result = AXUIElementPerformAction(element, kAXPressAction);
				CFRelease(element);
				if (result == kAXErrorSuccess)
					return true;
			}
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
			PostEvent(move, reference, action.kind == "move" ? input_applied_out : nullptr);
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
				PostEvent(down_event, reference, input_applied_out);
				if (interrupted())
				{
					PostEvent(up_event, reference);
					CFRelease(down_event);
					CFRelease(up_event);
					return false;
				}
				const bool release_target_ready = prepare_pointer_input();
				PostEvent(up_event, reference);
				CFRelease(down_event);
				CFRelease(up_event);
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
			CGEventRef drag_event = CGEventCreateMouseEvent(nullptr, dragged, end, button);
			CGEventRef up_event = CGEventCreateMouseEvent(nullptr, up, end, button);
			const auto release_events = [&]()
			{
				if (move != nullptr)
					CFRelease(move);
				if (down_event != nullptr)
					CFRelease(down_event);
				if (drag_event != nullptr)
					CFRelease(drag_event);
				if (up_event != nullptr)
					CFRelease(up_event);
			};
			if (move == nullptr || down_event == nullptr || drag_event == nullptr || up_event == nullptr)
			{
				release_events();
				if (error_out != nullptr)
					*error_out = "Drag events could not be created.";
				return false;
			}
			if (!prepare_pointer_input())
			{
				release_events();
				return false;
			}
			PostEvent(move, reference);
			if (interrupted())
			{
				release_events();
				return false;
			}
			if (!prepare_pointer_input())
			{
				release_events();
				return false;
			}
			PostEvent(down_event, reference, input_applied_out);
			if (interrupted())
			{
				CGEventSetLocation(up_event, start);
				PostEvent(up_event, reference);
				release_events();
				return false;
			}
			if (!prepare_pointer_input())
			{
				CGEventSetLocation(up_event, start);
				PostEvent(up_event, reference);
				release_events();
				return false;
			}
			PostEvent(drag_event, reference);
			if (interrupted())
			{
				PostEvent(up_event, reference);
				release_events();
				return false;
			}
			const bool release_target_ready = prepare_pointer_input();
			PostEvent(up_event, reference);
			release_events();
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
			PostEvent(event, reference, input_applied_out);
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
				PostEvent(down_event, reference, input_applied_out);
				if (interrupted())
				{
					PostEvent(up_event, reference);
					CFRelease(down_event);
					CFRelease(up_event);
					return false;
				}
				PostEvent(up_event, reference);
				CFRelease(down_event);
				CFRelease(up_event);
			}
			return true;
		}
		if (action.kind == "hotkey")
		{
			std::vector<std::pair<CGKeyCode, CGEventFlags>> keys;
			CGEventFlags flags = 0;
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
				flags |= ModifierFlag(key);
				keys.emplace_back(found->second, flags);
			}
			std::size_t posted = 0;
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
				if (!PostKey(keys[posted].first, true, keys[posted].second, reference, input_applied_out))
				{
					post_failed = true;
					break;
				}
				++posted;
				if (interrupted())
					break;
			}
			if (!cancellation_seen && !keyboard_target_is_focused())
				focus_lost = true;
			bool release_failed = false;
			for (std::size_t index = posted; index > 0; --index)
			{
				(void)interrupted();
				release_failed = !PostKey(keys[index - 1].first, false, flags, reference, nullptr) || release_failed;
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
			*error_out = "macOS denied Accessibility input. Enable the current UAM app in System Settings > Privacy & Security > Accessibility, then restart UAM. If it is already enabled after an app update, remove the stale entry and add the current UAM app again.";
		return false;
	}

	bool ConfirmComputerUse(const std::string& message)
	{
		__block bool allowed = false;
		void (^show_confirmation)() = ^{
		  @autoreleasepool
		  {
			  [NSApplication sharedApplication];
			  NSRunningApplication* previous = NSWorkspace.sharedWorkspace.frontmostApplication;
			  NSAlert* alert = [[NSAlert alloc] init];
			  alert.messageText = @"Allow UAM computer control";
			  alert.informativeText = [NSString stringWithUTF8String:message.c_str()];
			  [alert addButtonWithTitle:@"Deny"];
			  [alert addButtonWithTitle:@"Allow"];
			  alert.window.level = NSModalPanelWindowLevel;
			  alert.window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
			      NSWindowCollectionBehaviorFullScreenAuxiliary;
			  [NSApp activateIgnoringOtherApps:YES];
			  [alert.window orderFrontRegardless];
			  allowed = [alert runModal] == NSAlertSecondButtonReturn;
			  [previous activateWithOptions:NSApplicationActivateAllWindows];
		  }
		};
		if (NSThread.isMainThread)
			show_confirmation();
		else
			dispatch_sync(dispatch_get_main_queue(), show_confirmation);
		return allowed;
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
