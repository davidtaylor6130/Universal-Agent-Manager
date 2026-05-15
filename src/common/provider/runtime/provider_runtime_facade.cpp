#include "common/provider/provider_runtime.h"

#include "common/provider/runtime/provider_runtime_internal.h"

namespace
{
	bool RuntimeEnabledForProfile(const ProviderProfile& profile, const IProviderRuntime& runtime)
	{
		return runtime.IsEnabled() && uam::provider_runtime_internal::RuntimeConfigurationError(profile, runtime).empty();
	}

	std::string DisabledReasonForRuntime(const IProviderRuntime& runtime)
	{
		const char* reason = runtime.DisabledReason();
		return reason == nullptr ? std::string() : std::string(reason);
	}

	bool RuntimeProfileBoolean(const ProviderProfile& profile, bool (IProviderRuntime::*predicate)(const ProviderProfile&) const)
	{
		const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(profile);
		return (runtime.*predicate)(profile);
	}
} // namespace

std::string ProviderRuntime::BuildPrompt(const ProviderProfile& profile, std::string_view user_prompt, const std::vector<std::string>& files)
{
	return ProviderRuntimeRegistry::Resolve(profile).BuildPrompt(profile, user_prompt, files);
}

std::string ProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, const std::vector<std::string>& files, const std::string& resume_session_id)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(profile);
	if (!RuntimeEnabledForProfile(profile, runtime))
	{
		return "";
	}

	return runtime.BuildCommand(profile, settings, prompt, files, resume_session_id);
}

std::vector<std::string> ProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(profile);
	if (!RuntimeEnabledForProfile(profile, runtime))
	{
		return {};
	}

	return runtime.BuildInteractiveArgv(profile, chat, settings);
}

bool ProviderRuntime::IsRuntimeEnabled(const ProviderProfile& profile)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(profile);
	return RuntimeEnabledForProfile(profile, runtime);
}

bool ProviderRuntime::IsRuntimeEnabled(std::string_view provider_id)
{
	return ProviderRuntimeRegistry::ResolveById(provider_id).IsEnabled();
}

std::string ProviderRuntime::DisabledReason(const ProviderProfile& profile)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::Resolve(profile);

	if (runtime.IsEnabled())
	{
		const std::string config_error = uam::provider_runtime_internal::RuntimeConfigurationError(profile, runtime);

		if (!config_error.empty())
		{
			return config_error;
		}

		return "";
	}

	return DisabledReasonForRuntime(runtime);
}

std::string ProviderRuntime::DisabledReason(std::string_view provider_id)
{
	const IProviderRuntime& runtime = ProviderRuntimeRegistry::ResolveById(provider_id);

	if (runtime.IsEnabled())
	{
		return "";
	}

	const std::string reason = DisabledReasonForRuntime(runtime);

	if (!reason.empty())
	{
		return reason;
	}

	if (ProviderRuntimeRegistry::IsEnabledRuntimeId(provider_id))
	{
		return "Runtime '" + std::string(provider_id) + "' is disabled in this build.";
	}

	return "";
}

MessageRole ProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type)
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
	return RuntimeProfileBoolean(profile, &IProviderRuntime::UsesNativeOverlayHistory);
}

bool ProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile& profile)
{
	return RuntimeProfileBoolean(profile, &IProviderRuntime::SupportsGeminiJsonHistory);
}

bool ProviderRuntime::UsesLocalHistory(const ProviderProfile& profile)
{
	return RuntimeProfileBoolean(profile, &IProviderRuntime::UsesLocalHistory);
}

bool ProviderRuntime::UsesInternalEngine(const ProviderProfile& profile)
{
	return RuntimeProfileBoolean(profile, &IProviderRuntime::UsesInternalEngine);
}

bool ProviderRuntime::UsesCliOutput(const ProviderProfile& profile)
{
	return RuntimeProfileBoolean(profile, &IProviderRuntime::UsesCliOutput);
}

bool ProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile& profile)
{
	return RuntimeProfileBoolean(profile, &IProviderRuntime::UsesGeminiPathBootstrap);
}

bool ProviderRuntime::RebuildNativeSessionFile(const ProviderProfile& profile, const ChatSession& chat, const std::filesystem::path& workspace_path)
{
	return ProviderRuntimeRegistry::Resolve(profile).RebuildNativeSessionFile(profile, chat, workspace_path);
}

ProviderDiscoveryResult ProviderRuntime::DiscoverChatSources(const ProviderProfile& profile)
{
	return ProviderRuntimeRegistry::Resolve(profile).DiscoverChatSources(profile);
}
