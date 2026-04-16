#include "common/provider/provider_runtime.h"

#include "common/provider/runtime/provider_build_config.h"
#include "common/provider/runtime/provider_runtime_internal.h"

#include "common/provider/gemini/cli/gemini_cli_provider_runtime.h"

const IProviderRuntime& ProviderRuntimeRegistry::Resolve(const ProviderProfile& profile)
{
	return ResolveById(profile.id);
}

const IProviderRuntime& ProviderRuntimeRegistry::ResolveById(const std::string& provider_id)
{
	(void)provider_id;
	return GetGeminiCliProviderRuntime();
}

bool ProviderRuntimeRegistry::IsKnownRuntimeId(const std::string& provider_id)
{
	(void)provider_id;
	return true;
}
