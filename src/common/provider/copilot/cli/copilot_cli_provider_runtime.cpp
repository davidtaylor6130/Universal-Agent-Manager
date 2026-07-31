#include "common/provider/copilot/cli/copilot_cli_provider_runtime.h"

#include "common/config/approval_modes.h"
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/utils/string_utils.h"

#include <array>
#include <optional>
#include <string_view>

namespace
{
	constexpr const char* kCopilotAllowAllFlag = "--allow-all";
	constexpr auto kCopilotReasoningEfforts = std::to_array<std::string_view>({
	    "none",
	    "minimal",
	    "low",
	    "medium",
	    "high",
	    "xhigh",
	    "max",
	});

	std::vector<std::string> CopilotFlagsFromSettings(const AppSettings& settings)
	{
		return uam::provider_runtime_internal::BuildProviderFlagsArgv(settings, kCopilotAllowAllFlag);
	}

	void AppendCopilotModeArgs(std::vector<std::string>& argv, const ChatSession& chat)
	{
		uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", chat.model_id);

		const std::string approval_mode = uam::strings::Trim(chat.approval_mode);
		if (approval_mode == uam::approval_modes::kPlanApprovalMode)
		{
			argv.push_back("--plan");
		}
	}
} // namespace

std::string NormalizeCopilotReasoningEffort(std::string_view value)
{
	const std::optional<std::string_view> normalized = uam::strings::FindEqualIgnoreCase(kCopilotReasoningEfforts, uam::strings::TrimAsciiView(value));
	return normalized ? std::string(*normalized) : std::string();
}

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

	AppSettings provider_settings = uam::provider_runtime_internal::MergeProviderSettings(profile, settings);
	provider_settings.provider_yolo_mode = provider_settings.provider_yolo_mode || uam::strings::TrimAsciiView(chat.approval_mode) == uam::approval_modes::kLegacyYoloApprovalMode || uam::strings::TrimAsciiView(chat.command_safety_tier) == uam::approval_modes::kLegacyYoloApprovalMode;
	std::vector<std::string> argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "copilot");

	uam::provider_runtime_internal::AppendResumeArgs(argv, profile, chat.native_session_id);

	AppendCopilotModeArgs(argv, chat);
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

std::vector<std::string> CopilotCliProviderRuntime::BuildWorkerArgv(const ProviderProfile&, const AppSettings&, std::string_view prompt, std::string_view model_id) const
{
	std::vector<std::string> argv = {"copilot", "-p", std::string(prompt)};
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", model_id);
	uam::provider_runtime_internal::AppendArgs(argv, {
	                                                     "--no-auto-update",
	                                                     "--allow-all-tools",
	                                                     "--available-tools=__uam_text_only_worker_no_tools_7f4938d1__",
	                                                     "--disable-builtin-mcps",
	                                                     "--no-custom-instructions",
	                                                     "--no-remote",
	                                                     "--no-remote-export",
	                                                     "--disallow-temp-dir",
	                                                     "--silent",
	                                                 });
	return argv;
}

std::vector<std::string> CopilotCliProviderRuntime::BuildStructuredLaunchArgv(const ProviderProfile&, const ChatSession& chat) const
{
	(void)chat;
	return {"copilot", "--acp", "--stdio"};
}

std::string CopilotCliProviderRuntime::OnAcpValidateResumeId(const ChatSession& chat) const
{
	return uam::acp_detail::ValidGenericAcpResumeId(chat);
}

std::string CopilotCliProviderRuntime::OnAcpMapApprovalModeId(const std::string& mode_id) const
{
	const std::string_view normalized = uam::strings::TrimAsciiView(mode_id);
	if (normalized == uam::approval_modes::kPlanApprovalMode || normalized == uam::approval_modes::kAcpPlanMode)
	{
		return uam::approval_modes::kAcpPlanMode;
	}
	if (normalized == uam::approval_modes::kDefaultApprovalMode || normalized == uam::approval_modes::kAcceptEditsApprovalMode || normalized == uam::approval_modes::kAcpAgentMode)
	{
		return uam::approval_modes::kAcpAgentMode;
	}
	return std::string(normalized);
}

const IProviderRuntime& GetCopilotCliProviderRuntime()
{
	static const CopilotCliProviderRuntime runtime;
	return runtime;
}
