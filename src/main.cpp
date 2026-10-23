#include <nlohmann/json.hpp>
#include <optional>
#include <algorithm>
#include "common/discovery/management_discovery.h"
#include "common/config/build_features.h"
// UAM — CEF multiprocess entry point.
//
// CEF subprocesses and service modes bypass GUI crash supervision. The GUI child
// dispatches CEF before initializing the macOS application or running the app.

#include "app/application.h"
#include "common/platform/app_crash_supervisor.h"
#include "app/uam_control_service.h"
#include "cef/uam_cef_app.h"
#include "computer_use/computer_use_mcp_server.h"

#include "include/cef_app.h"

#include <cstdio>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace
{
/// <summary>Write only explicit discovery CLI output, preserving redirected Windows handles.</summary>
bool WriteDiscoveryJson(const std::string& text)
{
#if defined(_WIN32)
	HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
	bool owned_output = false;
	if (!output || output == INVALID_HANDLE_VALUE || GetFileType(output) == FILE_TYPE_UNKNOWN)
	{
		if (!AttachConsole(ATTACH_PARENT_PROCESS) && GetLastError() != ERROR_ACCESS_DENIED) return false;
		output = GetStdHandle(STD_OUTPUT_HANDLE);
		if (!output || output == INVALID_HANDLE_VALUE || GetFileType(output) == FILE_TYPE_UNKNOWN)
		{
			output = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
			owned_output = true;
		}
	}
	if (!output || output == INVALID_HANDLE_VALUE) return false;
	std::size_t offset = 0;
	while (offset < text.size())
	{
		DWORD written = 0;
		if (!WriteFile(output, text.data() + offset, static_cast<DWORD>(text.size() - offset), &written, nullptr) || written == 0)
		{
			if (owned_output) CloseHandle(output);
			return false;
		}
		offset += written;
	}
	if (owned_output) CloseHandle(output);
	return true;
#else
	return std::fwrite(text.data(), 1, text.size(), stdout) == text.size();
#endif
}

std::optional<int> DiscoveryCommand(const std::vector<std::string>& arguments)
{
	if (std::find(arguments.begin(), arguments.end(), "--discover-management-nodes") == arguments.end()) return std::nullopt;
	std::string error;
	const std::vector<uam::discovery::ManagementHint> hints = uam::discovery::BrowseManagementNodes(
		std::find(arguments.begin(), arguments.end(), "--discovery-local-only") != arguments.end(), error);
	nlohmann::json result = nlohmann::json::array();
	for (const uam::discovery::ManagementHint& hint : hints)
	{
		std::string host = hint.host;
		if (!host.empty() && host.back() == '.') host.pop_back();
		result.push_back({{"name", hint.name}, {"origin", "https://" + host + (hint.port == 443 ? "" : ":" + std::to_string(hint.port))}, {"trusted", false}});
	}
	if (!WriteDiscoveryJson(nlohmann::json({{"nodes", result}, {"error", error}}).dump(-1, ' ', true, nlohmann::json::error_handler_t::replace) + "\n")) return 1;
	return error.empty() ? 0 : 1;
}
}

#if defined(__APPLE__)
#include "common/platform/platform_application_macos.h"
#endif

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace
{
	std::string Utf8(const wchar_t* value)
	{
		if (value == nullptr || *value == L'\0') return {};
		const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
		std::string result(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
		if (size > 0)
		{
			WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
			result.pop_back();
		}
		return result;
	}

	std::vector<std::string> LaunchArguments()
	{
		int count = 0;
		LPWSTR* values = CommandLineToArgvW(GetCommandLineW(), &count);
		std::vector<std::string> result;
		for (int index = 0; values != nullptr && index < count; ++index) result.push_back(Utf8(values[index]));
		if (values != nullptr) LocalFree(values);
		return result;
	}
}

int WINAPI wWinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/, LPWSTR /*lpCmdLine*/, int /*nCmdShow*/)
{
	const std::vector<std::string> arguments = LaunchArguments();
	if (const std::optional<int> discovery_exit = DiscoveryCommand(arguments)) return *discovery_exit;
#if UAM_ENABLE_COMPUTER_USE
	if (uam::computer_use::IsMcpServerInvocation(arguments))
		return uam::computer_use::RunMcpServer(arguments);
#endif

	CefMainArgs main_args(GetModuleHandle(nullptr));

	// Run CEF sub-process entry point — returns >= 0 for sub-processes.
	if (!uam::UamControlService::IsStdioServerInvocation(arguments) &&
	    std::find(arguments.begin(), arguments.end(), "--management-node") == arguments.end())
	{
		if (const std::optional<int> supervisor_exit = uam::platform::RunAppCrashSupervisor(arguments))
			return *supervisor_exit;
	}
	auto cef_app = CefRefPtr<UamCefApp>(new UamCefApp());
	const int exit_code = CefExecuteProcess(main_args, cef_app.get(), nullptr);
	if (exit_code >= 0)
		return exit_code;
	if (uam::UamControlService::IsStdioServerInvocation(arguments))
		return uam::UamControlService::RunStdioServerFromEnvironment();

	Application application;
	return application.Run(main_args, arguments);
}

#else

int main(int argc, char* argv[])
{
#if defined(__APPLE__)
	if (const std::optional<int> terminal_exit = uam::platform::RunMacTerminalChildIfRequested(argc, argv); terminal_exit.has_value())
	{
		return *terminal_exit;
	}
	if (const std::optional<int> watchdog_exit = uam::platform::RunMacParentDeathWatchdogIfRequested(argc, argv); watchdog_exit.has_value())
	{
		return *watchdog_exit;
	}
#endif
	const std::vector<std::string> arguments(argv, argv + argc);
	if (const std::optional<int> discovery_exit = DiscoveryCommand(arguments)) return *discovery_exit;
#if UAM_ENABLE_COMPUTER_USE
	if (uam::computer_use::IsMcpServerInvocation(arguments))
		return uam::computer_use::RunMcpServer(arguments);
#endif

	CefMainArgs main_args(argc, argv);

	// Run CEF sub-process entry point — returns >= 0 for sub-processes.
	if (!uam::UamControlService::IsStdioServerInvocation(arguments) &&
	    std::find(arguments.begin(), arguments.end(), "--management-node") == arguments.end())
	{
		if (const std::optional<int> supervisor_exit = uam::platform::RunAppCrashSupervisor(arguments))
			return *supervisor_exit;
	}
	auto cef_app = CefRefPtr<UamCefApp>(new UamCefApp());
	const int exit_code = CefExecuteProcess(main_args, cef_app.get(), nullptr);
	if (exit_code >= 0)
		return exit_code;
	if (uam::UamControlService::IsStdioServerInvocation(arguments))
		return uam::UamControlService::RunStdioServerFromEnvironment();

#if defined(__APPLE__)
	if (!uam::platform::InitializeMacApplication())
	{
		std::fprintf(stderr, "Failed to initialize the CEF-compatible macOS application.\n");
		return 1;
	}
#endif

	Application application;
	return application.Run(main_args, arguments);
}

#endif
