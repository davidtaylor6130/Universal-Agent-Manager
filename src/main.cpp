// UAM — CEF multiprocess entry point.
//
// CefExecuteProcess() must be called first so that CEF can dispatch renderer,
// GPU, and utility subprocess invocations before the main process continues.

#include "app/application.h"
#include "cef/uam_cef_app.h"

#include "include/cef_app.h"

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
	CefMainArgs main_args(GetModuleHandle(nullptr));

	// Run CEF sub-process entry point — returns >= 0 for sub-processes.
	auto cef_app = CefRefPtr<UamCefApp>(new UamCefApp());
	const int exit_code = CefExecuteProcess(main_args, cef_app.get(), nullptr);
	if (exit_code >= 0)
		return exit_code;

	Application application;
	return application.Run(main_args, LaunchArguments());
}

#else

int main(int argc, char* argv[])
{
	CefMainArgs main_args(argc, argv);

	// Run CEF sub-process entry point — returns >= 0 for sub-processes.
	auto cef_app = CefRefPtr<UamCefApp>(new UamCefApp());
	const int exit_code = CefExecuteProcess(main_args, cef_app.get(), nullptr);
	if (exit_code >= 0)
		return exit_code;

	Application application;
	return application.Run(main_args, std::vector<std::string>(argv, argv + argc));
}

#endif
