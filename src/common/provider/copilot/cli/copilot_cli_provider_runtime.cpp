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
		return uam::provider_runtime_internal::BuildProviderFlagsArgv(settings, kCopilotAllowAllFlag);
	}

	void AppendCopilotModeArgs(std::vector<std::string>& argv, std::string_view raw_model_id, std::string_view raw_approval_mode)
	{
		uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", raw_model_id);

		const std::string approval_mode = uam::strings::Trim(raw_approval_mode);
		if (approval_mode == uam::approval_modes::kPlanApprovalMode)
		{
			argv.push_back("--plan");
		}
		else if (approval_mode == uam::approval_modes::kLegacyYoloApprovalMode)
		{
			argv.push_back(kCopilotAllowAllFlag);
		}
	}
} // namespace

const char* CopilotCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kCopilotCli;
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

	AppendCopilotModeArgs(argv, chat.model_id, chat.approval_mode);
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


std::vector<std::string> CopilotCliProviderRuntime::BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const
{
	std::vector<std::string> argv = {"copilot", "-p"};
	const std::vector<std::string> flags = uam::provider_runtime_internal::ProviderWorkerFlags(profile, settings);
	uam::provider_runtime_internal::AppendArgs(argv, flags);

	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", model_id);

	argv.push_back(std::string(prompt));
	return argv;
}

std::vector<std::string> CopilotCliProviderRuntime::BuildStructuredLaunchArgv(const ProviderProfile&, const ChatSession&) const
{
	return {"copilot", "--acp", "--stdio"};
}

const IProviderRuntime& GetCopilotCliProviderRuntime()
{
	static const CopilotCliProviderRuntime runtime;
	return runtime;
}
