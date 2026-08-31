#include "computer_use/computer_use_platform.h"

#include "common/utils/string_utils.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <UIAutomation.h>
#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uam::computer_use
{
	namespace
	{
		HANDLE controller_lock_handle = INVALID_HANDLE_VALUE;

		std::wstring Wide(std::string_view value)
		{
			if (value.empty())
				return {};
			const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
			if (size <= 0)
				return {};
			std::wstring result(static_cast<std::size_t>(size), L'\0');
			MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
			return result;
		}

		std::string Utf8(std::wstring_view value)
		{
			if (value.empty())
				return {};
			const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
			if (size <= 0)
				return {};
			std::string result(static_cast<std::size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
			return result;
		}

		std::string WindowTitle(HWND window)
		{
			const int size = GetWindowTextLengthW(window);
			if (size <= 0)
				return {};
			std::wstring title(static_cast<std::size_t>(size + 1), L'\0');
			const int written = GetWindowTextW(window, title.data(), size + 1);
			title.resize(static_cast<std::size_t>(std::max(0, written)));
			return Utf8(title);
		}

		BOOL CALLBACK AddMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context)
		{
			auto& targets = *reinterpret_cast<std::vector<Target>*>(context);
			MONITORINFO info{sizeof(info)};
			if (GetMonitorInfoW(monitor, &info))
			{
				targets.push_back({"screen", reinterpret_cast<std::uintptr_t>(monitor), "Display " + std::to_string(targets.size() + 1), static_cast<double>(info.rcMonitor.left), static_cast<double>(info.rcMonitor.top), static_cast<double>(info.rcMonitor.right - info.rcMonitor.left), static_cast<double>(info.rcMonitor.bottom - info.rcMonitor.top), (info.dwFlags & MONITORINFOF_PRIMARY) != 0, "foreground"});
			}
			return TRUE;
		}

		BOOL CALLBACK AddWindow(HWND window, LPARAM context)
		{
			if (!IsWindowVisible(window))
				return TRUE;
			DWORD process_id = 0;
			GetWindowThreadProcessId(window, &process_id);
			if (process_id == GetCurrentProcessId())
				return TRUE;
			RECT bounds{};
			const std::string title = WindowTitle(window);
			if (title.empty() || !GetWindowRect(window, &bounds) || bounds.right <= bounds.left || bounds.bottom <= bounds.top)
				return TRUE;
			auto& targets = *reinterpret_cast<std::vector<Target>*>(context);
			targets.push_back({"window", reinterpret_cast<std::uintptr_t>(window), title, static_cast<double>(bounds.left), static_cast<double>(bounds.top), static_cast<double>(bounds.right - bounds.left), static_cast<double>(bounds.bottom - bounds.top), false, "foreground", process_id});
			return TRUE;
		}

		bool TargetBounds(const std::string& kind, std::uint64_t raw_id, RECT* bounds_out, std::uint64_t* resolved_id_out)
		{
			if (kind == "screen")
			{
				HMONITOR monitor = raw_id == 0 ? MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY) : reinterpret_cast<HMONITOR>(static_cast<std::uintptr_t>(raw_id));
				MONITORINFO info{sizeof(info)};
				if (!GetMonitorInfoW(monitor, &info))
					return false;
				*bounds_out = info.rcMonitor;
				*resolved_id_out = reinterpret_cast<std::uintptr_t>(monitor);
				return true;
			}
			if (kind == "window")
			{
				HWND window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(raw_id));
				if (!IsWindow(window) || !GetWindowRect(window, bounds_out))
					return false;
				*resolved_id_out = raw_id;
				return true;
			}
			return false;
		}

		std::string EncodePng(HBITMAP bitmap)
		{
			const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			const bool uninitialize = initialized == S_OK || initialized == S_FALSE;
			IWICImagingFactory* factory = nullptr;
			IWICBitmap* source = nullptr;
			IWICBitmapEncoder* encoder = nullptr;
			IWICBitmapFrameEncode* frame = nullptr;
			IPropertyBag2* properties = nullptr;
			IStream* stream = nullptr;
			std::string result;

			if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUseAlpha, &source)) && SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) && SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, &properties)) && SUCCEEDED(frame->Initialize(properties)))
			{
				UINT width = 0;
				UINT height = 0;
				source->GetSize(&width, &height);
				WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
				if (SUCCEEDED(frame->SetSize(width, height)) && SUCCEEDED(frame->SetPixelFormat(&format)) && SUCCEEDED(frame->WriteSource(source, nullptr)) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()))
				{
					HGLOBAL memory = nullptr;
					STATSTG statistics{};
					if (SUCCEEDED(GetHGlobalFromStream(stream, &memory)) && SUCCEEDED(stream->Stat(&statistics, STATFLAG_NONAME)))
					{
						const void* bytes = GlobalLock(memory);
						if (bytes != nullptr)
						{
							result.assign(static_cast<const char*>(bytes), static_cast<std::size_t>(statistics.cbSize.QuadPart));
							GlobalUnlock(memory);
						}
					}
				}
			}
			if (properties != nullptr)
				properties->Release();
			if (frame != nullptr)
				frame->Release();
			if (encoder != nullptr)
				encoder->Release();
			if (stream != nullptr)
				stream->Release();
			if (source != nullptr)
				source->Release();
			if (factory != nullptr)
				factory->Release();
			if (uninitialize)
				CoUninitialize();
			return result;
		}

		POINT DesktopPoint(const Action& action, const Capture& reference)
		{
			return {
			    static_cast<LONG>(std::llround(reference.desktop_x + action.x * reference.desktop_width / std::max(1, reference.width))),
			    static_cast<LONG>(std::llround(reference.desktop_y + action.y * reference.desktop_height / std::max(1, reference.height))),
			};
		}

		std::wstring virtual_cursor_label;
		bool virtual_cursor_clicked = false;

		LRESULT CALLBACK VirtualCursorWindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
		{
			if (message != WM_PAINT)
				return DefWindowProcW(window, message, wparam, lparam);
			PAINTSTRUCT paint{};
			HDC context = BeginPaint(window, &paint);
			RECT bounds{};
			GetClientRect(window, &bounds);
			HBRUSH transparent = CreateSolidBrush(RGB(255, 0, 255));
			FillRect(context, &bounds, transparent);
			DeleteObject(transparent);

			HPEN accent_pen = CreatePen(PS_SOLID, 3, RGB(31, 209, 255));
			HPEN previous_pen = static_cast<HPEN>(SelectObject(context, accent_pen));
			HBRUSH hollow = static_cast<HBRUSH>(GetStockObject(HOLLOW_BRUSH));
			HBRUSH previous_brush = static_cast<HBRUSH>(SelectObject(context, hollow));
			if (virtual_cursor_clicked)
				Ellipse(context, 0, 0, 40, 40);

			POINT cursor[] = {{8, 8}, {8, 36}, {15, 29}, {21, 41}, {27, 38}, {21, 26}, {31, 26}};
			HPEN cursor_pen = CreatePen(PS_SOLID, 2, RGB(0, 0, 0));
			HBRUSH cursor_brush = CreateSolidBrush(RGB(255, 255, 255));
			SelectObject(context, cursor_pen);
			SelectObject(context, cursor_brush);
			Polygon(context, cursor, 7);

			SelectObject(context, accent_pen);
			HBRUSH badge_brush = CreateSolidBrush(RGB(20, 20, 20));
			SelectObject(context, badge_brush);
			RoundRect(context, 38, 11, 154, 36, 8, 8);
			SetBkMode(context, TRANSPARENT);
			SetTextColor(context, RGB(255, 255, 255));
			RECT text_bounds{48, 16, 148, 33};
			const std::wstring text = L"UAM  " + virtual_cursor_label;
			DrawTextW(context, text.c_str(), static_cast<int>(text.size()), &text_bounds, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

			SelectObject(context, previous_pen);
			SelectObject(context, previous_brush);
			DeleteObject(accent_pen);
			DeleteObject(cursor_pen);
			DeleteObject(cursor_brush);
			DeleteObject(badge_brush);
			EndPaint(window, &paint);
			return 0;
		}

		void ShowVirtualCursor(const Action& action, const Capture& reference)
		{
			if (action.kind != "move" && action.kind != "click" && action.kind != "drag" && action.kind != "scroll")
				return;
			static HWND window = nullptr;
			if (window == nullptr)
			{
				const wchar_t* class_name = L"UAMComputerUseVirtualCursor";
				WNDCLASSW window_class{};
				window_class.lpfnWndProc = VirtualCursorWindowProcedure;
				window_class.hInstance = GetModuleHandleW(nullptr);
				window_class.lpszClassName = class_name;
				RegisterClassW(&window_class);
				window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, class_name, L"", WS_POPUP, 0, 0, 160, 48, nullptr, nullptr, window_class.hInstance, nullptr);
				if (window == nullptr)
					return;
				SetLayeredWindowAttributes(window, RGB(255, 0, 255), 0, LWA_COLORKEY);
			}
			virtual_cursor_label = Wide(action.kind);
			CharUpperBuffW(virtual_cursor_label.data(), static_cast<DWORD>(virtual_cursor_label.size()));
			virtual_cursor_clicked = action.kind == "click";
			const POINT point = DesktopPoint(action, reference);
			SetWindowPos(window, HWND_TOPMOST, point.x - 8, point.y - 8, 160, 48, SWP_NOACTIVATE | SWP_SHOWWINDOW);
			InvalidateRect(window, nullptr, FALSE);
			UpdateWindow(window);
		}

		INPUT MouseInput(DWORD flags, LONG data = 0)
		{
			INPUT input{};
			input.type = INPUT_MOUSE;
			input.mi.dwFlags = flags;
			input.mi.mouseData = static_cast<DWORD>(data);
			return input;
		}

		INPUT DesktopMoveInput(const POINT& point)
		{
			const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
			const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
			const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
			const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
			INPUT move = MouseInput(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK);
			move.mi.dx = static_cast<LONG>(std::llround((point.x - left) * 65535.0 / std::max(1, width - 1)));
			move.mi.dy = static_cast<LONG>(std::llround((point.y - top) * 65535.0 / std::max(1, height - 1)));
			return move;
		}

		bool Send(std::vector<INPUT>& inputs, bool* input_applied_out = nullptr)
		{
			if (inputs.empty())
				return true;
			const UINT count = static_cast<UINT>(inputs.size());
			const UINT sent = SendInput(count, inputs.data(), sizeof(INPUT));
			if (sent > 0 && input_applied_out != nullptr)
				*input_applied_out = true;
			return sent == count;
		}

		std::string Lower(std::string value)
		{
			std::ranges::transform(value, value.begin(), [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
			return value;
		}

		std::optional<WORD> VirtualKey(const std::string& raw_key)
		{
			const std::string key = Lower(raw_key);
			static const std::unordered_map<std::string, WORD> named = {
			    {"ctrl", VK_CONTROL}, {"control", VK_CONTROL}, {"shift", VK_SHIFT}, {"alt", VK_MENU}, {"option", VK_MENU}, {"cmd", VK_LWIN}, {"command", VK_LWIN}, {"win", VK_LWIN}, {"enter", VK_RETURN}, {"return", VK_RETURN}, {"tab", VK_TAB}, {"space", VK_SPACE}, {"escape", VK_ESCAPE}, {"esc", VK_ESCAPE}, {"backspace", VK_BACK}, {"delete", VK_DELETE}, {"left", VK_LEFT}, {"right", VK_RIGHT}, {"up", VK_UP}, {"down", VK_DOWN}, {"home", VK_HOME}, {"end", VK_END}, {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT},
			};
			if (const auto found = named.find(key); found != named.end())
				return found->second;
			const std::wstring character = Wide(key);
			if (character.size() == 1)
			{
				const SHORT mapped = VkKeyScanW(character.front());
				if (mapped != -1)
					return static_cast<WORD>(mapped & 0xFF);
			}
			return std::nullopt;
		}

		ApplicationIdentity ApplicationForWindow(HWND window, std::uint64_t* process_id_out = nullptr)
		{
			if (window == nullptr)
				return {};
			DWORD process_id = 0;
			GetWindowThreadProcessId(window, &process_id);
			if (process_id_out != nullptr)
				*process_id_out = process_id;
			std::wstring executable(32768, L'\0');
			DWORD size = static_cast<DWORD>(executable.size());
			HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
			if (process != nullptr)
			{
				if (QueryFullProcessImageNameW(process, 0, executable.data(), &size))
					executable.resize(size);
				else
					executable.clear();
				CloseHandle(process);
			}
			const std::string title = WindowTitle(window);
			return {"pid:" + std::to_string(process_id) + "|" + Utf8(executable), title.empty() ? "Application" : title};
		}

		std::string BstrText(BSTR value)
		{
			if (value == nullptr)
				return {};
			const std::string result = uam::strings::SafeLine(Utf8(std::wstring_view(value, SysStringLen(value))), 200, true);
			SysFreeString(value);
			return result;
		}

		void AppendAutomationElements(IUIAutomationTreeWalker* walker, IUIAutomationElement* element, const RECT& window_bounds, std::vector<Element>& result, int depth)
		{
			if (walker == nullptr || element == nullptr || depth > 12 || result.size() >= 200)
				return;
			BOOL offscreen = TRUE;
			BOOL enabled = TRUE;
			RECT bounds{};
			BSTR role = nullptr;
			BSTR name = nullptr;
			(void)element->get_CurrentIsOffscreen(&offscreen);
			(void)element->get_CurrentIsEnabled(&enabled);
			(void)element->get_CurrentBoundingRectangle(&bounds);
			(void)element->get_CurrentLocalizedControlType(&role);
			(void)element->get_CurrentName(&name);
			const std::string role_text = BstrText(role);
			const std::string name_text = BstrText(name);
			const RECT intersection{std::max(bounds.left, window_bounds.left), std::max(bounds.top, window_bounds.top), std::min(bounds.right, window_bounds.right), std::min(bounds.bottom, window_bounds.bottom)};
			if (!offscreen && bounds.right > bounds.left && bounds.bottom > bounds.top && intersection.right > intersection.left && intersection.bottom > intersection.top && !name_text.empty())
			{
				result.push_back({static_cast<int>(result.size() + 1), role_text.empty() ? "element" : role_text, name_text, static_cast<double>(bounds.left), static_cast<double>(bounds.top), static_cast<double>(bounds.right - bounds.left), static_cast<double>(bounds.bottom - bounds.top), enabled != FALSE});
			}

			IUIAutomationElement* child = nullptr;
			if (FAILED(walker->GetFirstChildElement(element, &child)))
				return;
			while (child != nullptr && result.size() < 200)
			{
				AppendAutomationElements(walker, child, window_bounds, result, depth + 1);
				IUIAutomationElement* next = nullptr;
				(void)walker->GetNextSiblingElement(child, &next);
				child->Release();
				child = next;
			}
		}

		std::vector<Element> AccessibilityElements(HWND window, const RECT& window_bounds)
		{
			std::vector<Element> result;
			const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			const bool uninitialize = initialized == S_OK || initialized == S_FALSE;
			IUIAutomation* automation = nullptr;
			IUIAutomationElement* root = nullptr;
			IUIAutomationTreeWalker* walker = nullptr;
			if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))) && SUCCEEDED(automation->ElementFromHandle(window, &root)) && SUCCEEDED(automation->get_ControlViewWalker(&walker)))
			{
				AppendAutomationElements(walker, root, window_bounds, result, 0);
			}
			if (walker != nullptr)
				walker->Release();
			if (root != nullptr)
				root->Release();
			if (automation != nullptr)
				automation->Release();
			if (uninitialize)
				CoUninitialize();
			return result;
		}
	} // namespace

	bool AcquireControllerLock(std::string* error_out)
	{
		if (controller_lock_handle != INVALID_HANDLE_VALUE)
		{
			if (error_out != nullptr)
				*error_out = "Another UAM computer-use action is in progress. Observe again, then retry.";
			return false;
		}
		const HANDLE handle = CreateMutexW(nullptr, FALSE, L"Local\\UniversalAgentManagerComputerUseController");
		if (handle == nullptr)
		{
			if (error_out != nullptr)
				*error_out = "Computer-use MCP could not create its controller lock.";
			return false;
		}
		if (GetLastError() == ERROR_ALREADY_EXISTS)
		{
			CloseHandle(handle);
			if (error_out != nullptr)
				*error_out = "Another UAM computer-use action is in progress. Observe again, then retry.";
			return false;
		}
		controller_lock_handle = handle;
		return true;
	}

	void ReleaseControllerLock()
	{
		if (controller_lock_handle == INVALID_HANDLE_VALUE)
			return;
		CloseHandle(controller_lock_handle);
		controller_lock_handle = INVALID_HANDLE_VALUE;
	}

	std::vector<Target> ListTargets(std::string* error_out)
	{
		std::vector<Target> result;
		EnumDisplayMonitors(nullptr, nullptr, AddMonitor, reinterpret_cast<LPARAM>(&result));
		EnumWindows(AddWindow, reinterpret_cast<LPARAM>(&result));
		if (result.empty() && error_out != nullptr)
			*error_out = "No visible screens or windows were found on the active desktop.";
		return result;
	}

	Capture CaptureTarget(const std::string& kind, std::uint64_t raw_id, int max_width, int max_height)
	{
		Capture result;
		result.target_kind = kind;
		result.input_mode = "foreground";
		RECT bounds{};
		if (!TargetBounds(kind, raw_id, &bounds, &result.target_id))
		{
			result.error = "The selected screen or window is no longer available.";
			return result;
		}
		if (kind == "window")
		{
			const ApplicationIdentity application = ApplicationForWindow(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(result.target_id)), &result.process_id);
			result.application_id = application.id;
			result.application_title = application.title;
		}
		const int source_width = bounds.right - bounds.left;
		const int source_height = bounds.bottom - bounds.top;
		const double scale = std::min({1.0, static_cast<double>(max_width) / source_width, static_cast<double>(max_height) / source_height});
		const int width = std::max(1, static_cast<int>(std::floor(source_width * scale)));
		const int height = std::max(1, static_cast<int>(std::floor(source_height * scale)));

		HDC screen = GetDC(nullptr);
		HDC memory = screen == nullptr ? nullptr : CreateCompatibleDC(screen);
		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		void* pixels = nullptr;
		HBITMAP bitmap = memory == nullptr ? nullptr : CreateDIBSection(memory, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
		HGDIOBJ previous = bitmap == nullptr ? nullptr : SelectObject(memory, bitmap);
		if (screen == nullptr || memory == nullptr || bitmap == nullptr || pixels == nullptr)
		{
			if (previous != nullptr)
				SelectObject(memory, previous);
			if (bitmap != nullptr)
				DeleteObject(bitmap);
			if (memory != nullptr)
				DeleteDC(memory);
			if (screen != nullptr)
				ReleaseDC(nullptr, screen);
			result.error = "Windows could not allocate the screenshot buffer.";
			return result;
		}

		BOOL copied = FALSE;
		if (kind == "window")
		{
			HDC isolated = CreateCompatibleDC(screen);
			BITMAPINFO isolated_info = info;
			isolated_info.bmiHeader.biWidth = source_width;
			isolated_info.bmiHeader.biHeight = -source_height;
			void* isolated_pixels = nullptr;
			HBITMAP isolated_bitmap = isolated == nullptr ? nullptr : CreateDIBSection(isolated, &isolated_info, DIB_RGB_COLORS, &isolated_pixels, nullptr, 0);
			HGDIOBJ isolated_previous = isolated_bitmap == nullptr ? nullptr : SelectObject(isolated, isolated_bitmap);
			if (isolated != nullptr && isolated_bitmap != nullptr && isolated_pixels != nullptr)
			{
				const HWND window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(result.target_id));
				copied = PrintWindow(window, isolated, PW_RENDERFULLCONTENT);
				if (copied)
				{
					SetStretchBltMode(memory, HALFTONE);
					copied = StretchBlt(memory, 0, 0, width, height, isolated, 0, 0, source_width, source_height, SRCCOPY);
				}
			}
			if (isolated_previous != nullptr)
				SelectObject(isolated, isolated_previous);
			if (isolated_bitmap != nullptr)
				DeleteObject(isolated_bitmap);
			if (isolated != nullptr)
				DeleteDC(isolated);
		}
		else
		{
			SetStretchBltMode(memory, HALFTONE);
			copied = StretchBlt(memory, 0, 0, width, height, screen, bounds.left, bounds.top, source_width, source_height, SRCCOPY | CAPTUREBLT);
		}
		if (copied)
		{
			auto* bytes = static_cast<unsigned char*>(pixels);
			for (std::size_t offset = 3; offset < static_cast<std::size_t>(width) * height * 4; offset += 4)
				bytes[offset] = 255;
			result.png = EncodePng(bitmap);
		}
		SelectObject(memory, previous);
		DeleteObject(bitmap);
		DeleteDC(memory);
		ReleaseDC(nullptr, screen);
		if (!copied || result.png.empty())
		{
			result.error = kind == "window" ? "The selected window could not be captured without exposing other windows." : "The active Windows desktop could not be captured as PNG.";
			return result;
		}
		result.width = width;
		result.height = height;
		result.desktop_x = bounds.left;
		result.desktop_y = bounds.top;
		result.desktop_width = source_width;
		result.desktop_height = source_height;
		if (kind == "window")
		{
			result.elements = AccessibilityElements(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(result.target_id)), bounds);
			for (Element& element : result.elements)
			{
				element.x = (element.x - bounds.left) * result.width / source_width;
				element.y = (element.y - bounds.top) * result.height / source_height;
				element.width *= static_cast<double>(result.width) / source_width;
				element.height *= static_cast<double>(result.height) / source_height;
			}
		}
		result.ok = true;
		return result;
	}

	bool ExecuteAction(const Action& action, const Capture& reference, const std::function<bool()>& cancelled, std::string* error_out, bool* input_applied_out)
	{
		if (input_applied_out != nullptr)
			*input_applied_out = false;
		bool cancellation_seen = false;
		bool foreground_lost = false;
		bool release_cleanup_failed = false;
		HWND window_target = nullptr;
		const auto interrupted = [&]()
		{
			if (!cancellation_seen && (!cancelled || !cancelled()))
				return false;
			cancellation_seen = true;
			if (error_out != nullptr)
				*error_out = "Computer action interrupted because UAM was paused or stopped.";
			return true;
		};
		const auto exact_window_is_foreground = [&]()
		{
			if (reference.target_kind != "window")
				return true;
			if (window_target == nullptr || !IsWindow(window_target))
				return false;
			DWORD process_id = 0;
			GetWindowThreadProcessId(window_target, &process_id);
			return process_id == reference.process_id && GetForegroundWindow() == window_target;
		};
		const auto verify_input_target = [&]()
		{
			if (exact_window_is_foreground())
				return true;
			foreground_lost = true;
			if (error_out != nullptr)
				*error_out = "The selected window lost exact foreground input focus.";
			return false;
		};
		const auto set_action_error = [&](const std::string& message)
		{
			if (!foreground_lost && !release_cleanup_failed && error_out != nullptr)
				*error_out = message;
		};
		const auto send_one = [&](INPUT input, bool cleanup, bool semantic_effect)
		{
			if (!cleanup && !verify_input_target())
				return false;
			const UINT sent = SendInput(1, &input, sizeof(INPUT));
			if (sent > 0 && semantic_effect && input_applied_out != nullptr)
				*input_applied_out = true;
			return sent == 1;
		};
		const auto send_batch = [&](std::vector<INPUT>& inputs, bool cleanup, bool semantic_effect)
		{
			if (!cleanup && !verify_input_target())
				return false;
			return Send(inputs, semantic_effect ? input_applied_out : nullptr);
		};
		const auto send_cleanup_batch = [&](std::vector<INPUT>& inputs)
		{
			for (int attempt = 0; attempt < 3; ++attempt)
			{
				if (send_batch(inputs, true, false))
					return true;
			}
			return false;
		};
		const auto release_requested_button = [&](DWORD up, bool& button_down, bool verify_target)
		{
			if (!button_down)
				return !verify_target || verify_input_target();
			const bool target_ready = !verify_target || verify_input_target();
			bool released = false;
			for (int attempt = 0; attempt < 3 && !released; ++attempt)
				released = send_one(MouseInput(up), true, false);
			if (released)
				button_down = false;
			else
			{
				release_cleanup_failed = true;
				if (error_out != nullptr)
					*error_out = "Windows could not release the requested mouse button after three attempts; it may remain held.";
			}
			return target_ready && released;
		};
		if (interrupted())
			return false;

		if (reference.target_kind == "window")
		{
			window_target = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(reference.target_id));
			DWORD process_id = 0;
			if (IsWindow(window_target))
				GetWindowThreadProcessId(window_target, &process_id);
			if (!IsWindow(window_target) || process_id != reference.process_id)
			{
				if (error_out != nullptr)
					*error_out = "Windows requires the selected window on the foreground desktop.";
				return false;
			}
			if (GetForegroundWindow() != window_target)
				(void)SetForegroundWindow(window_target);
			if (!exact_window_is_foreground())
			{
				if (error_out != nullptr)
					*error_out = "Windows could not verify the selected window as the foreground input target.";
				return false;
			}
		}
		if (interrupted())
			return false;
		ShowVirtualCursor(action, reference);
		if (interrupted())
			return false;

		if (action.kind == "move" || action.kind == "click")
		{
			const POINT point = DesktopPoint(action, reference);
			if (!send_one(DesktopMoveInput(point), false, action.kind == "move"))
			{
				set_action_error("Windows rejected the mouse move.");
				return false;
			}
			if (action.kind == "move")
				return true;
			if (interrupted())
				return false;

			DWORD down = MOUSEEVENTF_LEFTDOWN;
			DWORD up = MOUSEEVENTF_LEFTUP;
			if (action.button == "right")
			{
				down = MOUSEEVENTF_RIGHTDOWN;
				up = MOUSEEVENTF_RIGHTUP;
			}
			else if (action.button == "middle")
			{
				down = MOUSEEVENTF_MIDDLEDOWN;
				up = MOUSEEVENTF_MIDDLEUP;
			}
			bool button_down = false;
			for (int click = 0; click < action.click_count; ++click)
			{
				if (interrupted())
					return false;
				if (!send_one(MouseInput(down), false, true))
				{
					set_action_error("Windows rejected the mouse button press.");
					return false;
				}
				button_down = true;
				if (interrupted())
				{
					(void)release_requested_button(up, button_down, false);
					return false;
				}
				if (!release_requested_button(up, button_down, true))
				{
					set_action_error("Windows rejected the mouse button release.");
					return false;
				}
			}
			return true;
		}
		if (action.kind == "drag")
		{
			Action end_action = action;
			end_action.x = action.end_x;
			end_action.y = action.end_y;
			DWORD down = MOUSEEVENTF_LEFTDOWN;
			DWORD up = MOUSEEVENTF_LEFTUP;
			if (action.button == "right")
			{
				down = MOUSEEVENTF_RIGHTDOWN;
				up = MOUSEEVENTF_RIGHTUP;
			}
			else if (action.button == "middle")
			{
				down = MOUSEEVENTF_MIDDLEDOWN;
				up = MOUSEEVENTF_MIDDLEUP;
			}
			bool button_down = false;
			if (!send_one(DesktopMoveInput(DesktopPoint(action, reference)), false, false))
			{
				set_action_error("Windows rejected the drag start.");
				return false;
			}
			if (interrupted())
				return false;
			if (!send_one(MouseInput(down), false, true))
			{
				set_action_error("Windows rejected the drag button press.");
				return false;
			}
			button_down = true;
			if (interrupted())
			{
				(void)release_requested_button(up, button_down, false);
				return false;
			}
			if (!send_one(DesktopMoveInput(DesktopPoint(end_action, reference)), false, false))
			{
				(void)release_requested_button(up, button_down, false);
				set_action_error("Windows rejected the drag movement; the requested button was released.");
				return false;
			}
			if (interrupted())
			{
				(void)release_requested_button(up, button_down, false);
				return false;
			}
			if (!release_requested_button(up, button_down, true))
			{
				set_action_error("Windows rejected the drag release.");
				return false;
			}
			return true;
		}
		if (action.kind == "scroll")
		{
			if (!send_one(DesktopMoveInput(DesktopPoint(action, reference)), false, false))
			{
				set_action_error("Windows rejected the scroll position.");
				return false;
			}
			if (interrupted())
				return false;
			if (action.delta_y != 0 && !send_one(MouseInput(MOUSEEVENTF_WHEEL, static_cast<LONG>(std::llround(action.delta_y))), false, true))
			{
				set_action_error("Windows rejected the vertical scroll action.");
				return false;
			}
			if (interrupted())
				return false;
			if (action.delta_x != 0 && !send_one(MouseInput(MOUSEEVENTF_HWHEEL, static_cast<LONG>(std::llround(action.delta_x))), false, true))
			{
				set_action_error("Windows rejected the horizontal scroll action.");
				return false;
			}
			return true;
		}
		if (action.kind == "type")
		{
			const std::wstring text = Wide(action.text);
			if (text.empty() && !action.text.empty())
			{
				if (error_out != nullptr)
					*error_out = "Text is not valid UTF-8.";
				return false;
			}
			constexpr std::size_t kTextChunkSize = 20;
			for (std::size_t offset = 0; offset < text.size(); offset += kTextChunkSize)
			{
				if (interrupted())
					return false;
				const std::size_t count = std::min(kTextChunkSize, text.size() - offset);
				std::vector<INPUT> inputs;
				std::vector<INPUT> releases;
				inputs.reserve(count * 2);
				releases.reserve(count);
				for (std::size_t index = 0; index < count; ++index)
				{
					INPUT down{};
					down.type = INPUT_KEYBOARD;
					down.ki.wScan = text[offset + index];
					down.ki.dwFlags = KEYEVENTF_UNICODE;
					INPUT up = down;
					up.ki.dwFlags |= KEYEVENTF_KEYUP;
					inputs.push_back(down);
					inputs.push_back(up);
					releases.push_back(up);
				}
				if (!send_batch(inputs, false, true))
				{
					if (!send_cleanup_batch(releases))
					{
						release_cleanup_failed = true;
						if (error_out != nullptr)
							*error_out = "Windows could not release a rejected text-input chunk after three attempts; keys may remain held.";
					}
					else
						set_action_error("Windows rejected text input; the chunk's keys were released.");
					return false;
				}
			}
			return true;
		}
		if (action.kind == "hotkey")
		{
			std::vector<WORD> keys;
			for (const std::string& key : action.keys)
			{
				const auto resolved = VirtualKey(key);
				if (!resolved)
				{
					if (error_out != nullptr)
						*error_out = "Unsupported hotkey key: " + key;
					return false;
				}
				keys.push_back(*resolved);
			}
			std::vector<WORD> pressed;
			bool post_failed = false;
			for (WORD key : keys)
			{
				if (interrupted())
					break;
				INPUT input{};
				input.type = INPUT_KEYBOARD;
				input.ki.wVk = key;
				if (!send_one(input, false, true))
				{
					post_failed = true;
					break;
				}
				pressed.push_back(key);
				if (interrupted())
					break;
			}
			if (!cancellation_seen)
				(void)verify_input_target();
			bool release_failed = false;
			for (auto it = pressed.rbegin(); it != pressed.rend(); ++it)
			{
				(void)interrupted();
				INPUT input{};
				input.type = INPUT_KEYBOARD;
				input.ki.wVk = *it;
				input.ki.dwFlags = KEYEVENTF_KEYUP;
				bool released = false;
				for (int attempt = 0; attempt < 3 && !released; ++attempt)
					released = send_one(input, true, false);
				release_failed = !released || release_failed;
			}
			if (release_failed)
			{
				release_cleanup_failed = true;
				if (error_out != nullptr)
					*error_out = "Windows could not release every hotkey key after three attempts; keys may remain held.";
			}
			if (cancellation_seen)
				return false;
			if (foreground_lost)
				return false;
			if (post_failed || release_failed || pressed.size() != keys.size())
			{
				set_action_error("Windows rejected the hotkey; all pressed keys were released.");
				return false;
			}
			return true;
		}
		return action.kind == "wait" && !interrupted();
	}

	bool EnsureActionPermission(std::string*)
	{
		return true;
	}

	bool ConfirmComputerUse(const std::string& message)
	{
		HWND previous = GetForegroundWindow();
		const std::wstring text = Wide(message);
		const int result = MessageBoxW(nullptr, text.c_str(), L"Allow UAM computer control", MB_YESNO | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND | MB_DEFBUTTON2);
		if (previous != nullptr)
			SetForegroundWindow(previous);
		return result == IDYES;
	}

	int RunWithUi(const std::function<int()>& work)
	{
		return work();
	}
} // namespace uam::computer_use
