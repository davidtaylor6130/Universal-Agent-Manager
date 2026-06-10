#include "common/provider/claude/cli/claude_cli_provider_runtime.h"

#include "app/goal_service.h"
#include "common/config/approval_modes.h"
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/utils/range_utils.h"

#include <array>
#include <string_view>

namespace
{
	constexpr const char* kClaudeDangerouslySkipPermissionsFlag = "--dangerously-skip-permissions";

	constexpr auto kClaudeProviderPermissionModes = std::to_array<std::string_view>({
	    uam::approval_modes::kDefaultApprovalMode,
	    uam::approval_modes::kPlanApprovalMode,
	});

	std::vector<std::string> ClaudeFlagsFromSettings(const AppSettings& settings)
	{
		return uam::provider_runtime_internal::BuildProviderFlagsArgv(settings, kClaudeDangerouslySkipPermissionsFlag);
	}

	bool ShouldPassClaudePermissionMode(std::string_view approval_mode)
	{
		return uam::ranges::Contains(kClaudeProviderPermissionModes, approval_mode);
	}

	void AppendClaudeModeArgs(std::vector<std::string>& argv, const ChatSession& chat, const AppSettings& settings)
	{
		uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", chat.model_id);

		if (!settings.provider_yolo_mode)
		{
			const std::string approval_mode = uam::approval_modes::AppApprovalModeOrEmpty(chat.approval_mode);
			if (ShouldPassClaudePermissionMode(approval_mode))
			{
				uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--permission-mode", approval_mode);
			}
		}
	}
} // namespace

const char* ClaudeCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kClaudeCli;
}

bool ClaudeCliProviderRuntime::IsEnabled() const
{
	return true;
}

const char* ClaudeCliProviderRuntime::DisabledReason() const
{
	return "";
}

std::string ClaudeCliProviderRuntime::BuildPrompt(const ProviderProfile&, std::string_view user_prompt, const std::vector<std::string>& files, const Goal* active_goal, int64_t tokens_used, int64_t token_budget) const
{
	std::string prompt = uam::provider_runtime_internal::BuildPrompt(user_prompt, files);
	if (active_goal && !active_goal->objective.empty())
	{
		const std::string goal_prompt = uam::GoalService::BuildContinuationPrompt(*active_goal, tokens_used, token_budget);
		if (!goal_prompt.empty())
		{
			prompt = goal_prompt + "\n\n" + prompt;
		}
	}
	return prompt;
}

std::string ClaudeCliProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, const std::vector<std::string>& files, const std::string& resume_session_id, const ChatSession* chat) const
{
	const AppSettings provider_settings = uam::provider_runtime_internal::MergeProviderSettings(profile, settings);
	std::vector<std::string> argv = {"claude", "-p"};
	uam::provider_runtime_internal::AppendResumeArgs(argv, profile, resume_session_id);

	uam::provider_runtime_internal::AppendArgs(argv, ClaudeFlagsFromSettings(provider_settings));
	const Goal* active_goal = nullptr;
	if (chat != nullptr && !chat->active_goal_id.empty())
	{
		for (const Goal& goal : chat->goals)
		{
			if (goal.id == chat->active_goal_id && goal.status == GoalStatus::Active)
			{
				active_goal = &goal;
				break;
			}
		}
	}
	argv.push_back(BuildPrompt(profile, prompt, files, active_goal, active_goal == nullptr ? 0 : active_goal->tokens_used, active_goal == nullptr ? 0 : active_goal->token_budget));
	return uam::provider_runtime_internal::JoinShellEscapedArgs(argv);
}

std::vector<std::string> ClaudeCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	const AppSettings provider_settings = uam::provider_runtime_internal::MergeProviderSettings(profile, settings);
	std::vector<std::string> argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "claude");
	uam::provider_runtime_internal::AppendResumeArgs(argv, profile, chat.native_session_id);

	AppendClaudeModeArgs(argv, chat, provider_settings);
	uam::provider_runtime_internal::AppendArgs(argv, ClaudeFlagsFromSettings(provider_settings));
	return argv;
}

MessageRole ClaudeCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const
{
	return uam::provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> ClaudeCliProviderRuntime::LoadHistory(const ProviderProfile&, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	return uam::provider_runtime_internal::LoadLocalChats(data_root);
}

bool ClaudeCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return uam::provider_runtime_internal::SaveLocalChat(data_root, chat);
}

bool ClaudeCliProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return false;
}

bool ClaudeCliProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return false;
}

bool ClaudeCliProviderRuntime::UsesLocalHistory(const ProviderProfile&) const
{
	return true;
}

bool ClaudeCliProviderRuntime::UsesInternalEngine(const ProviderProfile&) const
{
	return false;
}

bool ClaudeCliProviderRuntime::UsesCliOutput(const ProviderProfile&) const
{
	return true;
}

bool ClaudeCliProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile&) const
{
	return false;
}

const IProviderRuntime& GetClaudeCliProviderRuntime()
{
	static const ClaudeCliProviderRuntime runtime;
	return runtime;
}
