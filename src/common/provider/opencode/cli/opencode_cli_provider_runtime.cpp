#include "common/provider/opencode/cli/opencode_cli_provider_runtime.h"

#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"

namespace
{
	constexpr const char* kOpenCodeDangerouslySkipPermissionsFlag = "--dangerously-skip-permissions";

	AppSettings OpenCodeBatchCommandSettings(const ProviderProfile& profile, const AppSettings& settings)
	{
		return uam::provider_runtime_internal::MergeProviderSettingsWithCustomYoloFlag(profile, settings, kOpenCodeDangerouslySkipPermissionsFlag);
	}

	std::vector<std::string> OpenCodeFlagsFromSettings(const AppSettings& settings)
	{
		return uam::provider_runtime_internal::BuildProviderFlagsArgv(settings, kOpenCodeDangerouslySkipPermissionsFlag);
	}

} // namespace

const char* OpenCodeCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kOpenCodeCli;
}

bool OpenCodeCliProviderRuntime::IsEnabled() const
{
	return true;
}

const char* OpenCodeCliProviderRuntime::DisabledReason() const
{
	return "";
}

std::string OpenCodeCliProviderRuntime::BuildPrompt(const ProviderProfile&, std::string_view user_prompt, const std::vector<std::string>& files) const
{
	return uam::provider_runtime_internal::BuildPrompt(user_prompt, files);
}

std::string OpenCodeCliProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, const std::vector<std::string>& files, const std::string& resume_session_id) const
{
	const AppSettings provider_settings = OpenCodeBatchCommandSettings(profile, settings);

	std::vector<std::string> argv = {"opencode", "run"};
	uam::provider_runtime_internal::AppendResumeArgs(argv, profile, resume_session_id);

	uam::provider_runtime_internal::AppendArgs(argv, OpenCodeFlagsFromSettings(provider_settings));
	uam::provider_runtime_internal::AppendTrimmedOptionValues(argv, "--file", files);
	argv.push_back(BuildPrompt(profile, prompt, {}));
	return uam::provider_runtime_internal::JoinShellEscapedArgs(argv);
}

std::vector<std::string> OpenCodeCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	const AppSettings provider_settings = uam::provider_runtime_internal::MergeProviderSettings(profile, settings);
	std::vector<std::string> argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "opencode");

	uam::provider_runtime_internal::AppendResumeArgs(argv, profile, chat.native_session_id);

	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", chat.model_id);

	uam::provider_runtime_internal::AppendArgs(argv, OpenCodeFlagsFromSettings(provider_settings));
	return argv;
}

MessageRole OpenCodeCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const
{
	return uam::provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> OpenCodeCliProviderRuntime::LoadHistory(const ProviderProfile&, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	return uam::provider_runtime_internal::LoadLocalChats(data_root);
}

bool OpenCodeCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return uam::provider_runtime_internal::SaveLocalChat(data_root, chat);
}

bool OpenCodeCliProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return false;
}

bool OpenCodeCliProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return false;
}

bool OpenCodeCliProviderRuntime::UsesLocalHistory(const ProviderProfile&) const
{
	return true;
}

bool OpenCodeCliProviderRuntime::UsesInternalEngine(const ProviderProfile&) const
{
	return false;
}

bool OpenCodeCliProviderRuntime::UsesCliOutput(const ProviderProfile&) const
{
	return true;
}

bool OpenCodeCliProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile&) const
{
	return false;
}

const IProviderRuntime& GetOpenCodeCliProviderRuntime()
{
	static const OpenCodeCliProviderRuntime runtime;
	return runtime;
}
