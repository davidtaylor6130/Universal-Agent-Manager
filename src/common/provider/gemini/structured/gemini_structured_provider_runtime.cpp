#include "common/provider/gemini/structured/gemini_structured_provider_runtime.h"

#include "common/provider/runtime/provider_runtime_internal.h"

GeminiStructuredProviderRuntime::GeminiStructuredProviderRuntime() :
    GeminiBaseProviderRuntime("gemini-structured",
                              UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED != 0,
                              "Runtime 'gemini-structured' is disabled in this build (UAM_ENABLE_RUNTIME_GEMINI_STRUCTURED=OFF).",
                              false)
{
}

const IProviderRuntime& GetGeminiStructuredProviderRuntime()
{
	static const GeminiStructuredProviderRuntime runtime;
	return runtime;
}
