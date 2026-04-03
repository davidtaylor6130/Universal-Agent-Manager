#include "common/provider/opencode/cli/opencode_cli_provider_runtime.h"

#include "common/provider/runtime/provider_runtime_internal.h"

OpenCodeCliProviderRuntime::OpenCodeCliProviderRuntime() :
    OpenCodeBaseProviderRuntime("opencode-cli",
                                UAM_ENABLE_RUNTIME_OPENCODE_CLI != 0,
                                "Runtime 'opencode-cli' is disabled in this build (UAM_ENABLE_RUNTIME_OPENCODE_CLI=OFF).")
{
}

const IProviderRuntime& GetOpenCodeCliProviderRuntime()
{
	static const OpenCodeCliProviderRuntime runtime;
	return runtime;
}
