#include "common/provider/gemini/cli/gemini_cli_provider_runtime.h"

#include "common/provider/runtime/provider_runtime_internal.h"

GeminiCliProviderRuntime::GeminiCliProviderRuntime() :
    GeminiBaseProviderRuntime("gemini-cli",
                              UAM_ENABLE_RUNTIME_GEMINI_CLI != 0,
                              "Runtime 'gemini-cli' is disabled in this build (UAM_ENABLE_RUNTIME_GEMINI_CLI=OFF).",
                              true)
{
}

const IProviderRuntime& GetGeminiCliProviderRuntime()
{
	static const GeminiCliProviderRuntime runtime;
	return runtime;
}
