#pragma once

#if defined(_WIN32)

#include "platform_services.h"

PlatformServices& CreatePlatformServices();

#endif // defined(_WIN32)
