#include "common/runtime/acp/acp_session_internal.h"

#include "common/platform/platform_services.h"
#include "app/chat_domain_service.h"
#include "app/provider_worker_command.h"
#include "common/config/approval_modes.h"
#include "common/paths/workspace_root.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_ids.h"
#include "common/runtime/acp/acp_json_rpc.h"
#include "common/runtime/acp/acp_permissions.h"
#include "common/runtime/acp/acp_statuses.h"
#include "common/runtime/acp/acp_tool_kinds.h"
#include "common/runtime/acp/acp_tool_items.h"
#include "common/security/command_safety.h"
#include "common/utils/sensitive_text.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace uam::acp_detail
{

namespace
{
	constexpr std::size_t kPermissionReviewInputLimit = 16 * 1024;
	constexpr std::size_t kPermissionReviewOutputLimit = 8 * 1024;
	constexpr std::size_t kPermissionReviewReasonLimit = 512;
	constexpr int kPermissionReviewTimeoutMs = 20'000;
	constexpr std::size_t kMaxQueuedPermissions = 64;

	std::string PermissionReviewReasonForDiagnostic(std::string_view reason)
	{
		std::string result = CapDiagnosticString(uam::strings::Trim(reason), kPermissionReviewReasonLimit);
		return uam::sensitive::LooksSensitiveText(result) ? "Reviewer reason was hidden because it looked sensitive." : result;
	}

	std::string BuildPermissionReviewPrompt(const AcpPendingPermissionState& pending)
	{
		nlohmann::json request = {
		    {"title", CapDiagnosticString(uam::strings::Trim(pending.title), 1024)},
		    {"kind", CapDiagnosticString(uam::strings::Trim(pending.kind), 256)},
		    {"content", CapDiagnosticString(uam::strings::Trim(pending.content), 12 * 1024)},
		};
		std::string prompt = "Review this single provider permission request. Return exactly one JSON object and no prose or markdown: {\"decision\":\"approve|deny|uncertain\",\"reason\":\"short reason\"}. Approve only when the shown operation is clearly safe and intended. Deny clearly unsafe or destructive operations. Use uncertain for missing context. Request:\n" + request.dump();
		return CapDiagnosticString(prompt, kPermissionReviewInputLimit);
	}

	bool HasPermissionReviewTask(const AppState& app, std::string_view chat_id, std::string_view request_id_json)
	{
		return std::ranges::any_of(app.permission_review_tasks, [&](const AsyncPermissionReviewTask& task) {
			return task.chat_id == chat_id && task.request_id_json == request_id_json;
		});
	}

	void PersistPermissionReviewDiagnostic(ChatSession& chat, AcpSessionState& session, std::string decision, std::string reason)
	{
		if (session.pending_permission.tool_call_id.empty()) return;
		AcpToolCallState& tool_call = UpsertToolCall(session, session.pending_permission.tool_call_id);
		tool_call.permission_review_decision = std::move(decision);
		tool_call.permission_review_reason = PermissionReviewReasonForDiagnostic(std::move(reason));
		(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
	}

	bool TryStartPermissionReview(AppState& app, AcpSessionState& session, const ChatSession& chat)
	{
		if (uam::command_safety::ParseTier(chat.command_safety_tier) != uam::command_safety::Tier::AiReview ||
		    session.pending_permission.request_id_json.empty() ||
		    HasPermissionReviewTask(app, chat.id, session.pending_permission.request_id_json))
		{
			return false;
		}

		const std::string provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(app.settings.permission_reviewer_provider_id);
		const ProviderProfile* provider = ProviderProfileStore::FindById(app.provider_profiles, provider_id);
		if (provider == nullptr || !ProviderRuntime::IsRuntimeEnabled(*provider))
		{
			AppendAcpDiagnostic(session, "permission_review", "reviewer_unavailable", "", session.pending_permission.request_id_json, false, 0, "AI Review left this request for the user because no available reviewer provider is configured.");
			return false;
		}

		const std::string normalized_input = session.pending_permission.title + "\n" + session.pending_permission.kind + "\n" + session.pending_permission.content;
		if (uam::sensitive::LooksSensitiveText(normalized_input))
		{
			AppendAcpDiagnostic(session, "permission_review", "sensitive_input", "", session.pending_permission.request_id_json, false, 0, "AI Review left this request for the user because its content looked sensitive.");
			return false;
		}

		const std::string prompt = BuildPermissionReviewPrompt(session.pending_permission);
		std::string launch_error;
		ProviderWorkerInvocation invocation = BuildProviderWorkerInvocation(app, *provider, app.settings, prompt, app.settings.permission_reviewer_model_id, ProviderWorkerPathMode::BasePath, &launch_error);
		if (invocation.Empty())
		{
			AppendAcpDiagnostic(session, "permission_review", "reviewer_unavailable", "", session.pending_permission.request_id_json, false, 0, "AI Review left this request for the user because the isolated reviewer could not start.");
			return false;
		}

		AsyncPermissionReviewTask task;
		task.chat_id = chat.id;
		task.request_id_json = session.pending_permission.request_id_json;
		task.command_preview = invocation.command_preview;
		task.state = std::make_shared<AsyncProcessTaskState>();
		task.state->launch_time = std::chrono::steady_clock::now();
		task.state->provider_id = provider->id;
		auto state = task.state;
		task.worker = std::make_unique<std::jthread>([state, invocation = std::move(invocation)](std::stop_token stop_token) {
			state->result = ExecuteProviderWorkerInvocation(invocation, uam::paths::CurrentPathOrDot(), kPermissionReviewTimeoutMs, stop_token);
			state->completed = true;
		});
		app.permission_review_tasks.push_back(std::move(task));
		AppendAcpDiagnostic(session, "permission_review", "started", "", session.pending_permission.request_id_json, false, 0, "AI Review started in an isolated text-only worker.");
		return true;
	}

	bool PermissionFitsPlanCeiling(const AcpPendingPermissionState& pending)
	{
		const std::string kind = uam::strings::ToLowerAscii(uam::strings::Trim(pending.kind));
		if (kind == "read" || kind == "search" || kind == "fetch" || kind == "inspect" ||
		    kind == "list" || kind == "query" || kind == "find")
		{
			return true;
		}
		if (kind == "commandexecution" || kind == "execute")
		{
			return uam::command_safety::ClassifyCommand(pending.content) ==
			       uam::command_safety::RiskLevel::Allowed;
		}
		return false;
	}
}

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
	if (!cancelled && std::ranges::none_of(session.pending_permission.options, [&option_id](const AcpPermissionOptionState& option) { return option.id == option_id; }))
	{
		if (error_out != nullptr)
		{
			*error_out = "ACP permission option is not offered by the active request.";
		}
		return false;
	}
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
		if (TextContainsAnyCaseInsensitive(id, {"always"}) ||
		    TextContainsAnyCaseInsensitive(name, {"always"}) ||
		    TextContainsAnyCaseInsensitive(kind, {"always"}))
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

std::string RejectPermissionOptionId(const AcpPendingPermissionState& pending)
{
	for (const AcpPermissionOptionState& option : pending.options)
	{
		if (!TextContainsAnyCaseInsensitive(option.id, {"always"}) &&
		    !TextContainsAnyCaseInsensitive(option.name, {"always"}) &&
		    !TextContainsAnyCaseInsensitive(option.kind, {"always"}) &&
		    IsRejectPermissionOption(option.id, option.name, option.kind))
		{
			return option.id;
		}
	}
	return "";
}

std::optional<PermissionReviewDecision> ParsePermissionReviewOutput(std::string_view output)
{
	const std::string trimmed = uam::strings::Trim(output);
	if (trimmed.empty() || trimmed.size() > kPermissionReviewOutputLimit) return std::nullopt;
	try
	{
		const nlohmann::json parsed = nlohmann::json::parse(trimmed);
		if (!parsed.is_object() || parsed.size() != 2 || !parsed.contains("decision") || !parsed.contains("reason") ||
		    !parsed["decision"].is_string() || !parsed["reason"].is_string()) return std::nullopt;
		PermissionReviewDecision result{uam::strings::ToLowerAscii(uam::strings::Trim(parsed["decision"].get<std::string>())), PermissionReviewReasonForDiagnostic(parsed["reason"].get<std::string>())};
		if ((result.decision != "approve" && result.decision != "deny" && result.decision != "uncertain") || result.reason.empty()) return std::nullopt;
		return result;
	}
	catch (const nlohmann::json::exception&)
	{
		return std::nullopt;
	}
}

bool TryAutoApprovePendingPermission(AppState& app, AcpSessionState& session, const ChatSession& chat, std::string* error_out)
{
	const auto tier = uam::command_safety::ParseTier(chat.command_safety_tier);
	bool approved = false;
	while (!session.pending_permission.request_id_json.empty())
	{
		if ((session.goal_review_turn || uam::approval_modes::AppApprovalModeOrEmpty(chat.approval_mode) ==
		         uam::approval_modes::kPlanApprovalMode ||
		     session.active_uam_agent_workspace_access == "read") &&
		    !PermissionFitsPlanCeiling(session.pending_permission))
		{
			const std::string option_id = RejectPermissionOptionId(session.pending_permission);
			if (!SendPermissionResponse(session, session.pending_permission.request_id_json,
			                            option_id, option_id.empty(), error_out))
			{
				break;
			}
			if (!session.pending_permission.tool_call_id.empty())
			{
				UpsertToolCall(session, session.pending_permission.tool_call_id).status =
				    uam::acp_statuses::kFailed;
			}
			AppendAcpDiagnostic(session, "permission", "rejected_by_access_ceiling",
			                    session.pending_permission.provider_request_method,
			                    session.pending_permission.request_id_json, false, 0,
				                    "UAM read-only review rejected a request outside its access ceiling.");
			session.pending_permission = AcpPendingPermissionState{};
			if (!session.queued_permissions.empty())
			{
				session.pending_permission = std::move(session.queued_permissions.front());
				session.queued_permissions.pop_front();
			}
			continue;
		}

		if (tier == uam::command_safety::Tier::Off || tier == uam::command_safety::Tier::AiReview ||
		    (tier == uam::command_safety::Tier::AcceptEdits && session.pending_permission.safety_tier != "acceptEdits") ||
		    (tier != uam::command_safety::Tier::Yolo && tier != uam::command_safety::Tier::AcceptEdits &&
		     (session.pending_permission.safety_risk.empty() || session.pending_permission.safety_requires_approval)))
		{
			break;
		}

		const std::string option_id = AutoApproveOptionId(session.pending_permission);
		if (option_id.empty())
		{
			break;
		}
		if (!SendPermissionResponse(session, session.pending_permission.request_id_json, option_id, false, error_out))
		{
			break;
		}

		if (!session.pending_permission.tool_call_id.empty())
		{
			AcpToolCallState& tracked_tool_call = UpsertToolCall(session, session.pending_permission.tool_call_id);
			tracked_tool_call.status = uam::acp_statuses::kAutoApproved;
		}
		const char* decision_reason = tier == uam::command_safety::Tier::Yolo
		                                  ? "UAM YOLO auto-approved one permission request."
		                                  : tier == uam::command_safety::Tier::AcceptEdits
		                                        ? "UAM Accept Edits auto-approved one file-change request."
		                                        : "UAM command safety auto-approved one permission request.";
		AppendAcpDiagnostic(session, "permission", uam::acp_statuses::kAutoApproved, session.pending_permission.provider_request_method, session.pending_permission.request_id_json, false, 0, decision_reason);
		session.pending_permission = AcpPendingPermissionState{};
		approved = true;
		if (!session.queued_permissions.empty())
		{
			session.pending_permission = std::move(session.queued_permissions.front());
			session.queued_permissions.pop_front();
		}
	}

	session.waiting_for_permission = !session.pending_permission.request_id_json.empty();
	if (session.waiting_for_permission)
	{
		BeginAcpPendingWait(session, kAcpLifecycleWaitingPermission);
	}
	else
	{
		ClearAcpPendingWait(session);
		session.lifecycle_state = session.processing ? kAcpLifecycleProcessing : kAcpLifecycleReady;
	}
	if (session.waiting_for_permission)
	{
		(void)TryStartPermissionReview(app, session, chat);
	}
	return approved;
}

void QueueAcpPermission(AppState& app, AcpSessionState& session, const ChatSession& chat, AcpPendingPermissionState pending)
{
	if (!session.pending_permission.request_id_json.empty())
	{
		if (session.queued_permissions.size() >= kMaxQueuedPermissions)
		{
			const std::string message = "The provider exceeded UAM's 64-request permission queue limit.";
			AppendAcpDiagnostic(session, "permission", "queue_limit_exceeded",
			                    pending.provider_request_method, pending.request_id_json,
			                    false, 0, message);
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(session, true);
			PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(session);
			session.running = false;
			FailAcpTurnOrSession(session, message);
			return;
		}
		session.queued_permissions.push_back(std::move(pending));
		return;
	}
	session.pending_permission = std::move(pending);
	(void)TryAutoApprovePendingPermission(app, session, chat);
}

void AdvanceAcpPermissionQueue(AppState& app, AcpSessionState& session, const ChatSession& chat, std::string* error_out)
{
	session.pending_permission = AcpPendingPermissionState{};
	if (!session.queued_permissions.empty())
	{
		session.pending_permission = std::move(session.queued_permissions.front());
		session.queued_permissions.pop_front();
	}
	(void)TryAutoApprovePendingPermission(app, session, chat, error_out);
}

void StopPermissionReviewTasks(AppState& app, std::string_view chat_id, std::string_view request_id_json)
{
	for (auto it = app.permission_review_tasks.begin(); it != app.permission_review_tasks.end();)
	{
		if ((!chat_id.empty() && it->chat_id != chat_id) || (!request_id_json.empty() && it->request_id_json != request_id_json))
		{
			++it;
			continue;
		}
		StopAsyncPermissionReviewTask(*it);
		it = app.permission_review_tasks.erase(it);
	}
}

bool PollPermissionReviewTasks(AppState& app)
{
	bool changed = false;
	for (auto it = app.permission_review_tasks.begin(); it != app.permission_review_tasks.end();)
	{
		if (it->state == nullptr || !it->state->completed.load())
		{
			++it;
			continue;
		}
		StopAsyncPermissionReviewTask(*it);
		const ProcessExecutionResult result = it->state->result;
		AcpSessionState* session = FindAcpSessionForChat(app, it->chat_id);
		ChatSession* chat = ChatDomainService().FindChatById(app, it->chat_id);
		if (session != nullptr && chat != nullptr && session->running && session->pending_permission.request_id_json == it->request_id_json && uam::command_safety::ParseTier(chat->command_safety_tier) == uam::command_safety::Tier::AiReview)
		{
			const auto decision = result.ok && !result.output_truncated ? ParsePermissionReviewOutput(result.output) : std::nullopt;
			if (!decision)
			{
				const std::string reason = "AI Review could not make a valid decision; the request remains for the user.";
				PersistPermissionReviewDiagnostic(*chat, *session, "error", reason);
				AppendAcpDiagnostic(*session, "permission_review", result.timed_out ? "timeout" : result.canceled ? "canceled" : "invalid_result", "", it->request_id_json, false, 0, reason);
			}
			else if (decision->decision == "uncertain")
			{
				PersistPermissionReviewDiagnostic(*chat, *session, decision->decision, decision->reason);
				AppendAcpDiagnostic(*session, "permission_review", "uncertain", "", it->request_id_json, false, 0, decision->reason);
			}
			else
			{
				const bool approve = decision->decision == "approve";
				const std::string option_id = approve ? AutoApproveOptionId(session->pending_permission) : RejectPermissionOptionId(session->pending_permission);
				std::string response_error;
				const bool sent = approve && option_id.empty()
				    ? false
				    : SendPermissionResponse(*session, it->request_id_json, option_id, !approve && option_id.empty(), &response_error);
				if (sent)
				{
					PersistPermissionReviewDiagnostic(*chat, *session, decision->decision, decision->reason);
					AppendAcpDiagnostic(*session, "permission_review", approve ? "approved_once" : "denied", "", it->request_id_json, false, 0, decision->reason);
					AdvanceAcpPermissionQueue(app, *session, *chat, &response_error);
				}
				else
				{
					const std::string reason = "AI Review could not safely send its decision; the request remains for the user.";
					PersistPermissionReviewDiagnostic(*chat, *session, "error", reason);
					AppendAcpDiagnostic(*session, "permission_review", approve ? "missing_allow_once" : "response_failed", "", it->request_id_json, false, 0, reason);
				}
			}
			changed = true;
		}
		it = app.permission_review_tasks.erase(it);
	}
	return changed;
}

void CancelPendingAcpPermissions(AcpSessionState& session, std::string* error_out)
{
	while (!session.pending_permission.request_id_json.empty())
	{
		(void)SendPermissionResponse(session, session.pending_permission.request_id_json, "", true, error_out);
		session.pending_permission = AcpPendingPermissionState{};
		if (!session.queued_permissions.empty())
		{
			session.pending_permission = std::move(session.queued_permissions.front());
			session.queued_permissions.pop_front();
		}
	}
	session.queued_permissions.clear();
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
	pending.safety_risk.clear();
	pending.safety_tier.clear();
	pending.safety_requires_approval = false;
	pending.version_controlled_workspace = false;
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

std::string PermissionContentFromToolCall(const nlohmann::json& tool_call)
{
	if (const nlohmann::json* raw_input = uam::nlohmann_json::FindField(tool_call, "rawInput"); raw_input != nullptr)
	{
		std::string command = raw_input->is_object() ? JsonDiagnosticStringValue(*raw_input, "command") : ContentTextFromJson(*raw_input);
		command = uam::strings::Trim(command);
		if (!command.empty())
			return command;
		if (raw_input->is_object() && !raw_input->empty())
			return raw_input->dump();
	}
	return ToolCallContentTextFromJson(tool_call);
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
	pending.content = PermissionContentFromToolCall(tool_call);

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
	QueueAcpPermission(app, session, chat, std::move(pending));
}

} // namespace uam::acp_detail
