// UAM — CEF multiprocess entry point.
//
// CefExecuteProcess() must be called first so that CEF can dispatch renderer,
// GPU, and utility subprocess invocations before the main process continues.

#include "app/application.h"
#include "app/uam_control_service.h"
#include "cef/uam_cef_app.h"
#include "computer_use/computer_use_mcp_server.h"

#include "include/cef_app.h"

#include <cstdio>

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
	if (uam::computer_use::IsMcpServerInvocation(arguments))
		return uam::computer_use::RunMcpServer(arguments);

	CefMainArgs main_args(GetModuleHandle(nullptr));

	// Run CEF sub-process entry point — returns >= 0 for sub-processes.
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
	if (const std::optional<int> watchdog_exit = uam::platform::RunMacParentDeathWatchdogIfRequested(argc, argv); watchdog_exit.has_value())
	{
		return *watchdog_exit;
	}
#endif
	const std::vector<std::string> arguments(argv, argv + argc);
	if (uam::computer_use::IsMcpServerInvocation(arguments))
		return uam::computer_use::RunMcpServer(arguments);

	CefMainArgs main_args(argc, argv);

	// Run CEF sub-process entry point — returns >= 0 for sub-processes.
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
