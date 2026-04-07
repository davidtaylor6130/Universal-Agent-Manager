#include "platform_services.h"

#if defined(_WIN32)
#include "platform_services_windows_impl.h"
#elif defined(__APPLE__)
#include "platform_services_macos_impl.h"
#else
#error "PlatformServicesFactory supports only Windows and macOS."
#endif

PlatformServices& PlatformServicesFactory::Instance()
{
	return CreatePlatformServices();
}
