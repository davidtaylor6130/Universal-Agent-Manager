#include "common/runtime/acp/acp_session_internal.h"

#include "common/platform/platform_services.h"
#include "common/paths/workspace_root.h"
#include "common/runtime/acp/acp_json_rpc.h"
#include "common/runtime/acp/acp_permissions.h"
#include "common/runtime/acp/acp_statuses.h"
#include "common/runtime/acp/acp_tool_kinds.h"
#include "common/runtime/acp/acp_tool_items.h"
#include "common/security/command_safety.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"

#include <map>
#include <string>
#include <vector>

namespace uam::acp_detail
{

bool WriteAcpMessage(AcpSessionState& session, const nlohmann::json& message, std::string* error_out)
{
	std::string line = message.dump();
	line.push_back('\n');

	const std::string method = AcpMessageMethodForDiagnostics(message);
	const std::string request_id = AcpMessageRequestIdForDiagnostics(message);
	const std::string detail = AcpMessageDetailForDiagnostics(message);
	std::string write_error;
	if (!PlatformServicesFactory::Instance().process_service.WriteToStdioProcess(session, line.data(), line.size(), &write_error))
	{
		const std::string runtime_name = RuntimeDisplayName(session);
		session.last_error = write_error.empty() ? ("Failed to write message to " + runtime_name + ".") : ("Failed to write message to " + runtime_name + ": " + write_error);
		session.lifecycle_state = kAcpLifecycleError;
		AppendAcpDiagnostic(session, "write", "write_failed", method, request_id, false, 0, session.last_error, detail);
		if (error_out != nullptr)
		{
			*error_out = session.last_error;
		}
		return false;
	}

	AppendAcpDiagnostic(session, "write", "sent", method, request_id, false, 0, "", detail);
	return true;
}

void SendJsonRpcError(AcpSessionState& session, const nlohmann::json& id, int code, const std::string& message)
{
	(void)WriteAcpMessage(session, uam::acp_json_rpc::ErrorResponse(id, code, message));
}

nlohmann::json BuildGenericPermissionOutcomeResult(const std::string& option_id, bool cancelled)
{
	nlohmann::json outcome = {
	    {uam::acp_permissions::kOutcomeField, cancelled ? uam::acp_permissions::kCancelledOutcome : uam::acp_permissions::kSelectedOutcome},
	};

	if (!cancelled)
	{
		outcome[uam::acp_permissions::kOptionIdField] = option_id;
	}

	return {
	    {uam::acp_permissions::kOutcomeField, std::move(outcome)},
	};
}

bool SendPermissionResponse(AcpSessionState& session, const std::string& request_id_json, const std::string& option_id, bool cancelled, std::string* error_out)
{
	(void)request_id_json;
	nlohmann::json response = ProviderRuntimeRegistry::ResolveById(session.provider_id)
	    .OnAcpBuildPermissionResponse(session, option_id, cancelled);
	return WriteAcpMessage(session, response, error_out);
}

bool IsRejectPermissionOption(const std::string& id, const std::string& name, const std::string& kind)
{
	return TextContainsAnyCaseInsensitive(id,
	                                      {
	                                          "decline",
	                                          "deny",
	                                          "reject",
	                                          uam::acp_permissions::kCancelDecision,
	                                      }) ||
	       TextContainsAnyCaseInsensitive(name,
	                                      {
	                                          "decline",
	                                          "deny",
	                                          "reject",
	                                          uam::acp_permissions::kCancelDecision,
	                                      }) ||
	       TextContainsAnyCaseInsensitive(kind, {
	                                            "reject",
	                                            uam::acp_permissions::kCancelOptionKind,
	                                        });
}

bool IsAcceptPermissionOption(const std::string& id, const std::string& name, const std::string& kind)
{
	// ACP option kinds are allow_once / allow_always; ids and names vary by provider.
	return TextContainsAnyCaseInsensitive(kind,
	                                      {
	                                          "allow",
	                                      }) ||
	       TextContainsAnyCaseInsensitive(id,
	                                      {
	                                          "accept",
	                                          "allow",
	                                          "approve",
	                                          "yes",
	                                          "once",
	                                          "always",
	                                      }) ||
	       TextContainsAnyCaseInsensitive(name, {
	                                            "accept",
	                                            "allow",
	                                            "approve",
	                                            "yes",
	                                        });
}

std::string AutoApproveOptionId(const AcpPendingPermissionState& pending)
{
	for (const AcpPermissionOptionState& option : pending.options)
	{
		const std::string id = uam::strings::ToLowerAscii(option.id);
		const std::string name = uam::strings::ToLowerAscii(option.name);
		const std::string kind = uam::strings::ToLowerAscii(option.kind);
		if (IsRejectPermissionOption(id, name, kind))
		{
			continue;
		}
		if (IsAcceptPermissionOption(id, name, kind))
		{
			return option.id;
		}
	}
	return "";
}

bool TryAutoApprovePendingPermission(AcpSessionState& session, const ChatSession& chat, std::string* error_out)
{
	const auto tier = uam::command_safety::ParseTier(chat.command_safety_tier);
	if (session.pending_permission.request_id_json.empty() || tier == uam::command_safety::Tier::Off)
	{
		return false;
	}
	if (tier == uam::command_safety::Tier::AcceptEdits && session.pending_permission.safety_tier != "acceptEdits")
	{
		return false;
	}
	if (tier != uam::command_safety::Tier::Yolo && tier != uam::command_safety::Tier::AcceptEdits &&
	    (session.pending_permission.safety_risk.empty() || session.pending_permission.safety_requires_approval))
	{
		return false;
	}

	std::string option_id = AutoApproveOptionId(session.pending_permission);
	if (option_id.empty())
	{
		return false;
	}

	if (!SendPermissionResponse(session, session.pending_permission.request_id_json, option_id, false, error_out))
	{
		return false;
	}

	if (!session.pending_permission.tool_call_id.empty())
	{
		AcpToolCallState& tracked_tool_call = UpsertToolCall(session, session.pending_permission.tool_call_id);
		tracked_tool_call.status = uam::acp_statuses::kAutoApproved;
	}
	AppendAcpDiagnostic(session, "permission", uam::acp_statuses::kAutoApproved, session.pending_permission.provider_request_method, session.pending_permission.request_id_json, false, 0, "UAM yolo auto-approved a command permission request.");
	session.pending_permission = AcpPendingPermissionState{};
	session.waiting_for_permission = false;
	ClearAcpPendingWait(session);
	session.lifecycle_state = session.processing ? kAcpLifecycleProcessing : kAcpLifecycleReady;
	return true;
}

void AppendIgnoredRequestDuringCancelDiagnostic(AcpSessionState& session, const nlohmann::json& message, const char* reason, const char* diagnostic_message)
{
	AppendAcpDiagnostic(session, "request", reason, JsonDiagnosticStringValue(message, "method"), JsonRpcIdToStableString(JsonRpcIdOrNull(message)), false, 0, diagnostic_message);
}

nlohmann::json BuildCodexUserInputResponse(const std::string& request_id_json, const std::map<std::string, std::vector<std::string>>& answers)
{
	nlohmann::json answer_map = nlohmann::json::object();
	for (const auto& [question_id, values] : answers)
	{
		if (question_id.empty())
		{
			continue;
		}

		nlohmann::json answer_values = nlohmann::json::array();
		for (const std::string& value : values)
		{
			answer_values.push_back(value);
		}
		answer_map[question_id] = {{"answers", std::move(answer_values)}};
	}

	return uam::acp_json_rpc::SuccessResponse(StableStringToJsonRpcId(request_id_json), {
	                                                                                        {"answers", std::move(answer_map)},
	                                                                                    });
}

bool SendCodexUserInputResponse(AcpSessionState& session, const std::string& request_id_json, const std::map<std::string, std::vector<std::string>>& answers, std::string* error_out)
{
	return WriteAcpMessage(session, BuildCodexUserInputResponse(request_id_json, answers), error_out);
}

void ApplyCommandSafetyDecision(const AppState& app, const ChatSession& chat, AcpPendingPermissionState& pending)
{
	const auto tier = uam::command_safety::ParseTier(chat.command_safety_tier);
	if (tier == uam::command_safety::Tier::Off || tier == uam::command_safety::Tier::Yolo) return;
	const std::string kind = uam::strings::ToLowerAscii(uam::strings::Trim(pending.kind));
	const bool command = kind == "commandexecution" || kind == "execute";
	const bool file_change = kind == "filechange" || kind == "edit" || kind == "write" || kind == "create" || kind == "delete" || kind == "move";
	if (tier == uam::command_safety::Tier::AcceptEdits)
	{
		if (file_change)
		{
			pending.safety_risk = "allowed";
			pending.safety_tier = "acceptEdits";
			pending.safety_requires_approval = false;
		}
		return;
	}
	if (!command && !file_change) return;
	const auto risk = file_change
	    ? uam::command_safety::RiskLevel::Warn
	    : uam::command_safety::ClassifyCommand(pending.content);
	pending.safety_risk = uam::command_safety::RiskLevelName(risk);
	pending.safety_tier = uam::command_safety::TierName(tier);
	pending.version_controlled_workspace = uam::command_safety::WorkspaceIsVersionControlled(uam::paths::ResolveWorkspaceRootPath(app, chat));
	pending.safety_requires_approval = uam::command_safety::RequiresApproval(tier, risk, pending.version_controlled_workspace);
}

void HandlePermissionRequest(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
{
	if (uam::AcpSessionHasPendingCancel(session))
	{
		AppendIgnoredRequestDuringCancelDiagnostic(session, message, "ignored_permission_during_cancel", "Ignoring permission request while a turn cancel is pending.");
		return;
	}

	const nlohmann::json params = JsonObjectValue(message, "params");
	const nlohmann::json tool_call = JsonObjectValue(params, "toolCall");

	AcpPendingPermissionState pending;
	pending.request_id_json = JsonRpcIdToStableString(JsonRpcIdOrNull(message));
	pending.tool_call_id = JsonDiagnosticStringValue(tool_call, "toolCallId");
	pending.title = JsonDiagnosticStringValueOr(tool_call, "title", "Permission required");
	pending.kind = JsonDiagnosticStringValueOr(tool_call, "kind", uam::acp_tool_kinds::kOther);
	pending.status = JsonDiagnosticStringValueOr(tool_call, "status", std::string(uam::acp_statuses::kPending));
	if (const nlohmann::json* content = uam::nlohmann_json::FindField(tool_call, "content"); content != nullptr)
	{
		pending.content = ContentTextFromJson(*content);
	}

	const nlohmann::json options = JsonArrayValue(params, "options");
	if (options.is_array())
	{
		for (const nlohmann::json& option : options)
		{
			if (!option.is_object())
			{
				continue;
			}
			AcpPermissionOptionState parsed;
			parsed.id = JsonDiagnosticStringValue(option, "optionId");
			parsed.name = JsonDiagnosticStringValueOr(option, "name", parsed.id);
			parsed.kind = JsonDiagnosticStringValue(option, "kind");
			if (!parsed.id.empty())
			{
				pending.options.push_back(std::move(parsed));
			}
		}
	}

	if (!pending.tool_call_id.empty())
	{
		AcpToolCallState& tracked_tool_call = UpsertToolCall(session, pending.tool_call_id);
		tracked_tool_call.title = pending.title;
		tracked_tool_call.kind = pending.kind;
		tracked_tool_call.status = pending.status;
		tracked_tool_call.content = pending.content;
	}
	AppendPermissionTurnEventIfNeeded(session, pending.request_id_json, pending.tool_call_id);

	ApplyCommandSafetyDecision(app, chat, pending);
	session.pending_permission = std::move(pending);
	if (TryAutoApprovePendingPermission(session, chat))
	{
		return;
	}
	session.waiting_for_permission = true;
	BeginAcpPendingWait(session, kAcpLifecycleWaitingPermission);
}

} // namespace uam::acp_detail
