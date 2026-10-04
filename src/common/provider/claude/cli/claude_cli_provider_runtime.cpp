#include "common/provider/claude/cli/claude_cli_provider_runtime.h"

#include "computer_use/computer_use_mcp_config.h"
#include "common/config/approval_modes.h"
#include "common/provider/provider_ids.h"
#include "common/state/app_state.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_claude_stream.h"
#include "common/runtime/acp/acp_permissions.h"
#include "common/provider/claude/cli/claude_acp_message_handlers.h"
#include "common/utils/range_utils.h"
#include "common/utils/nlohmann_json_utils.h"

#include <array>
#include <string_view>

namespace
{
	constexpr auto kClaudeProviderPermissionModes = std::to_array<std::string_view>({
	    uam::approval_modes::kDefaultApprovalMode,
	    uam::approval_modes::kPlanApprovalMode,
	});


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

const ProviderCliPolicy* ClaudeCliProviderRuntime::CliVersionPolicy() const
{
	static constexpr ProviderCliPolicy policy
	{
		.provider_id = uam::provider_ids::kClaudeCli,
		.npm_package = "@anthropic-ai/claude-code",
		.fallback_title = "Claude Code",
		.executable_name = "claude",
		.version_probe_command = "claude --version",
		.homebrew_package = "claude-code",
		.homebrew_cask = true,
		.preferred_version = "latest",
		.provider_managed = true,
	};
	return &policy;
}

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
	uam::provider_runtime_internal::AppendArgs(argv, uam::provider_runtime_internal::BuildProviderFlagsArgv(provider_settings));
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
	std::vector<std::string> argv = {"claude", "-p", "--output-format", "stream-json", "--input-format", "stream-json", "--verbose", "--permission-prompt-tool", "stdio"};
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--permission-mode", ClaudeStructuredPermissionMode(chat));
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", chat.model_id);
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--resume", chat.native_session_id);
	uam::computer_use::AppendClaudeMcpLaunchArguments(argv, chat);
	return argv;
}

nlohmann::json ClaudeCliProviderRuntime::OnAcpBuildInitialize(uam::AcpSessionState& session, int request_id) const
{
	session.initialized = false;
	session.load_session_supported = true;
	session.available_modes = {
	    uam::AcpModeState{uam::approval_modes::kDefaultApprovalMode, "Default", "Use Claude's provider-managed permissions."},
	    uam::AcpModeState{uam::approval_modes::kPlanApprovalMode, "Plan", "Let Claude research and propose changes without editing files."},
	};
	if (session.current_mode_id.empty())
	{
		session.current_mode_id = uam::approval_modes::kDefaultApprovalMode;
	}
	return uam::acp_claude_stream::ControlRequest(request_id, {{"subtype", "initialize"}});
}

bool ClaudeCliProviderRuntime::OnAcpHandleMessage(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
    const nlohmann::json& message, const CefRefPtr<CefBrowser>& browser) const
{
	using namespace uam::acp_detail;
	try
	{
		HandleClaudeMessage(app, session, chat, message, browser);
		MarkAcpRuntimeActivity(session);
	}
	catch (const std::exception& ex)
	{
		const std::string error_message = std::string("Claude stream-json message handling failed: ") + ex.what();
		AppendAcpDiagnostic(session, "parse", "claude_message_parse_error", "", "", false, 0,
		                    error_message, CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
		InvalidateAcpTransport(app, session, chat, error_message);
	}
	return true;
}

void ClaudeCliProviderRuntime::OnAcpInitializeResult(uam::AcpSessionState& session, const nlohmann::json& result) const
{
	session.available_models.clear();
	for (const nlohmann::json& model : uam::acp_detail::JsonArrayValue(result, "models"))
	{
		if (!model.is_object())
			continue;
		const std::string id = uam::nlohmann_json::TrimmedStringValue(model, {"value"});
		if (id.empty())
			continue;
		session.available_models.push_back(uam::AcpModelState{id, uam::acp_detail::JsonDiagnosticStringValueOr(model, "displayName", id), uam::acp_detail::JsonDiagnosticStringValue(model, "description")});
	}
	session.agent_name = "claude";
	session.agent_title = "Claude Code";
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

nlohmann::json ClaudeCliProviderRuntime::OnAcpBuildCancel(const uam::AcpSessionState&, int request_id, std::string& out_method) const
{
	out_method = "claude/interrupt";
	return uam::acp_claude_stream::ControlRequest(request_id, {{"subtype", "interrupt"}});
}

nlohmann::json ClaudeCliProviderRuntime::OnAcpBuildPermissionResponse(const uam::AcpSessionState& session, const std::string& option_id, bool cancelled) const
{
	const bool deny = cancelled || option_id != "allow";
	nlohmann::json input = nlohmann::json::parse(session.pending_permission.provider_input_json, nullptr, false);
	nlohmann::json result = {{"behavior", "deny"}, {"message", "Permission denied by the user."}};
	if (!deny && input.is_object())
		result = {{"behavior", "allow"}, {"updatedInput", std::move(input)}};
	return uam::acp_claude_stream::ControlResponse(uam::acp_detail::StableStringToJsonRpcId(session.pending_permission.request_id_json), std::move(result));
}

nlohmann::json ClaudeCliProviderRuntime::OnAcpBuildUserInputResponse(const uam::AcpSessionState& session, const std::map<std::string, std::vector<std::string>>& answers) const
{
	nlohmann::json input = nlohmann::json::parse(session.pending_user_input.provider_input_json, nullptr, false);
	nlohmann::json result = {{"behavior", "deny"}, {"message", "Question cancelled by the user."}};
	if (!answers.empty() && input.is_object())
	{
		nlohmann::json mapped = nlohmann::json::object();
		for (const uam::AcpUserInputQuestionState& question : session.pending_user_input.questions)
		{
			const auto found = answers.find(question.id);
			if (found != answers.end())
				mapped[question.question] = uam::strings::Join(found->second, ", ");
		}
		input["answers"] = std::move(mapped);
		result = {{"behavior", "allow"}, {"updatedInput", std::move(input)}};
	}
	return uam::acp_claude_stream::ControlResponse(uam::acp_detail::StableStringToJsonRpcId(session.pending_user_input.request_id_json), std::move(result));
}

const IProviderRuntime& GetClaudeCliProviderRuntime()
{
	static const ClaudeCliProviderRuntime runtime;
	return runtime;
}
