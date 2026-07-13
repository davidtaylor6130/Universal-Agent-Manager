#include "platform_services_windows_impl.h"

#if defined(_WIN32)

namespace uam::platform_windows_impl
{
IPlatformTerminalRuntime& GetWindowsTerminalRuntime();
IPlatformProcessService& GetWindowsProcessService();
IPlatformFileDialogService& GetWindowsFileDialogService();
IPlatformPathService& GetWindowsPathService();
IPlatformDictationService& GetWindowsDictationService();
} // namespace uam::platform_windows_impl

PlatformServices& CreatePlatformServices()
{
	static PlatformServices services{
	    uam::platform_windows_impl::GetWindowsTerminalRuntime(),
	    uam::platform_windows_impl::GetWindowsProcessService(),
	    uam::platform_windows_impl::GetWindowsFileDialogService(),
	    uam::platform_windows_impl::GetWindowsPathService(),
	    uam::platform_windows_impl::GetWindowsDictationService(),
	};
	return services;
}

#endif // defined(_WIN32)
