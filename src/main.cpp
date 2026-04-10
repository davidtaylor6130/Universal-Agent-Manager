// UAM — CEF multiprocess entry point.
//
// CefExecuteProcess() must be called first so that CEF can dispatch renderer,
// GPU, and utility subprocess invocations before the main process continues.

#include "app/application.h"
#include "cef/uam_cef_app.h"

#include "include/cef_app.h"

#if defined(_WIN32)
#include <windows.h>

int WINAPI wWinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/, LPWSTR /*lpCmdLine*/, int /*nCmdShow*/)
{
	CefMainArgs main_args(GetModuleHandle(nullptr));

	// Run CEF sub-process entry point — returns >= 0 for sub-processes.
	auto cef_app = CefRefPtr<UamCefApp>(new UamCefApp());
	const int exit_code = CefExecuteProcess(main_args, cef_app.get(), nullptr);
	if (exit_code >= 0)
		return exit_code;

	Application application;
	return application.Run(main_args);
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
	return application.Run(main_args);
}

#endif
