#include "common/provider/provider_runtime.h"

#include "common/config/approval_modes.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_json_rpc.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/runtime/acp/acp_request_defaults.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"
#include "common/provider/runtime/provider_build_config.h"

#include <nlohmann/json.hpp>

using namespace uam::acp_detail;

nlohmann::json IProviderRuntime::OnAcpBuildInitialize(uam::AcpSessionState& session, int request_id) const
{
	(void)session;
	return uam::acp_json_rpc::Request(request_id, uam::acp_methods::kInitialize,
	    {
	        {"protocolVersion", 1},
	        {"clientCapabilities", nlohmann::json::object()},
	        {"clientInfo", uam::acp_request_defaults::ClientInfo()},
	    });
}

void IProviderRuntime::OnAcpInitializeResult(uam::AcpSessionState& session, const nlohmann::json& result) const
{
	if (!result.is_object()) return;
	const nlohmann::json agent_info = JsonObjectValue(result, "agentInfo");
	if (agent_info.is_object())
	{
		session.agent_name = JsonDiagnosticStringValue(agent_info, "name");
		session.agent_title = JsonDiagnosticStringValue(agent_info, "title");
		session.agent_version = JsonDiagnosticStringValue(agent_info, "version");
	}
	const nlohmann::json agent_capabilities = JsonObjectValue(result, "agentCapabilities");
	if (agent_capabilities.is_object())
	{
		session.load_session_supported = JsonBooleanValueOr(agent_capabilities, "loadSession", false);
		const nlohmann::json session_capabilities = JsonObjectValue(agent_capabilities, "sessionCapabilities");
		session.resume_session_supported = session_capabilities.is_object() &&
		                                   session_capabilities.contains("resume") &&
		                                   session_capabilities["resume"].is_object();
		const nlohmann::json mcp_capabilities = JsonObjectValue(agent_capabilities, "mcpCapabilities");
		session.mcp_http_supported = JsonBooleanValueOr(mcp_capabilities, "http", false);
		session.mcp_sse_supported = JsonBooleanValueOr(mcp_capabilities, "sse", false);
	}
}

nlohmann::json IProviderRuntime::OnAcpBuildSetupRequest(int request_id, const ChatSession& chat,
    const std::string& cwd, bool can_load, std::string& out_method) const
{
	if (can_load && !uam::strings::IsBlank(chat.native_session_id))
	{
		out_method = uam::acp_methods::kSessionLoad;
		return BuildLoadSessionRequest(request_id, chat.native_session_id, cwd);
	}
	out_method = uam::acp_methods::kSessionNew;
	return BuildNewSessionRequest(request_id, cwd);
}

std::string IProviderRuntime::OnAcpValidateResumeId(const ChatSession& chat) const
{
	return uam::acp_detail::ValidGeminiResumeId(chat);
}

nlohmann::json IProviderRuntime::OnAcpBuildPrompt(uam::AcpSessionState& session, int request_id,
    const std::string& prompt, const ChatSession& chat, std::string& out_method) const
{
	out_method = uam::acp_methods::kSessionPrompt;
	std::string supported_effort;
	for (const uam::AcpModelState& model : session.available_models)
	{
		if (model.id != chat.model_id)
		{
			continue;
		}
		for (const std::string& effort : model.supported_reasoning_efforts)
		{
			if (effort == chat.reasoning_effort)
			{
				supported_effort = effort;
				break;
			}
		}
		break;
	}
	return BuildPromptRequest(request_id, session.session_id, prompt, supported_effort);
}

nlohmann::json IProviderRuntime::OnAcpBuildCancel(const uam::AcpSessionState& session,
    int request_id, std::string& out_method) const
{
	(void)request_id;
	out_method.clear();
	return BuildCancelNotification(session.session_id);
}

bool IProviderRuntime::OnAcpSetModeLocally(uam::AcpSessionState& session, const std::string& mode_id) const
{
	(void)session;
	(void)mode_id;
	return false;
}

bool IProviderRuntime::OnAcpSetModelLocally(uam::AcpSessionState& session, const std::string& model_id) const
{
	(void)session;
	(void)model_id;
	return false;
}

nlohmann::json IProviderRuntime::OnAcpBuildPermissionResponse(const uam::AcpSessionState& session,
    const std::string& option_id, bool cancelled) const
{
	return uam::acp_json_rpc::SuccessResponse(
	    uam::acp_detail::StableStringToJsonRpcId(session.pending_permission.request_id_json),
	    BuildGenericPermissionOutcomeResult(option_id, cancelled));
}

bool IProviderRuntime::OnAcpTryAutoApprove(uam::AcpSessionState& session, const ChatSession& chat,
    std::string* error_out) const
{
	(void)session;
	(void)chat;
	(void)error_out;
	return false;
}

std::string IProviderRuntime::OnAcpMapApprovalModeId(const std::string& mode_id) const
{
	if (mode_id == uam::approval_modes::kAcceptEditsApprovalMode)
	{
		return uam::approval_modes::kProviderAutoEditApprovalMode;
	}
	return mode_id;
}

bool IProviderRuntime::ProviderRecognizesSubagentTool(std::string_view tool_name) const
{
	return WordMatchesAnyCaseInsensitive(tool_name, {"task", "subtask", "delegate", "spawn_agent", "delegate_to_agent"});
}
