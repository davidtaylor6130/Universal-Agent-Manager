#include "common/provider/codex/cli/codex_cli_provider_runtime.h"

#include "common/provider/codex/cli/codex_thread_id.h"
#include "app/goal_service.h"
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"

namespace
{
	constexpr const char* kCodexFullAutoFlag = "--full-auto";

	AppSettings CodexTemplateCommandSettings(const ProviderProfile& profile, const AppSettings& settings)
	{
		return uam::provider_runtime_internal::MergeProviderSettingsWithCustomYoloFlag(profile, settings, kCodexFullAutoFlag);
	}

	std::vector<std::string> CodexFlagsFromSettings(const AppSettings& settings)
	{
		return uam::provider_runtime_internal::BuildProviderFlagsArgv(settings, kCodexFullAutoFlag);
	}
} // namespace

const char* CodexCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kCodexCli;
}

bool CodexCliProviderRuntime::IsEnabled() const
{
	return true;
}

const char* CodexCliProviderRuntime::DisabledReason() const
{
	return "";
}

std::string CodexCliProviderRuntime::BuildPrompt(const ProviderProfile&, std::string_view user_prompt, const std::vector<std::string>& files, const Goal* active_goal, int64_t tokens_used, int64_t token_budget) const
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

std::string CodexCliProviderRuntime::BuildCommand(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, const std::vector<std::string>& files, const std::string& resume_session_id, const ChatSession* chat) const
{
	(void)resume_session_id;
	const AppSettings provider_settings = CodexTemplateCommandSettings(profile, settings);
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
	const std::string effective_prompt = BuildPrompt(profile, prompt, files, active_goal, active_goal == nullptr ? 0 : active_goal->tokens_used, active_goal == nullptr ? 0 : active_goal->token_budget);
	return uam::provider_runtime_internal::BuildCommandFromTemplate(provider_settings, effective_prompt, files, "", "codex exec {flags} {prompt}");
}

std::vector<std::string> CodexCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	const AppSettings provider_settings = uam::provider_runtime_internal::MergeProviderSettings(profile, settings);
	std::vector<std::string> argv;
	const std::string resume_id = uam::codex::ValidThreadIdOrEmpty(chat.native_session_id);
	if (profile.supports_resume && !resume_id.empty())
	{
		uam::provider_runtime_internal::AppendLiteralArgs(argv, {"codex", "resume", "--no-alt-screen"});
		argv.push_back(resume_id);
	}
	else
	{
		argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "codex --no-alt-screen");
	}

	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "-m", chat.model_id);

	uam::provider_runtime_internal::AppendArgs(argv, CodexFlagsFromSettings(provider_settings));
	return argv;
}

MessageRole CodexCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const
{
	return uam::provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> CodexCliProviderRuntime::LoadHistory(const ProviderProfile& profile, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	(void)profile;
	return uam::provider_runtime_internal::LoadLocalChats(data_root);
}

bool CodexCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return uam::provider_runtime_internal::SaveLocalChat(data_root, chat);
}

bool CodexCliProviderRuntime::UsesNativeOverlayHistory(const ProviderProfile&) const
{
	return false;
}

bool CodexCliProviderRuntime::SupportsGeminiJsonHistory(const ProviderProfile&) const
{
	return false;
}

bool CodexCliProviderRuntime::UsesLocalHistory(const ProviderProfile&) const
{
	return true;
}

bool CodexCliProviderRuntime::UsesInternalEngine(const ProviderProfile&) const
{
	return false;
}

bool CodexCliProviderRuntime::UsesCliOutput(const ProviderProfile&) const
{
	return true;
}

bool CodexCliProviderRuntime::UsesGeminiPathBootstrap(const ProviderProfile&) const
{
	return false;
}

const IProviderRuntime& GetCodexCliProviderRuntime()
{
	static const CodexCliProviderRuntime runtime;
	return runtime;
}
