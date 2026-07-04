#include "common/provider/codex/cli/codex_cli_provider_runtime.h"

#include "common/provider/codex/cli/codex_thread_id.h"
#include "common/provider/codex/codex_options.h"
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_json_rpc.h"
#include "common/runtime/acp/acp_permissions.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/runtime/acp/acp_request_defaults.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"

namespace
{
	constexpr const char* kCodexFullAutoFlag = "--full-auto";

	std::vector<std::string> CodexFlagsFromSettings(const AppSettings& settings)
	{
		return uam::provider_runtime_internal::BuildProviderFlagsArgv(settings, kCodexFullAutoFlag);
	}
} // namespace

const char* CodexCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kCodexCli;
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


std::vector<std::string> CodexCliProviderRuntime::BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const
{
	std::vector<std::string> argv = {"codex", "exec"};
	const std::vector<std::string> flags = uam::provider_runtime_internal::ProviderWorkerFlags(profile, settings);
	uam::provider_runtime_internal::AppendArgs(argv, flags);
	
	// Add read-only args for worker mode
	constexpr const char* kCodexReadOnlyWorkerArgs[] = {
	    "--ignore-user-config", "--ignore-rules", "--json", "--color", "never",
	    "--ephemeral", "--skip-git-repo-check", "--sandbox", "read-only",
	    "-c", "model_reasoning_effort=\"low\"",
	};
	for (const char* arg : kCodexReadOnlyWorkerArgs)
	{
		argv.push_back(arg);
	}
	
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "-m", model_id);

	argv.push_back(std::string(prompt));
	return argv;
}

std::vector<std::string> CodexCliProviderRuntime::BuildStructuredLaunchArgv(const ProviderProfile&, const ChatSession&) const
{
	return {"codex", "app-server", "--listen", "stdio://"};
}

nlohmann::json CodexCliProviderRuntime::OnAcpBuildInitialize(uam::AcpSessionState& session, int request_id) const
{
	(void)session;
	return uam::acp_detail::BuildCodexInitializeRequest(request_id);
}

void CodexCliProviderRuntime::OnAcpInitializeResult(uam::AcpSessionState& session, const nlohmann::json& result) const
{
	session.agent_name = "codex";
	session.agent_title = "Codex";
	session.load_session_supported = true;
	if (result.is_object())
	{
		session.agent_version = uam::acp_detail::JsonDiagnosticStringValue(result, "userAgent");
	}
	uam::acp_detail::WriteAcpMessage(session, uam::acp_detail::BuildCodexInitializedNotification());
	const int model_list_id = uam::acp_detail::NextAcpRequestId(session, uam::acp_methods::kModelList);
	uam::acp_detail::WriteAcpMessage(session, uam::acp_detail::BuildCodexModelListRequest(model_list_id));
}

nlohmann::json CodexCliProviderRuntime::OnAcpBuildSetupRequest(int request_id, const ChatSession& chat,
    const std::string& cwd, bool can_load, std::string& out_method) const
{
	(void)can_load;
	const std::string resume_id = uam::codex::ValidThreadIdOrEmpty(chat.native_session_id);
	if (resume_id.empty())
	{
		out_method = uam::acp_methods::kThreadStart;
		return uam::acp_detail::BuildCodexThreadStartRequest(request_id, chat, cwd);
	}
	out_method = uam::acp_methods::kThreadResume;
	ChatSession resume_chat = chat;
	resume_chat.native_session_id = resume_id;
	return uam::acp_detail::BuildCodexThreadResumeRequest(request_id, resume_chat, cwd);
}

std::string CodexCliProviderRuntime::OnAcpValidateResumeId(const ChatSession& chat) const
{
	return uam::codex::ValidThreadIdOrEmpty(chat.native_session_id);
}

nlohmann::json CodexCliProviderRuntime::OnAcpBuildPrompt(uam::AcpSessionState& session, int request_id,
    const std::string& prompt, const ChatSession& chat, std::string& out_method) const
{
	out_method = uam::acp_methods::kTurnStart;
	return uam::acp_detail::BuildCodexTurnStartRequest(
	    request_id, session.session_id, prompt, chat, session.current_model_id);
}

nlohmann::json CodexCliProviderRuntime::OnAcpBuildCancel(const uam::AcpSessionState& session,
    int request_id, std::string& out_method) const
{
	if (!session.session_id.empty() && !session.codex_turn_id.empty())
	{
		out_method = uam::acp_methods::kTurnInterrupt;
		return uam::acp_detail::BuildCodexTurnInterruptRequest(
		    request_id, session.session_id, session.codex_turn_id);
	}
	if (!session.session_id.empty())
	{
		out_method.clear();
		return uam::acp_detail::BuildCancelNotification(session.session_id);
	}
	out_method.clear();
	return nullptr;
}

bool CodexCliProviderRuntime::OnAcpSetModeLocally(uam::AcpSessionState& session, const std::string& mode_id) const
{
	session.current_mode_id = mode_id;
	return true;
}

bool CodexCliProviderRuntime::OnAcpSetModelLocally(uam::AcpSessionState& session, const std::string& model_id) const
{
	session.current_model_id = model_id;
	return true;
}

nlohmann::json CodexCliProviderRuntime::OnAcpBuildPermissionResponse(const uam::AcpSessionState& session,
    const std::string& option_id, bool cancelled) const
{
	const std::string kind = session.pending_permission.provider_request_kind;
	const bool deny = uam::acp_permissions::IsDenyDecision(option_id, cancelled);
	nlohmann::json response = uam::acp_json_rpc::SuccessResponse(
	    uam::acp_detail::StableStringToJsonRpcId(session.pending_permission.request_id_json),
	    nlohmann::json::object());

	if (uam::acp_permissions::IsCodexDecisionPermissionKind(kind))
	{
		response["result"] = {{"decision", uam::acp_permissions::CodexDecisionForOption(option_id, cancelled)}};
	}
	else if (kind == uam::acp_permissions::kCodexPermissionsRequestKind)
	{
		nlohmann::json permissions = nlohmann::json::object();
		if (!deny && !session.pending_permission.codex_approval_payload_json.empty())
		{
			try
			{
				const nlohmann::json payload = nlohmann::json::parse(session.pending_permission.codex_approval_payload_json);
				const nlohmann::json* parsed_permissions = uam::nlohmann_json::FindField(payload, "permissions");
				if (parsed_permissions != nullptr)
				{
					permissions = *parsed_permissions;
				}
			}
			catch (const nlohmann::json::exception&)
			{
				permissions = nlohmann::json::object();
			}
		}
		response["result"] = {
		    {uam::acp_permissions::kPermissionsField, permissions},
		    {uam::acp_permissions::kScopeField, uam::acp_permissions::kSessionScope},
		};
	}
	else
	{
		response["result"] = nlohmann::json::object();
	}
	return response;
}

bool CodexCliProviderRuntime::OnAcpTryAutoApprove(uam::AcpSessionState& session, const ChatSession& chat,
    std::string* error_out) const
{
	(void)session;
	(void)chat;
	(void)error_out;
	return false;
}

std::string CodexCliProviderRuntime::OnAcpMapApprovalModeId(const std::string& mode_id) const
{
	return mode_id;
}

const IProviderRuntime& GetCodexCliProviderRuntime()
{
	static const CodexCliProviderRuntime runtime;
	return runtime;
}
