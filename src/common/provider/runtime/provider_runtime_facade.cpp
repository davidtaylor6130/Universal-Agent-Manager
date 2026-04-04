#include "common/provider/provider_runtime.h"

#include "common/provider/runtime/provider_runtime_internal.h"

std::string ProviderRuntime::BuildPrompt(const ProviderProfile& profile, const std::string& user_prompt, const std::vector<std::string>& files)
{
	return ProviderRuntimeRegistry::Resolve(profile).BuildPrompt(profile, user_prompt, files);
}

std::string ProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, const std::string& prompt, const std::vector<std::string>& files, const std::string& resume_session_id)
{
	if (!IsRuntimeEnabled(profile))
	{
		return "";
	}

	return ProviderRuntimeRegistry::Resolve(profile).BuildCommand(profile, settings, prompt, files, resume_session_id);
}

std::vector<std::string> ProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings)
{
	if (!IsRuntimeEnabled(profile))
	{
		return {};
	}

	return ProviderRuntimeRegistry::Resolve(profile).BuildInteractiveArgv(profile, chat, settings);
}

bool ProviderRuntime::IsRuntimeEnabled(const ProviderProfile& profile)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(profile);

	if (!runtime.IsEnabled())
	{
		return false;
	}

	return provider_runtime_internal::RuntimeConfigurationError(profile, runtime).empty();
}

bool ProviderRuntime::IsRuntimeEnabled(const std::string& provider_id)
{
	return ProviderRuntimeRegistry::ResolveById(provider_id).IsEnabled();
}

std::string ProviderRuntime::DisabledReason(const ProviderProfile& profile)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(profile);

	if (runtime.IsEnabled())
	{
		const std::string config_error = provider_runtime_internal::RuntimeConfigurationError(profile, runtime);

		if (!config_error.empty())
		{
			return config_error;
		}

		return "";
	}

	const char* reason = runtime.DisabledReason();
	return reason == nullptr ? std::string() : std::string(reason);
}

std::string ProviderRuntime::DisabledReason(const std::string& provider_id)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(provider_id);

	if (runtime.IsEnabled())
	{
		return "";
	}

	const char* reason = runtime.DisabledReason();

	if (reason != nullptr && *reason != '\0')
	{
		return reason;
	}

	if (ProviderRuntimeRegistry::IsKnownRuntimeId(provider_id))
	{
		return "Runtime '" + provider_id + "' is disabled in this build.";
	}

	return "";
}

MessageRole ProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, const std::string& native_type)
{
	return ProviderRuntimeRegistry::Resolve(profile).RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> ProviderRuntime::LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options)
{
	return ProviderRuntimeRegistry::Resolve(profile).LoadHistory(profile, data_root, native_history_chats_dir, options);
}

bool ProviderRuntime::SaveHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const ChatSession& chat)
{
	return ProviderRuntimeRegistry::Resolve(profile).SaveHistory(profile, data_root, chat);
}

bool ProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).UsesNativeOverlayHistory(profile);
}

bool ProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).SupportsGeminiJsonHistory(profile);
}

bool ProviderRuntime::UsesLocalHistory(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).UsesLocalHistory(profile);
}

bool ProviderRuntime::UsesInternalEngine(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).UsesInternalEngine(profile);
}

bool ProviderRuntime::UsesCliOutput(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).UsesCliOutput(profile);
}

bool ProviderRuntime::UsesStructuredOutput(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).UsesStructuredOutput(profile);
}

bool ProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).UsesGeminiPathBootstrap(profile);
}

bool ProviderRuntime::PortSessionToWorkspace(const ProviderProfile& profile, const std::string& sessionId, const std::filesystem::path& fromWorkspace, const std::filesystem::path& toWorkspace)
{
	return ProviderRuntimeRegistry::Resolve(profile).PortSessionToWorkspace(profile, sessionId, fromWorkspace, toWorkspace);
}

ProviderDiscoveryResult ProviderRuntime::DiscoverChatSources(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).DiscoverChatSources(profile);
}
