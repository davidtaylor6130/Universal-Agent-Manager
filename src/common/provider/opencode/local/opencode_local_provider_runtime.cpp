#include "common/provider/opencode/local/opencode_local_provider_runtime.h"

#include "common/provider/runtime/provider_runtime_internal.h"

OpenCodeLocalProviderRuntime::OpenCodeLocalProviderRuntime() :
    OpenCodeBaseProviderRuntime("opencode-local",
                                UAM_ENABLE_RUNTIME_OPENCODE_LOCAL != 0,
                                "Runtime 'opencode-local' is disabled in this build (UAM_ENABLE_RUNTIME_OPENCODE_LOCAL=OFF).")
{
}

const IProviderRuntime& GetOpenCodeLocalProviderRuntime()
{
	static const OpenCodeLocalProviderRuntime runtime;
	return runtime;
}
