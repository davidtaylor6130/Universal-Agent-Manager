#include "common/provider/claude/cli/claude_cli_provider_runtime.h"

#include "computer_use/computer_use_mcp_config.h"
#include "common/config/approval_modes.h"
#include "common/provider/provider_ids.h"
#include "common/state/app_state.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/utils/range_utils.h"

#include <array>
#include <string_view>

namespace
{
	constexpr auto kClaudeProviderPermissionModes = std::to_array<std::string_view>({
	    uam::approval_modes::kDefaultApprovalMode,
	    uam::approval_modes::kPlanApprovalMode,
	});

	std::vector<std::string> ClaudeFlagsFromSettings(const AppSettings& settings)
	{
		return uam::provider_runtime_internal::BuildProviderFlagsArgv(settings);
	}

	bool ShouldPassClaudePermissionMode(std::string_view approval_mode)
	{
		return uam::ranges::Contains(kClaudeProviderPermissionModes, approval_mode);
	}

	std::string ClaudeStructuredPermissionMode(const ChatSession& chat)
	{
		return uam::strings::TrimAsciiView(chat.approval_mode) == uam::approval_modes::kPlanApprovalMode
		           ? uam::approval_modes::kPlanApprovalMode
		           : uam::approval_modes::kDefaultApprovalMode;
	}

	void AppendClaudeModeArgs(std::vector<std::string>& argv, const ChatSession& chat, const AppSettings& settings)
	{
		uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", chat.model_id);

		const std::string approval_mode = uam::approval_modes::EffectiveProviderMode(chat.approval_mode, "off");
		if (ShouldPassClaudePermissionMode(approval_mode))
		{
			uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--permission-mode", approval_mode);
		}
	}
} // namespace

const char* ClaudeCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kClaudeCli;
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


std::vector<std::string> ClaudeCliProviderRuntime::BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const
{
	std::vector<std::string> argv = {"claude", "-p"};
	const std::vector<std::string> flags = uam::provider_runtime_internal::ProviderWorkerFlags(profile, settings);
	uam::provider_runtime_internal::AppendArgs(argv, flags);
	
	// Add stateless args for worker mode
	argv.push_back("--no-session-persistence");
	argv.push_back("--tools");
	argv.push_back("");

	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", model_id);

	argv.push_back("--");
	argv.push_back(std::string(prompt));
	return argv;
}

std::vector<std::string> ClaudeCliProviderRuntime::BuildStructuredLaunchArgv(const ProviderProfile&, const ChatSession& chat) const
{
	std::vector<std::string> argv = {"claude", "-p", "--output-format", "stream-json", "--input-format", "stream-json", "--verbose"};
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--permission-mode", ClaudeStructuredPermissionMode(chat));
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", chat.model_id);
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--resume", chat.native_session_id);
	uam::computer_use::AppendClaudeMcpLaunchArguments(argv, chat);
	return argv;
}

nlohmann::json ClaudeCliProviderRuntime::OnAcpBuildInitialize(uam::AcpSessionState& session, int request_id) const
{
	(void)request_id;
	session.initialized = true;
	session.load_session_supported = true;
	session.available_modes = {
	    uam::AcpModeState{uam::approval_modes::kDefaultApprovalMode, "Default", "Use Claude's provider-managed permissions."},
	    uam::AcpModeState{uam::approval_modes::kPlanApprovalMode, "Plan", "Let Claude research and propose changes without editing files."},
	};
	if (session.current_mode_id.empty())
	{
		session.current_mode_id = uam::approval_modes::kDefaultApprovalMode;
	}
	return nullptr;
}

void ClaudeCliProviderRuntime::OnAcpInitializeResult(uam::AcpSessionState& session, const nlohmann::json& result) const
{
	(void)session;
	(void)result;
}

nlohmann::json ClaudeCliProviderRuntime::OnAcpBuildSetupRequest(int request_id, const ChatSession& chat,
    const std::string& cwd, bool can_load, std::string& out_method) const
{
	(void)request_id;
	(void)chat;
	(void)cwd;
	(void)can_load;
	out_method.clear();
	return nullptr;
}

nlohmann::json ClaudeCliProviderRuntime::OnAcpBuildPrompt(uam::AcpSessionState& session, int request_id,
    const std::string& prompt, const ChatSession& chat, std::string& out_method) const
{
	(void)session;
	(void)request_id;
	(void)chat;
	out_method.clear();
	return uam::acp_detail::BuildClaudeInputMessage(prompt);
}

nlohmann::json ClaudeCliProviderRuntime::OnAcpBuildCancel(const uam::AcpSessionState&, int, std::string& out_method) const
{
	out_method.clear();
	return nullptr;
}

bool ClaudeCliProviderRuntime::OnAcpSetModeLocally(uam::AcpSessionState& session, const std::string& mode_id) const
{
	session.current_mode_id = mode_id;
	return false;
}

bool ClaudeCliProviderRuntime::OnAcpSetModelLocally(uam::AcpSessionState& session, const std::string& model_id) const
{
	session.current_model_id = model_id;
	return false;
}

const IProviderRuntime& GetClaudeCliProviderRuntime()
{
	static const ClaudeCliProviderRuntime runtime;
	return runtime;
}
