#include "platform_services_macos_impl.h"

#if defined(__APPLE__)

namespace uam::platform_macos_impl
{
IPlatformTerminalRuntime& GetMacTerminalRuntime();
IPlatformProcessService& GetMacProcessService();
IPlatformFileDialogService& GetMacFileDialogService();
IPlatformPathService& GetMacPathService();
} // namespace uam::platform_macos_impl

PlatformServices& CreatePlatformServices()
{
	static PlatformServices services{
	    uam::platform_macos_impl::GetMacTerminalRuntime(),
	    uam::platform_macos_impl::GetMacProcessService(),
	    uam::platform_macos_impl::GetMacFileDialogService(),
	    uam::platform_macos_impl::GetMacPathService(),
	};
	return services;
}

#endif // defined(__APPLE__)
