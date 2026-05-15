#include "common/provider/copilot/cli/copilot_cli_provider_runtime.h"

#include "common/config/approval_modes.h"
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/utils/string_utils.h"

#include <string_view>

namespace
{
	constexpr const char* kCopilotAllowAllFlag = "--allow-all";

	std::vector<std::string> CopilotFlagsFromSettings(const AppSettings& settings)
	{
		return uam::provider_runtime_internal::BuildProviderFlagsArgv(settings, "");
	}

	void AppendCopilotModeArgs(std::vector<std::string>& argv, std::string_view raw_model_id, std::string_view raw_approval_mode, const AppSettings& settings)
	{
		uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", raw_model_id);

		const std::string approval_mode = uam::strings::Trim(raw_approval_mode);
		if (approval_mode == uam::approval_modes::kPlanApprovalMode)
		{
			argv.push_back("--plan");
		}
		else if (approval_mode == uam::approval_modes::kLegacyYoloApprovalMode || settings.provider_yolo_mode)
		{
			argv.push_back(kCopilotAllowAllFlag);
		}
	}

	void AppendCopilotBatchModeArgs(std::vector<std::string>& argv, const AppSettings& settings)
	{
		if (settings.provider_yolo_mode)
		{
			argv.push_back(kCopilotAllowAllFlag);
		}
	}
} // namespace

const char* CopilotCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kCopilotCli;
}

bool CopilotCliProviderRuntime::IsEnabled() const
{
	return true;
}

const char* CopilotCliProviderRuntime::DisabledReason() const
{
	return "";
}

std::string CopilotCliProviderRuntime::BuildPrompt(const ProviderProfile&, std::string_view user_prompt, const std::vector<std::string>& files) const
{
	return uam::provider_runtime_internal::BuildPrompt(user_prompt, files);
}

std::string CopilotCliProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, const std::vector<std::string>& files, const std::string& resume_session_id) const
{
	const AppSettings provider_settings = uam::provider_runtime_internal::MergeProviderSettings(profile, settings);
	std::vector<std::string> argv = {"copilot", "-p"};
	uam::provider_runtime_internal::AppendResumeArgs(argv, profile, resume_session_id);

	AppendCopilotBatchModeArgs(argv, provider_settings);
	uam::provider_runtime_internal::AppendArgs(argv, CopilotFlagsFromSettings(provider_settings));
	argv.push_back(BuildPrompt(profile, prompt, files));
	return uam::provider_runtime_internal::JoinShellEscapedArgs(argv);
}

std::vector<std::string> CopilotCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	const AppSettings provider_settings = uam::provider_runtime_internal::MergeProviderSettings(profile, settings);
	std::vector<std::string> argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "copilot");

	uam::provider_runtime_internal::AppendResumeArgs(argv, profile, chat.native_session_id);

	AppendCopilotModeArgs(argv, chat.model_id, chat.approval_mode, provider_settings);
	uam::provider_runtime_internal::AppendArgs(argv, CopilotFlagsFromSettings(provider_settings));
	return argv;
}

MessageRole CopilotCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const
{
	return uam::provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> CopilotCliProviderRuntime::LoadHistory(const ProviderProfile&, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	return uam::provider_runtime_internal::LoadLocalChats(data_root);
}

bool CopilotCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return uam::provider_runtime_internal::SaveLocalChat(data_root, chat);
}

bool CopilotCliProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return false;
}

bool CopilotCliProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return false;
}

bool CopilotCliProviderRuntime::UsesLocalHistory(const ProviderProfile&) const
{
	return true;
}

bool CopilotCliProviderRuntime::UsesInternalEngine(const ProviderProfile&) const
{
	return false;
}

bool CopilotCliProviderRuntime::UsesCliOutput(const ProviderProfile&) const
{
	return true;
}

bool CopilotCliProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile&) const
{
	return false;
}

const IProviderRuntime& GetCopilotCliProviderRuntime()
{
	static const CopilotCliProviderRuntime runtime;
	return runtime;
}
