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

std::vector<std::string> ClaudeCliProviderRuntime::BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const
{
	std::vector<std::string> argv = {"claude", "-p"};
	const std::vector<std::string> flags = uam::provider_runtime_internal::ProviderWorkerFlags(profile, settings);
	uam::provider_runtime_internal::AppendArgs(argv, flags);
	
	// Add stateless args for worker mode
	argv.push_back("--no-session-persistence");
	argv.push_back("--tools");
	argv.push_back("");
	
	if (!model_id.empty())
	{
		argv.push_back("--model");
		argv.push_back(std::string(model_id));
	}
	
	argv.push_back("--");
	argv.push_back(std::string(prompt));
	return argv;
}

const IProviderRuntime& GetClaudeCliProviderRuntime()
{
	static const ClaudeCliProviderRuntime runtime;
	return runtime;
}
