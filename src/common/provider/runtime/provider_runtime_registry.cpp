#include "common/provider/provider_runtime.h"

#include "common/provider/runtime/provider_build_config.h"
#include "common/provider/runtime/provider_runtime_internal.h"

#include "common/provider/custom/unknown_provider_runtime.h"
#if UAM_ENABLE_RUNTIME_GEMINI_CLI
#include "common/provider/gemini/cli/gemini_cli_provider_runtime.h"
#endif

const IProviderRuntime& ProviderRuntimeRegistry::Resolve(const ProviderProfile& profile)
{
	return ResolveById(profile.id);
}

const IProviderRuntime& ProviderRuntimeRegistry::ResolveById(const std::string& provider_id)
{
	const std::string lower_id = provider_runtime_internal::LowerAscii(provider_id);

	if (lower_id == "gemini" || lower_id == "gemini-structured" || lower_id == "gemini-cli")
	{
		return GetGeminiCliProviderRuntime();
	}

	return GetUnknownProviderRuntime();
}

bool ProviderRuntimeRegistry::IsKnownRuntimeId(const std::string& provider_id)
{
	const std::string lower_id = provider_runtime_internal::LowerAscii(provider_id);
	return lower_id == "gemini" || lower_id == "gemini-structured" || lower_id == "gemini-cli";
}
