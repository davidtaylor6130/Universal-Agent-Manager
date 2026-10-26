#include "common/provider/copilot/cli/copilot_cli_provider_runtime.h"
#include "common/provider/copilot/cli/copilot_child_lifecycle.h"
#include "common/provider/copilot/cli/copilot_mcp_presentation.h"
#include "common/provider/copilot/cli/copilot_managed_read_policy.h"

#include "app/provider_model_catalog_service.h"
#include "cef/cef_push.h"
#include "common/config/approval_modes.h"
#include "common/config/execution_host_config.h"
#include "common/chat/chat_ids.h"
#include "common/paths/workspace_root.h"
#include "common/platform/platform_services.h"
#include "common/provider/provider_ids.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_json_rpc.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_statuses.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/utils/base64.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"
#include "computer_use/computer_use_mcp_config.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <unordered_set>
#include <limits>

namespace
{

using namespace uam;
using namespace uam::acp_detail;

std::string Text(const nlohmann::json& object, const char* key)
{
	return std::string(uam::nlohmann_json::StringViewOrEmpty(object, key));
}

bool Flag(const nlohmann::json& object, const char* key)
{
	return uam::nlohmann_json::BoolFieldStrict(object, key).value_or(false);
}

std::optional<std::int64_t> Count(const nlohmann::json& object, const char* key)
{
	const nlohmann::json* value = uam::nlohmann_json::FindField(object, key);
	if (value == nullptr || !value->is_number_integer()) return std::nullopt;
	if (value->is_number_unsigned() && value->get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
	const std::int64_t parsed = value->get<std::int64_t>();
	return parsed >= 0 ? std::optional<std::int64_t>(parsed) : std::nullopt;
}

void FinishSteer(AppState& app, AcpSessionState& session, ChatSession& chat,
    const std::string& request_id, const std::string& error)
{
	const auto pending = session.pending_steer_requests.find(request_id);
	if (pending == session.pending_steer_requests.end()) return;
	if (!error.empty())
	{
		const int index = pending->second.user_message_index;
		if (index >= 0 && index < static_cast<int>(chat.messages.size()) && chat.messages[static_cast<std::size_t>(index)].role == MessageRole::User)
			chat.messages[static_cast<std::size_t>(index)].interrupted = true;
		if (pending->second.turn_serial == session.turn_serial) session.last_error = error;
		(void)SaveChatQuietly(app, chat);
	}
	session.pending_steer_requests.erase(pending);
}

void FinishMaintenance(AcpSessionState& session)
{
	session.manual_compaction_pending = false;
	session.context_compaction_in_progress = false;
	session.processing = false;
	session.inactivity_timeout_pending = false;
	if (session.cancel_request_id == 0)
	{
		session.cancel_requested = false;
		session.cancel_requested_time_s = 0;
	}
	session.lifecycle_state = kAcpLifecycleReady;
}

/// SDK protocol v3 emits an event and expects a separate native permission RPC.
/// Protocol v2 used a JSON-RPC callback; accepting that callback remains fail-closed.
nlohmann::json PermissionResponse(const AcpSessionState& session,
    const AcpPendingPermissionState& pending, const std::string& option, bool cancelled)
{
	const std::string decision = cancelled ? "user-not-available" :
	    option == "allow_once" ? "approved" : "denied-interactively-by-user";
	const nlohmann::json result = {{"kind", decision}};
	const nlohmann::json request_id = StableStringToJsonRpcId(pending.request_id_json);
	if (pending.provider_request_method == "permission.request")
		return uam::acp_json_rpc::SuccessResponse(request_id, {{"result", result}});
	// Permission decisions are independent of prompt/cancel IDs. A namespaced
	// string ID avoids colliding with the monotonically allocated integer IDs.
	return {{"jsonrpc", "2.0"}, {"id", "uam-permission:" + pending.request_id_json},
	    {"method", "session.permissions.handlePendingPermissionRequest"},
	    {"params", {{"sessionId", session.session_id}, {"requestId", request_id}, {"result", result}}}};
}

void RequestPermission(AppState& app, AcpSessionState& session, ChatSession& chat,
    const nlohmann::json& id, const nlohmann::json& request, const std::string& method)
{
	if (!id.is_string() && !id.is_number_integer()) return;
	const std::string stable_id = JsonRpcIdToStableString(id);
	AcpPendingPermissionState pending;
	pending.request_id_json = JsonRpcIdToStableString(id);
	pending.provider_request_method = method;
	pending.provider_request_kind = Text(request, "kind");
	pending.provider_request_params_json = request.dump();
	pending.provider_input_json = request.dump();
	pending.tool_call_id = Text(request, "toolCallId");
	pending.title = Text(request, "intention");
	if (pending.title.empty()) pending.title = "Copilot " + pending.provider_request_kind + " permission";
	pending.kind = pending.provider_request_kind == "shell" ? "execute" :
	    pending.provider_request_kind == "write" ? "edit" : pending.provider_request_kind == "read" ? "read" : "other";
	pending.content = Text(request, "fullCommandText");
	if (pending.provider_request_kind == "shell")
		if (const nlohmann::json* command = uam::nlohmann_json::FindStringField(request, "fullCommandText"))
			pending.execution_command = command->get_ref<const std::string&>();
	if (!pending.content.empty())
		pending.provider_input_json = nlohmann::json{{"command", pending.content}, {"copilotPermission", request}}.dump();
	else pending.content = Text(request, "path");
	if (pending.content.empty()) pending.content = Text(request, "url");
	if (pending.content.empty()) pending.content = request.dump(2);
	pending.status = uam::acp_statuses::kPending;
	pending.options = {{"allow_once", "Allow once", "allow_once"}, {"reject_once", "Deny", "reject_once"}};
	if (!session.remote_source_exit_reported && ReplayPersistedInteractionResponseIfMatched(app, session, chat, {{"id", id}}, &pending))
	{
		if (std::ranges::any_of(chat.remote_interaction_responses, [&](const auto& saved) { return saved.request_id_json == stable_id; }))
			session.copilot_native_state["replayedPermissions"]["uam-permission:" + stable_id] = true;
		return;
	}
	if (uam::AcpSessionHasPendingCancel(session) || session.remote_source_exit_reported)
	{
		if (!session.remote_source_exit_reported) (void)WriteAcpMessage(session, PermissionResponse(session, pending, "", true));
		return;
	}
	if (session.pending_permission.request_id_json == pending.request_id_json &&
	    session.pending_permission.provider_request_method == method) return;
	for (const AcpPendingPermissionState& queued : session.queued_permissions)
		if (queued.request_id_json == pending.request_id_json && queued.provider_request_method == method) return;
	AppendPermissionTurnEventIfNeeded(session, pending.request_id_json, pending.tool_call_id);
	ApplyCommandSafetyDecision(app, chat, pending);
	QueueAcpPermission(app, session, chat, std::move(pending));
}

constexpr std::string_view kKeepPlanning = "Keep planning";
constexpr std::string_view kFinishPlanning = "Finish planning without implementation";
constexpr std::string_view kApproveImplementation = "Approve plan and implement interactively";

bool ReadOnlyPlan(const AcpSessionState& session)
{
	return session.goal_review_turn || session.active_uam_agent_workspace_access == "read";
}

void RecordPlanDecision(AcpSessionState& session, const std::string& stable_id,
	const nlohmann::json& native_id, const nlohmann::json& response, const std::string& prior_policy)
{
	auto& reviews = session.copilot_native_state["planReviews"];
	if (!reviews.is_object()) reviews = nlohmann::json::object();
	for (auto it = reviews.begin(); it != reviews.end();)
		if (Count(*it, "turnSerial").value_or(-1) != session.turn_serial) it = reviews.erase(it);
		else ++it;
	const bool event = Text(response, "method") == "session.ui.handlePendingExitPlanMode";
	const auto decision = event ? JsonObjectValue(JsonObjectValue(response, "params"), "response") : JsonObjectValue(response, "result");
	reviews[stable_id] = {{"nativeId", JsonRpcIdToStableString(native_id)}, {"turnSerial", session.turn_serial},
		{"action", Flag(decision, "approved") ? Text(decision, "selectedAction") : ""},
		{"priorApprovalMode", prior_policy}, {"response", response}, {"delivered", false},
		{"callback", !event}, {"activeTurn", session.processing},
		{"rpcId", event ? JsonRpcIdToStableString(JsonRpcIdOrNull(response)) : ""}, {"ackPending", event}};
}

void NotePlanResponseWritten(AcpSessionState& session, const nlohmann::json& response)
{
	auto& reviews = session.copilot_native_state["planReviews"];
	if (!reviews.is_object()) return;
	for (auto& review : reviews)
	{
		if (Count(review, "turnSerial").value_or(-1) != session.turn_serial ||
		    uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(review, "response")) != response) continue;
		review["delivered"] = true;
	}
}

bool CurrentPlanDecision(const AcpSessionState& session, const nlohmann::json& review)
{
	const auto turn = Count(review, "turnSerial").value_or(-1);
	return turn == session.turn_serial && Flag(review, "delivered") &&
	    !(Flag(review, "activeTurn") && turn > 0 && session.last_settled_turn_serial >= static_cast<std::uint64_t>(turn));
}

bool ConfirmPlanImplementation(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& review)
{
	if (!CurrentPlanDecision(session, review) || Text(review, "action") != "interactive" || ReadOnlyPlan(session) ||
	    chat.imported_read_only || Text(review, "priorApprovalMode") != chat.approval_mode) return false;
	const std::string prior_policy = chat.approval_mode;
	chat.approval_mode = uam::approval_modes::kDefaultApprovalMode;
	if (!SaveChatQuietly(app, chat))
	{
		chat.approval_mode = prior_policy;
		const int abort = NextAcpRequestId(session, "session.abort");
		(void)WriteAcpMessage(session, uam::acp_json_rpc::Request(abort, "session.abort", {{"sessionId", session.session_id}}));
		FailAcpTurnOrSession(session, &chat, "The confirmed Copilot plan permission could not be saved; the turn was stopped and Plan was retained.");
		session.current_mode_id = uam::approval_modes::kPlanApprovalMode;
		session.queue_held = chat.acp_queue_held = true;
		return false;
	}
	session.current_mode_id = uam::approval_modes::kDefaultApprovalMode;
	return true;
}

void RequestPlanReview(AppState& app, AcpSessionState& session, ChatSession& chat,
	const nlohmann::json& id, const nlohmann::json& request, bool native_event)
{
	if (!id.is_string() && !id.is_number_integer()) return;
	const std::string stable = JsonRpcIdToStableString(id);
	if (session.remote_source_exit_reported) return;
	// Captured read turns must decode a fresh question; no speculative replay authority.
	for (const auto& saved : chat.remote_interaction_responses)
		if (!ReadOnlyPlan(session) && saved.request_id_json == stable)
		{
			session.copilot_native_state["replayedPlanReviews"]["uam-exit-plan:" + stable] = true;
			const auto response = nlohmann::json::parse(saved.response_json, nullptr, false);
			if (response.is_object()) RecordPlanDecision(session, stable,
					uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(request, "requestId")), response, chat.approval_mode);
		}
	if (ReplayPersistedInteractionResponseIfMatched(app, session, chat, {{"id", id}})) return;
	if (session.pending_user_input.request_id_json == stable) return;
	const auto deny = [&]
	{
		const nlohmann::json result = {{"approved", false}};
		if (native_event) (void)WriteAcpMessage(session, {{"jsonrpc", "2.0"}, {"id", "uam-exit-plan:" + stable},
			{"method", "session.ui.handlePendingExitPlanMode"}, {"params", {{"sessionId", session.session_id}, {"requestId", id}, {"response", result}}}});
		else (void)WriteAcpMessage(session, uam::acp_json_rpc::SuccessResponse(id, result));
	};
	if (uam::AcpSessionHasPendingCancel(session) || session.waiting_for_user_input ||
		!request.contains("actions") || !request["actions"].is_array() ||
		(Text(request, "summary").empty() && Text(request, "planContent").empty()))
	{
		deny();
		return;
	}
	// A new review supersedes any unconfirmed callback decision whose native
	// request UUID was never exposed; a later mode event cannot approve this new plan.
	auto& reviews = session.copilot_native_state["planReviews"];
	if (reviews.is_object())
	{
		for (auto it = reviews.begin(); it != reviews.end();)
		{
			if (Flag(*it, "callback") && it.key() != stable)
			{
				it = reviews.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
	AcpPendingUserInputState pending;
	pending.request_id_json = stable;
	nlohmann::json metadata = request;
	metadata["copilotExitPlan"] = true;
	metadata["copilotExitPlanEvent"] = native_event;
	metadata["copilotApprovalMode"] = chat.approval_mode;
	// Callback transport IDs have no documented relationship to native plan UUIDs.
	metadata["copilotPlanRequestId"] = uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(request, "requestId"));
	pending.provider_input_json = metadata.dump();
	pending.status = uam::acp_statuses::kPending;
	pending.attention_kind = "question";
	AcpUserInputQuestionState question;
	question.id = "plan-review";
	question.question = "Review Copilot plan\n\n" + Text(request, "summary");
	const std::string plan = Text(request, "planContent");
	if (!plan.empty()) question.question += "\n\n" + plan;
	question.question += "\n\nImplementation requires your explicit approval. Your command safety settings continue to apply.";
	question.is_other = true; // Free-form text is feedback and never grants approval.
	question.options.push_back({std::string(kKeepPlanning), "Keep the UAM Plan restriction and request further planning."});
	for (const auto& action : request["actions"])
	{
		if (!action.is_string()) continue;
		if (action == "exit_only") question.options.push_back({std::string(kFinishPlanning), "Finish this planning turn; keep UAM Plan mode for future input."});
		if (action == "interactive" && !ReadOnlyPlan(session)) question.options.push_back({std::string(kApproveImplementation), "Switch this chat to Default mode and implement under its existing command safety policy."});
	}
	pending.questions.push_back(std::move(question));
	AppendUserInputTurnEventIfNeeded(session, stable, "");
	session.pending_user_input = std::move(pending);
	session.waiting_for_user_input = true;
	BeginAcpPendingWait(session, kAcpLifecycleWaitingUserInput);
}

void RememberRemoteCompaction(ChatSession& chat, const AcpSessionState& session,
    const std::string& identity, bool completed, bool cancelled = false)
{
	if (!chat.remote_manual_compaction_pending || !session.manual_compaction_pending) return;
	std::string request_id;
	for (const auto& pending : chat.remote_pending_requests)
		if (pending.method == "session.history.compact" && !pending.response_consumed) request_id = std::to_string(pending.request_id);
	if (request_id.empty()) return;
	chat.remote_manual_compaction_boundary_json = nlohmann::json{{"kind", "copilot"}, {"sessionId", session.session_id},
	    {"requestId", request_id}, {"identity", identity}, {"completed", completed}, {"cancelled", cancelled}}.dump();
}

bool AcceptEvent(AcpSessionState& session, const ChatSession& chat, const nlohmann::json& event)
{
	const std::string id = Text(event, "id");
	if (id.empty()) return true;
	nlohmann::json& seen = session.copilot_native_state["events"];
	if (!seen.is_object()) seen = nlohmann::json::object();
	if (seen.contains(id)) return false;
	const std::string receipt = session.session_id + ":" + id;
	for (const Message& message : chat.messages)
		if (std::ranges::find(message.provider_event_ids, receipt) != message.provider_event_ids.end()) return false;
	// Durable content receipts remain in their owning message when the bounded
	// in-memory cache rotates, including after a controller restart before ACK.
	if (seen.size() >= 16384) seen.clear();
	seen[id] = true;
	return true;
}


// Auto routing is a native session observation, separate from ACP configOptions and saved Codex service tiers.
constexpr const char* kAutoRoutingList = "session.model.list";
constexpr const char* kAutoRoutingCurrent = "session.model.getCurrent";
constexpr const char* kAutoRoutingSwitch = "session.model.switchAutoTier";

nlohmann::json AutoRoutingAbsent() { return {{"present", false}}; }

bool AutoRoutingString(const nlohmann::json& value, std::size_t maximum, bool nonempty = true)
{
	return value.is_string() && value.get_ref<const std::string&>().size() <= maximum &&
	    (!nonempty || !value.get_ref<const std::string&>().empty());
}

nlohmann::json AutoRoutingPresence(const nlohmann::json& source, const char* key, bool nullable, bool& valid)
{
	const auto found = source.find(key);
	if (found == source.end()) return AutoRoutingAbsent();
	if (!(nullable && found->is_null()) && !AutoRoutingString(*found, 512)) valid = false;
	return {{"present", true}, {"value", *found}};
}

nlohmann::json* AutoRoutingState(AcpSessionState& session)
{
	const auto found = session.copilot_native_state.find("autoRouting");
	return found != session.copilot_native_state.end() && found->is_object() ? &*found : nullptr;
}

const nlohmann::json* AutoRoutingState(const AcpSessionState& session)
{
	const auto found = session.copilot_native_state.find("autoRouting");
	return found != session.copilot_native_state.end() && found->is_object() ? &*found : nullptr;
}

bool AutoRoutingOwnerMatches(const AcpSessionState& session, const nlohmann::json& routing, const ChatSession* chat = nullptr)
{
	const auto owner = JsonObjectValue(routing, "owner");
	if (!session.running || !session.session_ready || session.protocol_kind != "copilot-sdk" ||
	    !uam::provider_ids::IsCliProviderAliasOf(session.provider_id, uam::provider_ids::kCopilotCli) ||
	    session.chat_id.empty() || session.session_id.empty() || session.process_execution_host_id.empty() ||
	    Text(owner, "chatId") != session.chat_id || Text(owner, "sessionId") != session.session_id ||
	    Text(owner, "executionHostId") != session.process_execution_host_id || Text(owner, "lifetime").empty()) return false;
	for (const char* field : {"chatId", "sessionId", "executionHostId", "lifetime"})
	{
		const auto value = owner.find(field);
		if (value == owner.end() || !AutoRoutingString(*value, std::string_view(field) == "lifetime" ? 128 : 512)) return false;
	}
	const std::string nonce = Text(session.copilot_native_state, "autoRoutingLifetime");
	if (!nonce.empty() && nonce != Text(owner, "lifetime")) return false;
	if (chat != nullptr && (chat->id != session.chat_id || chat->native_session_id != session.session_id ||
	    !uam::provider_ids::IsCliProviderAliasOf(chat->provider_id, uam::provider_ids::kCopilotCli) ||
	    (chat->execution_host_id.empty() ? "local" : chat->execution_host_id) != session.process_execution_host_id)) return false;
	return true;
}

void BindAutoRouting(AcpSessionState& session, const ChatSession& chat)
{
	if (session.model_discovery_only || session.goal_internal_session || chat.imported_read_only ||
	    chat.id != session.chat_id || chat.native_session_id != session.session_id || session.process_execution_host_id.empty() ||
	    (chat.execution_host_id.empty() ? "local" : chat.execution_host_id) != session.process_execution_host_id) return;
	if (Text(session.copilot_native_state, "autoRoutingLifetime").empty())
		session.copilot_native_state["autoRoutingLifetime"] = PlatformServicesFactory::Instance().process_service.GenerateUuid();
	if (Text(session.copilot_native_state, "autoRoutingPublicLifetime").empty())
		session.copilot_native_state["autoRoutingPublicLifetime"] = PlatformServicesFactory::Instance().process_service.GenerateUuid();
	const nlohmann::json owner = {{"chatId", chat.id}, {"sessionId", session.session_id},
	    {"executionHostId", session.process_execution_host_id}, {"lifetime", Text(session.copilot_native_state, "autoRoutingLifetime")}};
	const auto* previous = AutoRoutingState(session);
	const bool switch_unsupported = previous != nullptr && JsonObjectValue(*previous, "owner") == owner && Flag(*previous, "switchUnsupported");
	const std::string switch_unsupported_reason = switch_unsupported ? Text(*previous, "switchUnsupportedReason") : std::string{};
	session.copilot_native_state["autoRouting"] = {
	    {"owner", owner}, {"switchUnsupported", switch_unsupported}, {"switchUnsupportedReason", switch_unsupported_reason},
	    {"revision", 0}, {"catalogStatus", "unknown"}, {"stateStatus", "unknown"},
	    {"eligibleTiers", AutoRoutingAbsent()}, {"providerDefault", AutoRoutingAbsent()},
	    {"currentModel", AutoRoutingAbsent()}, {"effective", AutoRoutingAbsent()},
	    {"pending", AutoRoutingAbsent()}, {"activating", AutoRoutingAbsent()},
	    {"requests", nlohmann::json::object()}, {"lastError", ""}, {"lastAckStatus", ""},
	    {"refreshCatalog", true}, {"refreshState", true}};
}

bool AutoRoutingBusy(const AcpSessionState& session)
{
	return !session.initialized || !session.session_ready || session.lifecycle_state != kAcpLifecycleReady ||
	    session.model_discovery_only || session.goal_internal_session || session.processing || session.waiting_for_permission || session.waiting_for_user_input ||
	    session.manual_compaction_pending || session.context_compaction_in_progress || AcpSessionHasBlockingRuntimeWork(session) ||
	    session.cancel_requested || session.reconnect_pending || session.recovering_remote_turn || session.recovering_remote_process ||
	    session.local_stop_pending || session.remote_stop_pending || session.remote_stop_unconfirmed || session.remote_source_exit_reported ||
	    session.worktree_launch_pending || session.ignore_session_updates_until_ready;
}

bool AutoRoutingMethod(std::string_view method)
{
	return method == kAutoRoutingList || method == kAutoRoutingCurrent || method == kAutoRoutingSwitch;
}

void AutoRoutingError(nlohmann::json& routing, std::string_view method, const std::string& status, const std::string& error)
{
	if (method == kAutoRoutingList)
	{
		routing["catalogStatus"] = status;
		routing["eligibleTiers"] = AutoRoutingAbsent();
		routing["providerDefault"] = AutoRoutingAbsent();
	}
	else routing["stateStatus"] = status;
	routing["lastError"] = std::string(uam::strings::Utf8Prefix(error, 2048));
}

void RecordAutoRoutingSwitchUnsupported(nlohmann::json& routing, const std::string& reason)
{
	// Observation freshness cannot establish a mutation method already rejected by this owner/lifetime.
	routing["switchUnsupported"] = true;
	routing["switchUnsupportedReason"] = std::string(uam::strings::Utf8Prefix(reason, 2048));
}

void RetireAutoRoutingTransportRequest(AcpSessionState& session, const std::string& key, const std::string& method)
{
	int id = 0;
	const auto parsed = std::from_chars(key.data(), key.data() + key.size(), id);
	if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || id <= 0) return;
	const auto found = session.pending_request_methods.find(id);
	if (found != session.pending_request_methods.end() && found->second == method) session.pending_request_methods.erase(found);
	if (method == kAutoRoutingSwitch && session.config_option_change_request_id == id && session.config_option_change_id == "autoRouting")
		ClearAcpConfigOptionChangeRequest(session);
}

bool ExpireAutoRoutingRequests(AcpSessionState& session, nlohmann::json& routing)
{
	auto& requests = routing["requests"];
	if (!requests.is_object()) return false;
	bool changed = false;
	for (auto it = requests.begin(); it != requests.end();)
	{
		const auto deadline = it->find("deadline");
		if (deadline == it->end() || !deadline->is_number() || deadline->get<double>() > GetAppTimeSeconds()) { ++it; continue; }
		const std::string method = Text(*it, "method");
		changed = true;
		if (JsonObjectValue(*it, "owner") != JsonObjectValue(routing, "owner")) { it = requests.erase(it); continue; }
		RetireAutoRoutingTransportRequest(session, it.key(), method);
		it = requests.erase(it);
		AutoRoutingError(routing, method, "unknown", "Copilot Auto routing request timed out; refresh before changing routing.");
	}
	return changed;
}

bool AdvanceAutoRoutingRevision(nlohmann::json& routing)
{
	const auto revision = Count(routing, "revision");
	if (!revision || *revision == std::numeric_limits<std::int64_t>::max())
	{
		AutoRoutingError(routing, kAutoRoutingCurrent, "invalid", "Copilot Auto routing observation revision is invalid.");
		return false;
	}
	routing["revision"] = *revision + 1;
	return true;
}

bool IssueAutoRoutingRequest(AcpSessionState& session, nlohmann::json& routing, const std::string& method,
    nlohmann::json params, std::string* error_out = nullptr)
{
	auto& requests = routing["requests"];
	if (!requests.is_object() || requests.size() >= 3 ||
	    std::ranges::any_of(requests, [&](const auto& record) { return Text(record, "method") == method; }) ||
	    session.next_request_id <= 0 || session.next_request_id == std::numeric_limits<int>::max() ||
	    session.pending_request_methods.contains(session.next_request_id))
	{
		AutoRoutingError(routing, method, "invalid", "Copilot Auto routing request ownership limit reached; wait or reconnect.");
		if (error_out != nullptr) *error_out = Text(routing, "lastError");
		return false;
	}
	const int id = NextAcpRequestId(session, method);
	const std::string key = std::to_string(id);
	nlohmann::json record = {{"method", method}, {"owner", routing.at("owner")}, {"revision", routing.at("revision")},
	    {"deadline", GetAppTimeSeconds() + 30.0}};
	if (method == kAutoRoutingSwitch) record["requested"] = params.at("autoTier");
	requests[key] = std::move(record);
	if (method == kAutoRoutingSwitch)
	{
		// Reuse the existing setting gate so a later user prompt cannot race the native staging ACK.
		// Only this exact owned RPC may release it; ACP configOptions/currentValue are never fabricated.
		session.config_option_change_request_id = id;
		session.config_option_change_id = "autoRouting";
	}
	params["sessionId"] = session.session_id;
	std::string error;
	if (!WriteAcpMessage(session, uam::acp_json_rpc::Request(id, method, std::move(params)), &error))
	{
		requests.erase(key);
		RetireAutoRoutingTransportRequest(session, key, method);
		if (error.empty()) error = "Copilot Auto routing request could not be written.";
		AutoRoutingError(routing, method, "unknown", error);
		if (error_out != nullptr) *error_out = Text(routing, "lastError");
		return false;
	}
	// The shared settings timeout belongs to this newly admitted switch, not prior idle time.
	if (method == kAutoRoutingSwitch) session.last_runtime_activity_time_s = GetAppTimeSeconds();
	return true;
}

bool FlushAutoRoutingRefresh(AcpSessionState& session, const ChatSession& chat)
{
	auto* routing = AutoRoutingState(session);
	if (routing == nullptr || !AutoRoutingOwnerMatches(session, *routing, &chat) || AutoRoutingBusy(session)) return false;
	auto& requests = (*routing)["requests"];
	if (!requests.is_object() || std::ranges::any_of(requests, [](const auto& record) { return Text(record, "method") == kAutoRoutingSwitch; })) return false;
	bool changed = false;
	for (const auto& entry : {std::pair{"refreshCatalog", kAutoRoutingList}, std::pair{"refreshState", kAutoRoutingCurrent}})
	{
		if (!Flag(*routing, entry.first) || std::ranges::any_of(requests, [&](const auto& record) { return Text(record, "method") == entry.second; })) continue;
		(*routing)[entry.first] = false;
		if (IssueAutoRoutingRequest(session, *routing, entry.second, nlohmann::json::object())) changed = true;
	}
	return changed;
}

nlohmann::json TakeAutoRoutingRequest(AcpSessionState& session, const ChatSession& chat, const std::string& method, const std::string& key)
{
	auto* routing = AutoRoutingState(session);
	if (routing == nullptr) return nullptr;
	auto& requests = (*routing)["requests"];
	if (!requests.is_object()) return nullptr;
	const auto found = requests.find(key);
	if (found == requests.end()) return nullptr;
	const auto record = *found;
	if (Text(record, "method") != method) return nullptr;
	requests.erase(found);
	if (!AutoRoutingOwnerMatches(session, *routing, &chat) || Text(record, "method") != method ||
	    JsonObjectValue(record, "owner") != JsonObjectValue(*routing, "owner")) return nullptr;
	RetireAutoRoutingTransportRequest(session, key, method);
	const auto deadline = record.find("deadline");
	if (deadline != record.end() && deadline->is_number() && deadline->get<double>() <= GetAppTimeSeconds())
	{
		AutoRoutingError(*routing, method, "unknown", "Copilot Auto routing request timed out; refresh before changing routing.");
		return nullptr;
	}
	const auto revision = Count(*routing, "revision");
	if (!revision || Count(record, "revision") != revision)
	{
		(*routing)[method == kAutoRoutingList ? "refreshCatalog" : "refreshState"] = true;
		return nullptr;
	}
	return record;
}

// Invocation-local saved-image evidence. No originals or input are read/written by these controls.
nlohmann::json* SavedImageState(AcpSessionState& session)
{
	const auto found = session.copilot_native_state.find("savedImageRetry");
	return found != session.copilot_native_state.end() && found->is_object() ? &*found : nullptr;
}

const nlohmann::json* SavedImageState(const AcpSessionState& session)
{
	const auto found = session.copilot_native_state.find("savedImageRetry");
	return found != session.copilot_native_state.end() && found->is_object() ? &*found : nullptr;
}

nlohmann::json SavedImageOwner(const AcpSessionState& session, const ChatSession& chat)
{
	const auto& permission = session.saved_image_retry;
	if (!permission || !session.running || !session.initialized || !session.session_ready || !session.processing || session.cancel_requested || session.remote_source_exit_reported || session.local_stop_pending || session.remote_stop_pending || session.protocol_kind != "copilot-sdk" || session.session_id.empty() || chat.native_session_id != session.session_id || session.chat_id != permission->chat_id || session.saved_image_retry_turn_serial != session.turn_serial || session.turn_user_message_index != permission->row_index || session.turn_first_user_message_index != permission->row_index || chat.model_id.empty() || chat.model_id.size() > 512 || Text(session.copilot_native_state, "autoRoutingLifetime").empty())
		return nullptr;
	const bool remote = session.process_execution_host_id != "local";
	if (remote)
	{
		if (!permission->remote_scope) return nullptr;
		const auto& scope = *permission->remote_scope;
		if (scope.host.id.empty() || scope.host.id == "local" || scope.host.transport != "ssh" || scope.host.ssh_alias.empty() ||
		    permission->execution_host_id != scope.host.id || chat.execution_host_id != scope.host.id || session.process_execution_host_id != scope.host.id ||
		    scope.native_session_id != chat.native_session_id || scope.runtime_session_id != session.session_id ||
		    scope.process_id.empty() || scope.process_id != AcpProcessHandleLabel(session) || scope.proxy_token.empty() || scope.proxy_token != session.remote_output_delivery_token ||
		    scope.proxy_protocol < 3 || scope.proxy_protocol != session.remote_runner_protocol_version || scope.proxy_protocol != scope.host.runner_protocol_version ||
		    !execution_hosts::IsAbsoluteRemotePath(scope.host.platform, chat.workspace_directory)) return nullptr;
	}
	nlohmann::json owner = {{"chatId", session.chat_id}, {"sessionId", session.session_id}, {"savedSessionId", chat.native_session_id}, {"executionHostId", session.process_execution_host_id}, {"lifetime", Text(session.copilot_native_state, "autoRoutingLifetime")}, {"turnSerial", session.turn_serial}, {"row", permission->row_index}, {"rowCreatedAt", permission->row_created_at}, {"model", chat.model_id}, {"runtimeModel", session.current_model_id}, {"workspace", chat.workspace_directory}, {"approval", chat.approval_mode}, {"readOnly", ReadOnlyPlan(session)}, {"effort", chat.reasoning_effort}, {"commandSafety", chat.command_safety_tier}};
	if (remote)
	{
		owner["remoteOwnedRetry"] = true;
		owner["executionPlatform"] = permission->remote_scope->host.platform;
	}
	return owner;
}

bool SavedImageOwnerMatches(const AppState& app, const AcpSessionState& session, const nlohmann::json& state, const ChatSession& chat)
{
	// Result/poll callers have the full chat; preflight/snapshot callers have its effective prompt settings.
	ChatSession effective;
	effective.native_session_id = chat.native_session_id;
	effective.model_id = session.goal_turn_model_id.empty() ? chat.model_id : session.goal_turn_model_id;
	effective.workspace_directory = Text(JsonObjectValue(state, "owner"), "workspace");
	if (session.saved_image_retry && session.saved_image_retry->remote_scope)
	{
		effective.execution_host_id = chat.execution_host_id;
		effective.workspace_directory = paths::Utf8PathString(paths::ResolveWorkspaceRootPath(app, chat));
	}
	effective.approval_mode = chat.approval_mode;
	effective.reasoning_effort = chat.reasoning_effort;
	effective.command_safety_tier = chat.command_safety_tier;
	const auto owner = SavedImageOwner(session, effective);
	return !owner.is_null() && owner == JsonObjectValue(state, "owner");
}

void RejectSavedImageEvidence(AcpSessionState& session, nlohmann::json& state, const std::string& error)
{
	auto& requests = state["requests"];
	if (requests.is_object())
		for (auto it = requests.begin(); it != requests.end(); ++it)
			RetireAutoRoutingTransportRequest(session, it.key(), Text(*it, "method"));
	requests = nlohmann::json::object();
	state["phase"] = "rejected";
	state["error"] = std::string(uam::strings::Utf8Prefix(error, 2048));
}

bool PollSavedImageEvidence(const AppState& app, AcpSessionState& session, const ChatSession& chat)
{
	auto* state = SavedImageState(session);
	if (state == nullptr || Text(*state, "phase") == "rejected")
		return false;
	if (!SavedImageOwnerMatches(app, session, *state, chat))
	{
		RejectSavedImageEvidence(session, *state, "Copilot saved-image ownership changed. Retry the saved message again.");
		return true;
	}
	const auto& requests = (*state)["requests"];
	if (requests.is_object())
		for (const auto& record : requests)
		{
			const auto deadline = record.find("deadline");
			if (deadline == record.end() || !deadline->is_number() || deadline->get<double>() <= GetAppTimeSeconds())
			{
				RejectSavedImageEvidence(session, *state, "Copilot image-support verification timed out. Retry the saved message again.");
				return true;
			}
		}
	return false;
}

bool StrictImageServerVersion(const std::string& version)
{
	if (version.empty() || version.size() > 512)
		return false;
	std::size_t start = 0;
	for (int component = 0; component < 3; ++component)
	{
		const auto end = version.find('.', start);
		const auto stop = end == std::string::npos ? version.size() : end;
		if (stop == start || (component < 2) != (end != std::string::npos))
			return false;
		for (std::size_t i = start; i < stop; ++i)
			if (version[i] < '0' || version[i] > '9')
				return false;
		start = stop + 1;
	}
	return CliProviderVersionAtLeast(version, GetCopilotCliProviderRuntime().CliVersionPolicy()->minimum_version);
}

bool PositiveImageLimit(const nlohmann::json& object, const char* field)
{
	const auto found = object.find(field);
	return found != object.end() && found->is_number() && std::isfinite(found->get<double>()) && found->get<double>() > 0;
}

bool ParseSavedImageModel(const nlohmann::json& result, const std::string& model, nlohmann::json& evidence)
{
	const auto* models = uam::nlohmann_json::FindArrayField(result, "models");
	if (models == nullptr || models->size() > 256)
		return false;
	nlohmann::json selected = nullptr;
	for (const auto& candidate : *models)
	{
		if (!candidate.is_object() || Text(candidate, "id") != model)
			continue;
		if (!selected.is_null() && selected != candidate)
			return false;
		selected = candidate;
	}
	if (selected.is_null())
		return false;
	const auto capabilities = JsonObjectValue(selected, "capabilities");
	const auto vision = uam::nlohmann_json::BoolFieldStrict(JsonObjectValue(capabilities, "supports"), "vision");
	if (!vision || !*vision)
		return false;
	evidence = {{"modelId", model}, {"vision", true}, {"limits", nullptr}, {"policy", nullptr}};
	if (selected.contains("policy"))
	{
		const auto& policy = selected["policy"];
		const auto state = Text(policy, "state");
		const auto* terms = uam::nlohmann_json::FindField(policy, "terms");
		if (!policy.is_object() || (state != "enabled" && state != "unconfigured") || terms == nullptr || !AutoRoutingString(*terms, 4096, false))
			return false;
		evidence["policy"] = {{"state", state}, {"terms", *terms}};
	}
	if (capabilities.contains("limits"))
	{
		const auto& limits = capabilities["limits"];
		if (!limits.is_object())
			return false;
		if (limits.contains("vision"))
		{
			const auto& native = limits["vision"];
			const auto* mimes = uam::nlohmann_json::FindArrayField(native, "supported_media_types");
			if (!native.is_object() || mimes == nullptr || mimes->empty() || mimes->size() > 32 || !PositiveImageLimit(native, "max_prompt_images") || !PositiveImageLimit(native, "max_prompt_image_size"))
				return false;
			nlohmann::json normalized = nlohmann::json::array();
			for (const auto& mime : *mimes)
			{
				if (!AutoRoutingString(mime, 128))
					return false;
				normalized.push_back(strings::ToLowerAscii(mime.get_ref<const std::string&>()));
			}
			evidence["limits"] = {{"mimes", normalized}, {"count", native["max_prompt_images"]}, {"bytes", native["max_prompt_image_size"]}};
		}
	}
	return true;
}

bool SavedImageFormatAllowed(const nlohmann::json& evidence, const std::string& mime, std::size_t bytes, std::size_t count)
{
	const auto& limits = evidence.at("limits");
	if (!limits.is_null())
	{
		if (count > limits.at("count").get<double>() || bytes > limits.at("bytes").get<double>())
			return false;
		if (!mime.empty() && !std::ranges::any_of(limits.at("mimes"), [&](const auto& candidate) { return candidate == mime; }))
			return false;
	}
	return mime.empty() || mime == "image/png" || mime == "image/jpeg" || mime == "image/gif" || (mime == "image/webp" && !limits.is_null() && std::ranges::any_of(limits.at("mimes"), [&](const auto& candidate) { return candidate == mime; }));
}

bool IssueSavedImageQuery(AcpSessionState& session, nlohmann::json& state, const std::string& method)
{
	auto& requests = state["requests"];
	if (requests.size() >= 3 || session.next_request_id <= 0 || session.next_request_id == std::numeric_limits<int>::max() || session.pending_request_methods.contains(session.next_request_id))
		return false;
	const int id = NextAcpRequestId(session, method);
	const auto key = std::to_string(id);
	requests[key] = {{"method", method}, {"owner", state.at("owner")}, {"generation", state.at("generation")}, {"deadline", GetAppTimeSeconds() + 30.0}};
	const auto params = method == kAutoRoutingCurrent ? nlohmann::json{{"sessionId", session.session_id}} : nlohmann::json::object();
	return WriteAcpMessage(session, acp_json_rpc::Request(id, method, params));
}

bool HandleSavedImageResponse(const AppState& app, AcpSessionState& session, const ChatSession& chat, const std::string& method, const std::string& key, const nlohmann::json& result, const std::string* failure = nullptr)
{
	auto* state = SavedImageState(session);
	if (state == nullptr)
		return false;
	auto& requests = (*state)["requests"];
	const auto found = requests.find(key);
	if (found == requests.end())
		return false;
	const auto record = *found;
	if (Text(record, "method") != method)
		return false;
	requests.erase(found); // Shared response handling already retired the exact transport method.
	const auto deadline = record.find("deadline");
	if (Text(*state, "phase") != "querying" || !SavedImageOwnerMatches(app, session, *state, chat) || JsonObjectValue(record, "owner") != JsonObjectValue(*state, "owner") || record["generation"] != (*state)["generation"] || deadline == record.end() || !deadline->is_number() || deadline->get<double>() <= GetAppTimeSeconds())
	{
		RejectSavedImageEvidence(session, *state, "Copilot image-support response ownership changed or expired. Retry the saved message again.");
		return true;
	}
	if (failure != nullptr)
	{
		RejectSavedImageEvidence(session, *state, "Copilot could not verify image support: " + *failure);
		return true;
	}
	const std::string model = Text(JsonObjectValue(*state, "owner"), "model");
	nlohmann::json parsed;
	bool valid = false;
	if (method == "status.get")
	{
		const auto protocol = Count(result, "protocolVersion");
		valid = protocol && *protocol == 3 && StrictImageServerVersion(Text(result, "version"));
		// Remote input requires this stable server floor; the local version policy stays unchanged.
		if (valid && Text(JsonObjectValue(*state, "owner"), "executionHostId") != "local")
			valid = CliProviderVersionAtLeast(Text(result, "version"), "1.0.98");
		if (valid)
			parsed = {{"version", Text(result, "version")}, {"protocolVersion", 3}};
	}
	else if (method == "models.list")
		valid = ParseSavedImageModel(result, model, parsed);
	else if (method == kAutoRoutingCurrent)
	{
		valid = Text(result, "modelId") == model;
		if (valid)
			parsed = {{"modelId", model}};
	}
	if (!valid)
	{
		RejectSavedImageEvidence(session, *state, "Copilot did not confirm a compatible running server and the selected vision model. Choose an explicit vision model and retry.");
		return true;
	}
	(*state)["results"][method] = std::move(parsed);
	if (requests.empty())
	{
		const auto& results = (*state)["results"];
		if (results.size() != 3)
			RejectSavedImageEvidence(session, *state, "Copilot image-support evidence is incomplete.");
		else
		{
			(*state)["facts"] = {{"server", results.at("status.get")}, {"model", results.at("models.list")}, {"current", results.at(kAutoRoutingCurrent)}};
			(*state)["phase"] = "ready";
		}
	}
	return true;
}

void InvalidateSavedImageEvidence(AcpSessionState& session, const std::string& error)
{
	if (auto* state = SavedImageState(session); state != nullptr && Text(*state, "phase") != "rejected")
		RejectSavedImageEvidence(session, *state, error);
}

bool ParseAutoRoutingCatalog(const nlohmann::json& result, nlohmann::json& catalog, nlohmann::json& provider_default, bool& missing)
{
	missing = result.is_object() && !result.contains("auto");
	if (missing)
		return true;
	if (!result.is_object() || !result.contains("list") || !result["list"].is_array() || !result.contains("auto") || !result["auto"].is_object())
		return false;
	const auto& metadata = result["auto"];
	if (!metadata.contains("defaultTier") || !AutoRoutingString(metadata["defaultTier"], 512) || !metadata.contains("tiers") || !metadata["tiers"].is_array() || metadata["tiers"].size() > 64)
		return false;
	try
	{
		if (metadata.dump().size() > 65536)
			return false;
	}
	catch (const nlohmann::json::exception&)
	{
		return false;
	}
	catalog = nlohmann::json::array();
	std::unordered_set<std::string> ids;
	for (const auto& descriptor : metadata["tiers"])
	{
		if (!descriptor.is_object() || !descriptor.contains("id") || !AutoRoutingString(descriptor["id"], 512) || !descriptor.contains("displayName") || !AutoRoutingString(descriptor["displayName"], 256, false) || !descriptor.contains("description") || !AutoRoutingString(descriptor["description"], 2048, false) || !descriptor.contains("type") || !AutoRoutingString(descriptor["type"], 64) || !descriptor.contains("status") || !descriptor["status"].is_object() || !descriptor["status"].contains("enabled") || !descriptor["status"]["enabled"].is_boolean() || !ids.insert(descriptor["id"].get<std::string>()).second)
			return false;
		nlohmann::json status = {{"enabled", descriptor["status"]["enabled"]}};
		for (const auto& field : {std::pair{"reason", 128u}, std::pair{"message", 2048u}})
		{
			const auto value = descriptor["status"].find(field.first);
			if (value == descriptor["status"].end())
				continue;
			if (!AutoRoutingString(*value, field.second, false))
				return false;
			status[field.first] = *value;
		}
		catalog.push_back({{"id", descriptor["id"]}, {"displayName", descriptor["displayName"]}, {"description", descriptor["description"]}, {"type", descriptor["type"]}, {"status", std::move(status)}});
	}
	provider_default = {{"present", true}, {"value", metadata["defaultTier"]}};
	return true;
}

bool HandleAutoRoutingResult(AppState& app, AcpSessionState& session, const ChatSession& chat, const std::string& method, const std::string& key, const nlohmann::json& result)
{
	const auto owned = TakeAutoRoutingRequest(session, chat, method, key);
	if (owned.is_null()) { (void)FlushAutoRoutingRefresh(session, chat); return true; }
	auto& routing = *AutoRoutingState(session);
	if (method == kAutoRoutingCurrent)
	{
		uam::copilot::managed_read::ObserveModel(app, session, chat, Text(result, "modelId"));
		const auto* image = SavedImageState(session);
		if (image != nullptr && Text(*image, "phase") != "rejected" && Text(result, "modelId") != Text(JsonObjectValue(*image, "owner"), "model"))
			InvalidateSavedImageEvidence(session, "The native Copilot model changed before image delivery. Retry the saved message again.");
	}
	if (method == kAutoRoutingList)
	{
		nlohmann::json catalog, provider_default;
		bool missing = false;
		if (!ParseAutoRoutingCatalog(result, catalog, provider_default, missing))
			AutoRoutingError(routing, method, "invalid", "Copilot returned malformed Auto routing eligibility metadata.");
		else if (missing) AutoRoutingError(routing, method, "unknown", "The installed runtime did not advertise Auto routing eligibility.");
		else
		{
			routing["catalogStatus"] = "ready";
			routing["eligibleTiers"] = {{"present", true}, {"value", std::move(catalog)}};
			routing["providerDefault"] = std::move(provider_default);
		}
	}
	else if (method == kAutoRoutingCurrent)
	{
		bool valid = result.is_object();
		const auto model = AutoRoutingPresence(result, "modelId", false, valid);
		const auto effective = AutoRoutingPresence(result, "autoTier", false, valid);
		const auto pending = AutoRoutingPresence(result, "pendingAutoTier", true, valid);
		const auto activating = AutoRoutingPresence(result, "activatingAutoTier", true, valid);
		if (!valid) AutoRoutingError(routing, method, "invalid", "Copilot returned a malformed Auto routing state snapshot.");
		else if (!Flag(model, "present")) AutoRoutingError(routing, method, "unknown", "The installed runtime did not identify the current native model.");
		else
		{
			routing["currentModel"] = model;
			routing["effective"] = effective;
			routing["pending"] = pending;
			routing["activating"] = activating;
			routing["stateStatus"] = "ready";
		}
	}
	else
	{
		bool valid = result.is_object();
		const auto effective = AutoRoutingPresence(result, "effectiveAutoTier", false, valid);
		(void)effective; // An ACK never promotes the requested preference into committed state.
		const auto pending = AutoRoutingPresence(result, "pendingAutoTier", true, valid);
		const auto activating = AutoRoutingPresence(result, "activatingAutoTier", true, valid);
		const std::string status = Text(result, "status");
		if (!valid || (status != "pending" && status != "unchanged") ||
		    (status == "pending" && !Flag(pending, "present") && !Flag(activating, "present")))
			AutoRoutingError(routing, method, "invalid", "Copilot returned a malformed Auto routing switch acknowledgement.");
		else if (AdvanceAutoRoutingRevision(routing))
		{
			routing["lastAckStatus"] = status;
			routing["pending"] = pending;
			routing["activating"] = activating;
			routing["refreshState"] = true;
		}
	}
	(void)FlushAutoRoutingRefresh(session, chat);
	return true;
}

bool HandleAutoRoutingFailure(AcpSessionState& session, const ChatSession& chat, const AcpResponseFailureDetails& details)
{
	const auto owned = TakeAutoRoutingRequest(session, chat, details.failure.method, details.failure.request_id);
	if (owned.is_null()) { (void)FlushAutoRoutingRefresh(session, chat); return true; }
	auto& routing = *AutoRoutingState(session);
	const bool unsupported = details.failure.has_code && details.failure.code == -32601;
	AutoRoutingError(routing, details.failure.method, unsupported ? "unsupported" : "unknown",
	    details.formatted_error.empty() ? "Copilot rejected the Auto routing request." : details.formatted_error);
	if (unsupported && details.failure.method == kAutoRoutingSwitch)
		RecordAutoRoutingSwitchUnsupported(routing, Text(routing, "lastError"));
	// Preserve all committed/staged observations on rejection. A later explicit refresh may reconcile them.
	return true;
}

bool HandleAutoRoutingEvent(AcpSessionState& session, const ChatSession& chat, const nlohmann::json& event)
{
	const std::string type = Text(event, "type");
	if (type != "session.model_change" && type != "session.auto_tier_switch_failed" && type != "session.start" && type != "session.resume") return false;
	const auto data = JsonObjectValue(event, "data");
	auto* routing = AutoRoutingState(session);
	if (routing == nullptr || !AutoRoutingOwnerMatches(session, *routing, &chat) ||
	    !Text(event, "agentId").empty() || !Text(data, "parentToolCallId").empty() || !AcceptEvent(session, chat, event)) return true;
	bool valid = event.contains("data") && event["data"].is_object();
	const auto selected = AutoRoutingPresence(data, "autoTier", type == "session.model_change", valid);
	if (type == "session.auto_tier_switch_failed")
	{
		const auto requested = AutoRoutingPresence(data, "requestedAutoTier", true, valid);
		(void)AutoRoutingPresence(data, "effectiveAutoTier", false, valid);
		const std::string reason = Text(data, "reason");
		if (!Flag(requested, "present") || (reason != "policy_rejected" && reason != "request_failed" && reason != "setup_failed" && reason != "unsupported")) valid = false;
		if (!valid) { AutoRoutingError(*routing, kAutoRoutingCurrent, "invalid", "Copilot returned a malformed Auto routing failure event."); return true; }
		(*routing)["lastError"] = "Copilot Auto routing preference did not activate: " + reason;
		if (!AdvanceAutoRoutingRevision(*routing)) return true;
		(*routing)["stateStatus"] = reason == "unsupported" ? "unsupported" : "unknown";
		(*routing)["refreshState"] = reason != "unsupported";
	}
	else
	{
		if (!valid) { AutoRoutingError(*routing, kAutoRoutingCurrent, "invalid", "Copilot returned a malformed Auto routing model event."); return true; }
		if (uam::copilot::presentation::ValidSessionReset(data, type, session.session_id))
			session.copilot_native_state.erase("mcpPresentationOrigins");
		if (!AdvanceAutoRoutingRevision(*routing)) return true;
		if (Flag(selected, "present")) (*routing)["effective"] = selected.at("value").is_null() ? AutoRoutingAbsent() : selected;
		(*routing)["stateStatus"] = "unknown";
		(*routing)["refreshState"] = true;
		if (type == "session.resume") (*routing)["refreshCatalog"] = true;
	}
	// Do not erase potentially newer unclaimed/activating state from an event that lacks it.
	(void)FlushAutoRoutingRefresh(session, chat);
	return true;
}

std::pair<std::string, std::string> AutoRoutingDisabled(const AcpSessionState& session, const nlohmann::json* routing)
{
	if (!session.running || !session.session_ready || session.model_discovery_only || session.goal_internal_session)
		return {"not_ready", "Auto routing is available after native Chat is ready."};
	if (routing == nullptr || !AutoRoutingOwnerMatches(session, *routing) || Text(session.copilot_native_state, "autoRoutingPublicLifetime").empty())
		return {"ownership_changed", "The native session changed; refresh its model options."};
	if (AutoRoutingBusy(session)) return {"busy", "Wait for native Chat work to finish before changing Auto routing."};
	const auto requests = JsonObjectValue(*routing, "requests");
	if (!requests.empty()) return {"request_pending", "Wait for the native model option request to finish."};
	if (Flag(*routing, "switchUnsupported"))
	{
		const std::string reason = Text(*routing, "switchUnsupportedReason");
		return {"switch_unsupported", reason.empty() ? "This native Copilot session does not support session.model.switchAutoTier." : reason};
	}
	for (const char* field : {"catalogStatus", "stateStatus"})
	{
		const std::string status = Text(*routing, field);
		if (status != "ready")
		{
			const std::string error = Text(*routing, "lastError");
			return {std::string(std::string_view(field) == "catalogStatus" ? "catalog_" : "state_") +
			    (status == "unsupported" || status == "invalid" ? status : "unknown"),
			    error.empty() ? "Refresh native Auto routing metadata before changing routing." : error};
		}
	}
	const auto model = JsonObjectValue(*routing, "currentModel");
	if (!Flag(model, "present") || Text(model, "value") != "auto") return {"model_not_auto", "Auto routing preferences apply only while the native model is Auto."};
	if (Flag(JsonObjectValue(*routing, "activating"), "present")) return {"activating", "Wait for the native Auto routing activation to finish."};
	return {};
}

void RecordEventReceipt(AcpSessionState& session, ChatSession& chat, const nlohmann::json& event, Message* target = nullptr)
{
	const std::string id = Text(event, "id");
	if (id.empty()) return;
	Message* message = target ? target : CurrentAssistantMessage(chat, session);
	const std::string tool = Text(JsonObjectValue(event, "data"), "toolCallId");
	const auto owner = session.tool_call_message_indices.find(tool);
	if (!target && owner != session.tool_call_message_indices.end() && owner->second >= 0 && owner->second < static_cast<int>(chat.messages.size()))
		message = &chat.messages[static_cast<std::size_t>(owner->second)];
	if (message == nullptr) return;
	const std::string receipt = session.session_id + ":" + id;
	if (std::ranges::find(message->provider_event_ids, receipt) == message->provider_event_ids.end())
		message->provider_event_ids.push_back(receipt);
}

AcpToolCallState& TrackedTool(AcpSessionState& session, const ChatSession& chat, const std::string& id)
{
	for (AcpToolCallState& tool : session.tool_calls) if (tool.id == id) return tool;
	AcpToolCallState& tool = UpsertToolCall(session, id);
	for (std::size_t index = 0; index < chat.messages.size(); ++index)
	{
		for (const ToolCall& saved : chat.messages[index].tool_calls)
		{
			if (saved.id != id) continue;
			tool.title = saved.name;
			tool.kind = saved.kind;
			tool.status = saved.status;
			tool.args_json = saved.args_json;
			tool.content = saved.result_text;
			tool.is_sub_agent = saved.is_sub_agent;
			tool.sub_agent_id = saved.sub_agent_id;
			tool.sub_agent_title = saved.sub_agent_title;
			tool.sub_agent_children = saved.sub_agent_children;
			tool.sub_agent_children_authoritative = saved.sub_agent_children_authoritative;
			tool.approval_status = saved.approval_status;
			tool.task_id = saved.task_id;
			tool.child_activity = saved.child_activity;
			session.tool_call_message_indices[id] = static_cast<int>(index);
			return tool;
		}
	}
	return tool;
}

void AppendTrackedToolEvent(AcpSessionState& session, const std::string& id)
{
	const auto owner = session.tool_call_message_indices.find(id);
	if (owner == session.tool_call_message_indices.end() || owner->second == session.current_assistant_message_index)
		AppendToolTurnEventIfNeeded(session, id);
}

std::string AppendNativeText(AcpSessionState& session, ChatSession& chat, const nlohmann::json& data, bool delta, bool thought)
{
	const std::string key = Text(data, thought ? "reasoningId" : "messageId");
	const std::string incoming = Text(data, delta ? "deltaContent" : "content");
	if (incoming.empty()) return {};
	std::string piece = incoming;
	if (!key.empty())
	{
		nlohmann::json& streams = session.copilot_native_state[thought ? "reasoning" : "messages"];
		if (!streams.is_object()) streams = nlohmann::json::object();
		const std::string previous = Text(streams, key.c_str());
		if (delta) streams[key] = previous + incoming;
		else
		{
			if (!previous.empty() && !incoming.starts_with(previous)) return {};
			piece = incoming.substr(previous.size());
			streams[key] = incoming;
		}
	}
	if (piece.empty()) return {};
	if (thought)
	{
		(void)AppendThoughtChunk(chat, session, piece);
		if (!session.turn_events.empty())
		{
			session.turn_events.back().request_id_json = "copilot-reasoning:" + key;
			(void)SyncCurrentAssistantMessageBlocksFromTurnEvents(chat, session);
		}
		return {};
	}
	return AppendAssistantChunk(chat, session, piece, "copilot:" + key);
}

nlohmann::json& McpPresentationOrigins(AcpSessionState& session)
{
	auto& frame = session.copilot_native_state["mcpPresentationOrigins"];
	if (!frame.is_object() || Text(frame, "session") != session.session_id || Text(frame, "process") != session.last_process_id ||
	    !frame.contains("turn") || frame["turn"] != session.turn_serial)
		frame = {{"session", session.session_id}, {"process", session.last_process_id}, {"turn", session.turn_serial}, {"state", nlohmann::json::object()}};
	return frame["state"];
}

void ApplyToolEvent(AcpSessionState& session, ChatSession& chat, const nlohmann::json& event, const nlohmann::json& data, const std::string& type)
{
	const std::string id = Text(data, "toolCallId");
	if (id.empty()) return;
	if (type == "tool.execution_start") uam::copilot::presentation::Observe(McpPresentationOrigins(session), event, data);
	AcpToolCallState& tool = TrackedTool(session, chat, id);
	const std::string name = Text(data, "toolName");
	if (!name.empty())
	{
		tool.title = Text(data, "toolTitle");
		if (tool.title.empty()) tool.title = name;
		tool.kind = name == "bash" || name == "powershell" || name == "shell" ? "execute" :
		    name == "edit" || name == "create" || name == "apply_patch" ? "edit" : "other";
		tool.is_sub_agent = GetCopilotCliProviderRuntime().ProviderRecognizesSubagentTool(name);
		if (tool.is_sub_agent) tool.kind = "sub-agent";
	}
	if (data.contains("arguments")) tool.args_json = data["arguments"].dump();
	if (type == "tool.execution_start") tool.status = uam::acp_statuses::kInProgress;
	else if (type == "tool.execution_complete")
	{
		tool.status = Flag(data, "success") ? uam::acp_statuses::kCompleted : uam::acp_statuses::kFailed;
		const nlohmann::json result = JsonObjectValue(data, "result");
		tool.content = Text(result, "content");
		if (tool.content.empty()) tool.content = ContentTextFromJson(result);
		if (!Flag(data, "success"))
		{
			const std::string error = Text(JsonObjectValue(data, "error"), "message");
			if (!error.empty()) tool.content = error;
		}
		tool.content = uam::copilot::presentation::Consume(McpPresentationOrigins(session), event, data, Flag(data, "success"), tool.content);
	}
	else
	{
		const std::string output = Text(data, "partialOutput");
		if (!output.empty()) tool.content += output;
	}
	const std::string parent = Text(data, "parentToolCallId");
	const std::string agent = Text(event, "agentId");
	if (!parent.empty())
	{
		const std::string activity = tool.title;
		AcpToolCallState& owner = TrackedTool(session, chat, parent);
		owner.is_sub_agent = true;
		if (!agent.empty())
		{
			owner.task_id = agent;
			owner.sub_agent_id = BuildCopilotAgentHistoryId(session.session_id, agent, chat.execution_host_id.empty() ? "local" : chat.execution_host_id);
		}
		owner.child_activity = activity;
	}
	AppendTrackedToolEvent(session, id);
	(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
}

void ApplyCompaction(AppState& app, AcpSessionState& session, ChatSession& chat,
    const nlohmann::json& data, const std::string& identity)
{
	const bool success = Flag(data, "success");
	if (success)
	{
		std::string summary = Text(data, "summaryContent");
		if (summary.empty()) summary = Text(data, "summary");
		for (Message& message : chat.messages)
		{
			for (MessageBlock& block : message.blocks)
			{
				if (block.type != "context_compaction" || block.request_id_json != identity) continue;
				if (!summary.empty()) block.text = summary;
				block.compaction_tokens_before = Count(data, "preCompactionTokens").value_or(block.compaction_tokens_before);
				block.compaction_tokens_after = Count(data, "postCompactionTokens").value_or(block.compaction_tokens_after);
				session.context_compaction_in_progress = false;
				return;
			}
		}
		AppendContextCompactionEvent(app, session, chat, summary, identity,
		    Count(data, "preCompactionTokens").value_or(-1), Count(data, "postCompactionTokens").value_or(-1));
	}
	else
	{
		session.last_error = Text(data, "error");
		if (session.last_error.empty()) session.last_error = "Copilot context compaction did not complete.";
	}
	session.context_compaction_in_progress = false;
}

void ApplyTokenUsage(AcpSessionState& session, const nlohmann::json& data)
{
	const auto input = Count(data, "inputTokens"), output = Count(data, "outputTokens");
	if (!input || !output) return;
	const std::string id = Text(data, "apiCallId");
	if (!id.empty())
	{
		nlohmann::json& seen = session.copilot_native_state["usageCalls"];
		if (!seen.is_object()) seen = nlohmann::json::object();
		if (seen.contains(id)) return;
		if (seen.size() >= 16384) seen.clear();
		seen[id] = true;
	}
	const auto add = [](std::int64_t left, std::int64_t right)
	{
		return right > std::numeric_limits<std::int64_t>::max() - left ? std::numeric_limits<std::int64_t>::max() : left + right;
	};
	AcpTokenUsageState& usage = session.provider_usage.token_usage;
	usage.available = true;
	usage.session_id = session.session_id;
	usage.updated_at_ms = uam::time::SystemEpochMillisecondsNow();
	usage.last = {};
	usage.last.input_tokens = *input;
	usage.last.output_tokens = *output;
	usage.last.cached_input_tokens = Count(data, "cacheReadTokens").value_or(0);
	usage.last.cache_write_input_tokens = Count(data, "cacheWriteTokens").value_or(0);
	usage.last.total_tokens = add(*input, *output);
	usage.total.input_tokens = add(usage.total.input_tokens, *input);
	usage.total.output_tokens = add(usage.total.output_tokens, *output);
	usage.total.cached_input_tokens = add(usage.total.cached_input_tokens, usage.last.cached_input_tokens);
	usage.total.cache_write_input_tokens = add(usage.total.cache_write_input_tokens, usage.last.cache_write_input_tokens);
	usage.total.total_tokens = add(usage.total.total_tokens, usage.last.total_tokens);
	RecordGoalReviewTurnUsage(session, usage.last.total_tokens, true);
}

}

std::string CopilotCliProviderRuntime::AcpTurnIdentity(const uam::AcpSessionState& session) const
{
	// A confirmed completed turn must not regain replay authority on a later save.
	// Failed/unresolved turns retain their identity; restore runs before processing starts.
	// Serial 0 means no input was ever started: a save between native setup and the first
	// queued prompt must not publish a phantom turn that blocks that prompt's admission.
	if (session.turn_serial == 0) return {};
	const bool completed = session.turn_serial > 0 &&
	    session.last_settled_turn_serial == session.turn_serial && session.last_turn_outcome == "completed" &&
	    !session.processing && session.prompt_request_id == 0;
	return session.session_id.empty() || completed ? std::string{} : session.session_id + ":" + std::to_string(session.turn_serial);
}

std::size_t CopilotCliProviderRuntime::AcpBackgroundTaskCount(const uam::AcpSessionState& session) const
{
	const auto live = session.copilot_native_state.find("liveAgents");
	return live != session.copilot_native_state.end() && live->is_object() ? live->size() : 0;
}

void CopilotCliProviderRuntime::RestoreAcpTurnIdentity(uam::AcpSessionState& session, const std::string& identity) const
{
	if (identity.empty() || identity != AcpTurnIdentity(session)) return;
	session.copilot_native_state["activeTurnSerial"] = session.turn_serial;
	session.copilot_native_state["sendAccepted"] = true;
	if (!session.copilot_native_state.contains("messages") || !session.copilot_native_state.contains("reasoning"))
	{
		nlohmann::json restored = nlohmann::json::object();
		nlohmann::json reasoning = nlohmann::json::object();
		for (const AcpTurnEventState& event : session.turn_events)
		{
			if (event.type == "thought" && event.request_id_json.starts_with("copilot-reasoning:"))
			{
				const std::string key = event.request_id_json.substr(18);
				reasoning[key] = Text(reasoning, key.c_str()) + event.text;
				continue;
			}
			if (event.type != "assistant_text" || !event.request_id_json.starts_with("copilot:")) continue;
			const std::string key = event.request_id_json.substr(8);
			restored[key] = Text(restored, key.c_str()) + event.text;
		}
		session.copilot_native_state["messages"] = std::move(restored);
		session.copilot_native_state["reasoning"] = std::move(reasoning);
	}
}

void CopilotCliProviderRuntime::PrepareStructuredMessage(nlohmann::json& message) const
{
	const std::string method = Text(message, "method");
	if (method != "session.create" && method != "session.resume") return;
	const auto params_it = message.find("params");
	if (params_it == message.end() || !params_it->is_object()) return;
	nlohmann::json* params = &*params_it;
	const auto servers = params->find("mcpServers");
	if (servers == params->end() || !servers->is_array()) return;
	nlohmann::json normalized = nlohmann::json::object();
	for (const nlohmann::json& server : *servers)
	{
		if (!server.is_object()) continue;
		const std::string name = Text(server, "name");
		if (name.empty()) continue;
		nlohmann::json entry = server;
		entry.erase("name");
		if (!entry.contains("tools")) entry["tools"] = nlohmann::json::array({"*"});
		for (const char* field : {"env", "headers"})
		{
			const auto env = entry.find(field);
			if (env == entry.end() || !env->is_array()) continue;
			nlohmann::json environment = nlohmann::json::object();
			for (const nlohmann::json& value : *env)
			{
				const std::string key = Text(value, "name");
				if (!key.empty() && value.contains("value") && value["value"].is_string()) environment[key] = value["value"];
			}
			entry[field] = std::move(environment);
		}
		if (const auto cwd = entry.find("workingDirectory"); cwd != entry.end()) { entry["cwd"] = *cwd; entry.erase("workingDirectory"); }
		normalized[name] = std::move(entry);
	}
	(*params)["mcpServers"] = std::move(normalized);
}

nlohmann::json CopilotCliProviderRuntime::OnAcpBuildInitialize(uam::AcpSessionState& session, int request_id) const
{
	session.copilot_native_state = nlohmann::json::object();
	session.copilot_native_state["autoRoutingLifetime"] = PlatformServicesFactory::Instance().process_service.GenerateUuid();
	session.copilot_native_state["autoRoutingPublicLifetime"] = PlatformServicesFactory::Instance().process_service.GenerateUuid();
	return uam::acp_json_rpc::Request(request_id, "ping", nlohmann::json::object());
}

void CopilotCliProviderRuntime::OnAcpInitializeResult(uam::AcpSessionState& session, const nlohmann::json& result) const
{
	const auto version = Count(result, "protocolVersion");
	if (!version || *version != 3)
	{
		session.initialized = false;
		session.lifecycle_state = kAcpLifecycleError;
		session.last_error = "Copilot SDK server protocol 3 is required; update the CLI or UAM before retrying.";
		return;
	}
	session.copilot_native_state["protocolVersion"] = *version;
	session.agent_name = "copilot";
	session.agent_title = "GitHub Copilot";
	session.load_session_supported = true;
	session.resume_session_supported = false; // Native setup builder owns session.resume.
	session.mcp_http_supported = true;
	session.mcp_sse_supported = true;
	const int request_id = NextAcpRequestId(session, "models.list");
	if (!WriteAcpMessage(session, uam::acp_json_rpc::Request(request_id, "models.list", nlohmann::json::object())))
		session.pending_request_methods.erase(request_id);
}

nlohmann::json CopilotCliProviderRuntime::OnAcpBuildSetupRequest(int request_id, const ChatSession& chat,
    const std::string& cwd, bool, std::string& method) const
{
	method = chat.native_session_id.empty() ? "session.create" : "session.resume";
	nlohmann::json params = {{"workingDirectory", cwd}, {"clientName", "UAM"},
	    {"streaming", true}, {"requestPermission", true}, {"requestUserInput", true},
	    {"requestExitPlanMode", true},
	    {"includeSubAgentStreamingEvents", true}, {"envValueMode", "direct"},
	    {"mcpServers", uam::computer_use::AcpMcpServers(chat)}};
	if (!chat.native_session_id.empty()) params["sessionId"] = chat.native_session_id;
	if (!chat.model_id.empty()) params["model"] = chat.model_id;
	if (!chat.reasoning_effort.empty()) params["reasoningEffort"] = chat.reasoning_effort;
	return uam::acp_json_rpc::Request(request_id, method, std::move(params));
}

nlohmann::json CopilotCliProviderRuntime::OnAcpBuildPrompt(uam::AcpSessionState& session, int request_id,
    const std::string& prompt, const ChatSession& chat, std::string& method) const
{
	if (session.manual_compaction_pending) { method.clear(); return nullptr; }
	method = "session.send";
	session.copilot_native_state["activeTurnSerial"] = session.turn_serial;
	session.copilot_native_state["sendAccepted"] = false;
	session.copilot_native_state["turnObserved"] = false;
	session.copilot_native_state["messages"] = nlohmann::json::object();
	session.copilot_native_state["reasoning"] = nlohmann::json::object();
	const bool read_only = session.goal_review_turn || session.active_uam_agent_workspace_access == "read";
	const std::string mode = read_only || chat.approval_mode == uam::approval_modes::kPlanApprovalMode ? "plan" : "interactive";
	return uam::acp_json_rpc::Request(request_id, method,
	    {{"sessionId", session.session_id}, {"prompt", prompt}, {"mode", "enqueue"}, {"agentMode", mode}});
}

bool CopilotCliProviderRuntime::SupportsSavedImageRetry(const uam::AcpSessionState& session) const
{
	// Route opt-in must also work before a cold process has discovered native capabilities.
	return session.protocol_kind == "copilot-sdk";
}

bool CopilotCliProviderRuntime::SupportsRemoteOwnedImageRetry(const uam::AcpSessionState& session) const
{
	// Invocation eligibility only; fresh native version and model evidence still gates originals/input.
	return SupportsSavedImageRetry(session) && session.running && session.initialized && session.session_ready;
}

SavedImageRetryPreflight CopilotCliProviderRuntime::PrepareSavedImageRetry(uam::AcpSessionState& session, const ChatSession& chat, const std::vector<MessageAttachment>& images, std::string& error) const
{
	const auto owner = SavedImageOwner(session, chat);
	if (owner.is_null() || images.empty() || images.size() > 32 || strings::ToLowerAscii(chat.model_id) == "auto")
	{
		error = session.saved_image_retry && session.saved_image_retry->remote_scope
		    ? "Copilot saved-image Retry requires the same ready SSH session and an explicit vision model. Choose a model and retry."
		    : "Copilot saved-image Retry requires a ready local session and an explicit vision model. Choose a model and retry.";
		return SavedImageRetryPreflight::Rejected;
	}
	auto* state = SavedImageState(session);
	if (state == nullptr || Count(JsonObjectValue(*state, "owner"), "turnSerial").value_or(-1) != session.turn_serial)
	{
		const auto generation = state == nullptr ? std::optional<std::int64_t>(0) : Count(*state, "generation");
		if (!generation || *generation == std::numeric_limits<std::int64_t>::max())
		{
			error = "Copilot image-support observation generation is invalid. Reconnect before retrying.";
			return SavedImageRetryPreflight::Rejected;
		}
		if (state != nullptr)
			RejectSavedImageEvidence(session, *state, "Prior saved-image attempt ended.");
		session.copilot_native_state["savedImageRetry"] = {{"owner", owner}, {"generation", *generation + 1}, {"phase", "querying"}, {"requests", nlohmann::json::object()}, {"results", nlohmann::json::object()}, {"error", ""}};
		state = SavedImageState(session);
		for (const auto method : {"status.get", "models.list", kAutoRoutingCurrent})
			if (!IssueSavedImageQuery(session, *state, method))
			{
				RejectSavedImageEvidence(session, *state, "Copilot image-support query could not be sent. Retry the saved message again.");
				break;
			}
	}
	if (JsonObjectValue(*state, "owner") != owner)
		RejectSavedImageEvidence(session, *state, "Copilot saved-image ownership or native settings changed. Retry the saved message again.");
	if (Text(*state, "phase") == "rejected")
	{
		error = Text(*state, "error");
		return SavedImageRetryPreflight::Rejected;
	}
	if (Text(*state, "phase") != "ready")
		return SavedImageRetryPreflight::Pending;
	const auto& model = (*state)["facts"]["model"];
	for (const auto& image : images)
		if (!SavedImageFormatAllowed(model, strings::ToLowerAscii(image.mime_type), image.size_bytes, images.size()))
		{
			RejectSavedImageEvidence(session, *state, "The selected Copilot model does not support these image formats, count or sizes. The prompt was not sent.");
			error = Text(*state, "error");
			return SavedImageRetryPreflight::Rejected;
		}
	return SavedImageRetryPreflight::Ready;
}

nlohmann::json CopilotCliProviderRuntime::SavedImageRetryMetadata(const uam::AcpSessionState& session, const ChatSession& chat) const
{
	const auto* state = SavedImageState(session);
	const auto owner = SavedImageOwner(session, chat);
	if (state == nullptr || owner.is_null() || Text(*state, "phase") != "ready" || owner != JsonObjectValue(*state, "owner"))
		return {{"phase", "invalid"}}; // Never fall back to the null Claude/ACP encoding route.
	return {{"phase", "ready"}, {"owner", owner}, {"generation", state->at("generation")}, {"facts", state->at("facts")}};
}

bool CopilotCliProviderRuntime::EncodePreparedSavedImageRetry(nlohmann::json& request, const nlohmann::json& prepared, std::string& error, const std::vector<MessageAttachment>& images, const nlohmann::json& native_metadata) const
try
{
	const auto fail = [&error]()
	{
		error = "Copilot saved-image Retry could not retain complete supported image input. The prompt was not sent.";
		return false;
	};
	if (!request.is_object() || Text(request, "method") != "session.send" || !request.contains("params") || !request["params"].is_object() || !prepared.is_object() || !prepared.contains("message") || !prepared["message"].is_object() || !prepared["message"].contains("content") || !prepared["message"]["content"].is_array() || images.empty() || images.size() > 32 || Text(native_metadata, "phase") != "ready")
		return fail();
	const auto& owner = native_metadata.at("owner");
	const bool remote = Text(owner, "executionHostId") != "local";
	if (remote && (!Flag(owner, "remoteOwnedRetry") || Text(owner, "executionHostId").empty() ||
	    !execution_hosts::IsAbsoluteRemotePath(Text(owner, "executionPlatform"), Text(owner, "workspace")))) return fail();
	const auto& evidence = native_metadata.at("facts").at("model");
	const auto& params = request["params"];
	const auto& content = prepared["message"]["content"];
	if (Text(params, "sessionId").empty() || Text(params, "sessionId") != Text(owner, "sessionId") || !params.contains("prompt") || !params["prompt"].is_string() || content.size() != images.size() + 1 || !content[0].is_object() || Text(content[0], "type") != "text" || !content[0].contains("text") || !content[0]["text"].is_string() || !content[0]["text"].get_ref<const std::string&>().starts_with(params["prompt"].get_ref<const std::string&>()))
		return fail();
	nlohmann::json attachments = nlohmann::json::array();
	for (std::size_t index = 0; index < images.size(); ++index)
	{
		if (remote && (images[index].source != AttachmentSource::OwnedNativeAsset ||
		    images[index].origin_execution_host_id != Text(owner, "executionHostId"))) return fail();
		const auto& part = content[index + 1];
		const auto source = JsonObjectValue(part, "source");
		const auto mime = Text(source, "media_type");
		const auto data = Text(source, "data");
		std::string bytes;
		if (images[index].name.empty() || Text(part, "type") != "image" || Text(source, "type") != "base64" || data.empty() || data.size() > 4 * 1024 * 1024 || !base64::Decode(data, bytes) || bytes.empty() || !SavedImageFormatAllowed(evidence, mime, bytes.size(), images.size()))
			return fail();
		attachments.push_back({{"type", "blob"}, {"data", data}, {"mimeType", mime}, {"displayName", images[index].name}});
	}
	request["params"]["prompt"] = content[0]["text"];
	request["params"]["attachments"] = std::move(attachments);
	return true;
}
catch (...)
{
	error = "Copilot saved-image Retry has invalid native image evidence. The prompt was not sent.";
	return false;
}

nlohmann::json CopilotCliProviderRuntime::OnAcpBuildSteer(uam::AcpSessionState& session, int request_id, const std::string& prompt, std::string& method) const
{
	if (session.manual_compaction_pending)
	{
		method.clear();
		return nullptr;
	}
	method = "copilot/steer"; // Durable correlation is separate from ordinary session.send acknowledgements.
	return uam::acp_json_rpc::Request(request_id, "session.send", {{"sessionId", session.session_id}, {"prompt", prompt}, {"mode", "immediate"}});
}

nlohmann::json CopilotCliProviderRuntime::OnAcpBuildCompact(const uam::AcpSessionState& session, int request_id, std::string& method) const
{
	if (session.session_id.empty())
		return nullptr;
	method = "session.history.compact";
	return uam::acp_json_rpc::Request(request_id, method, {{"sessionId", session.session_id}, {"trigger", "manual"}});
}

nlohmann::json CopilotCliProviderRuntime::OnAcpBuildCancel(const uam::AcpSessionState& session, int request_id, std::string& method) const
{
	method = "session.abort";
	return uam::acp_json_rpc::Request(request_id, method, {{"sessionId", session.session_id}});
}

nlohmann::json CopilotCliProviderRuntime::OnAcpBuildPermissionResponse(const uam::AcpSessionState& session,
    const uam::AcpPendingPermissionState& pending, const std::string& option_id, bool cancelled) const
{
	return PermissionResponse(session, pending, option_id, cancelled);
}

bool CopilotCliProviderRuntime::AcpPermissionRequiresUserDecision(const uam::AcpPendingPermissionState& pending) const
{
	const nlohmann::json request = nlohmann::json::parse(pending.provider_request_params_json, nullptr, false);
	return request.is_discarded() || Flag(request, "managedApprovalRequired") ||
	    Flag(request, "requestSandboxBypass") || Flag(request, "requiresUserInteraction") || Flag(request, "mustPrompt");
}

nlohmann::json CopilotCliProviderRuntime::OnAcpBuildUserInputResponse(const uam::AcpSessionState& session,
    const std::map<std::string, std::vector<std::string>>& answers) const
{
	const nlohmann::json metadata = nlohmann::json::parse(session.pending_user_input.provider_input_json, nullptr, false);
	if (Flag(metadata, "copilotExitPlan"))
	{
		if (!answers.empty() && (answers.size() != 1 || answers.begin()->first != "plan-review" || answers.begin()->second.size() != 1))
			return uam::acp_json_rpc::ErrorResponse(StableStringToJsonRpcId(session.pending_user_input.request_id_json), -32602, "Answer the active plan-review question once.");
		std::string answer;
		for (const auto& [id, values] : answers)
			for (const std::string& value : values) { if (!answer.empty()) answer += "\n"; answer += value; }
		bool offered = false;
		for (const auto& question : session.pending_user_input.questions)
			for (const auto& option : question.options) if (option.label == answer) offered = true;
		nlohmann::json decision = {{"approved", false}};
		if (offered && answer == kFinishPlanning) decision = {{"approved", true}, {"selectedAction", "exit_only"}};
		else if (offered && answer == kApproveImplementation && !ReadOnlyPlan(session)) decision = {{"approved", true}, {"selectedAction", "interactive"}};
		else if (!answer.empty() && answer != kKeepPlanning) decision["feedback"] = answer;
		const nlohmann::json id = StableStringToJsonRpcId(session.pending_user_input.request_id_json);
		if (Flag(metadata, "copilotExitPlanEvent"))
			return {{"jsonrpc", "2.0"}, {"id", "uam-exit-plan:" + session.pending_user_input.request_id_json},
			    {"method", "session.ui.handlePendingExitPlanMode"}, {"params", {{"sessionId", session.session_id}, {"requestId", id}, {"response", decision}}}};
		return uam::acp_json_rpc::SuccessResponse(id, decision);
	}
	if (answers.empty()) return uam::acp_json_rpc::ErrorResponse(StableStringToJsonRpcId(session.pending_user_input.request_id_json), -32800, "User input cancelled.");
	std::string answer;
	for (const auto& [id, values] : answers)
		for (const std::string& value : values) { if (!answer.empty()) answer += "\n"; answer += value; }
	bool freeform = true;
	for (const AcpUserInputQuestionState& question : session.pending_user_input.questions)
		for (const AcpUserInputOptionState& option : question.options) if (option.label == answer) freeform = false;
	if (freeform && std::ranges::any_of(session.pending_user_input.questions, [](const AcpUserInputQuestionState& question) { return !question.is_other; }))
		return uam::acp_json_rpc::ErrorResponse(StableStringToJsonRpcId(session.pending_user_input.request_id_json), -32602, "Choose one of the offered answers.");
	return uam::acp_json_rpc::SuccessResponse(StableStringToJsonRpcId(session.pending_user_input.request_id_json),
	    {{"answer", answer}, {"wasFreeform", freeform}});
}

bool uam::copilot::PrepareUserInputResponse(AppState&, AcpSessionState& session, ChatSession& chat,
	const std::map<std::string, std::vector<std::string>>& answers, nlohmann::json& response, std::string* error)
{
	const nlohmann::json metadata = nlohmann::json::parse(session.pending_user_input.provider_input_json, nullptr, false);
	const auto fail = [&](const char* reason) { if (error) *error = reason; return false; };
	if (!Flag(metadata, "copilotExitPlan"))
	{
		if ((!answers.empty() && (answers.size() != 1 || answers.begin()->first != "answer" || answers.begin()->second.size() != 1)) ||
			uam::nlohmann_json::IntFieldStrict(JsonObjectValue(response, "error"), "code").value_or(0) == -32602)
			return fail("Answer the active Copilot question with one of its offered choices or permitted free-form text.");
		return true;
	}
	if (!answers.empty() && (answers.size() != 1 || answers.begin()->first != "plan-review" || answers.begin()->second.size() != 1))
		return fail("Answer the active plan-review question once.");
	// A hidden implementation choice is not free-form feedback on a read-only review.
	if ((ReadOnlyPlan(session) || chat.imported_read_only) && !answers.empty() &&
	    answers.begin()->second.front() == kApproveImplementation)
		return fail("This read-only review cannot authorize implementation.");
	const auto reviews = JsonObjectValue(session.copilot_native_state, "planReviews");
	std::size_t current_reviews = 0;
	for (const auto& review : reviews) if (Count(review, "turnSerial").value_or(-1) == session.turn_serial) ++current_reviews;
	if (current_reviews >= 128 && !reviews.contains(session.pending_user_input.request_id_json))
		return fail("Copilot exceeded the outstanding plan-review receipt limit for this turn.");
	if (CopilotCliProviderRuntime().OnAcpBuildUserInputResponse(session, answers) != response)
		return fail("The Copilot plan decision no longer matches this review request.");
	const nlohmann::json decision = Flag(metadata, "copilotExitPlanEvent") ? JsonObjectValue(JsonObjectValue(response, "params"), "response") : JsonObjectValue(response, "result");
	if (!Flag(decision, "approved")) return true;
	const std::string action = Text(decision, "selectedAction");
	bool offered = false;
	for (const auto& candidate : JsonArrayValue(metadata, "actions")) if (candidate.is_string() && candidate == action) offered = true;
	if (!offered || (action != "exit_only" && action != "interactive")) return fail("Choose one of the supported Copilot plan review actions.");
	if (action == "interactive")
	{
		if (ReadOnlyPlan(session) || chat.imported_read_only) return fail("This read-only review cannot authorize implementation.");
		if (Text(metadata, "copilotApprovalMode") != chat.approval_mode) return fail("The Copilot permission mode changed while this plan review was open; review the current plan again.");
	}
	return true; // Keep the existing ceiling until this delivered decision receives native confirmation.
}

void uam::copilot::NoteUserInputResponse(AcpSessionState& session, const nlohmann::json& response)
{
	const auto metadata = nlohmann::json::parse(session.pending_user_input.provider_input_json, nullptr, false);
	if (!Flag(metadata, "copilotExitPlan")) return;
	RecordPlanDecision(session, session.pending_user_input.request_id_json,
		uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(metadata, "copilotPlanRequestId")), response, Text(metadata, "copilotApprovalMode"));
}

void uam::copilot::NotePermissionResponse(AcpSessionState& session, const nlohmann::json& response)
{
	NotePlanResponseWritten(session, response); // This shared hook runs only after the exact response was written, including durable replay.
	if (Text(response, "method") != "session.permissions.handlePendingPermissionRequest") return;
	const nlohmann::json id = JsonRpcIdOrNull(response);
	if (!id.is_string() || !id.get_ref<const std::string&>().starts_with("uam-permission:") ||
		Text(JsonObjectValue(response, "params"), "sessionId") != session.session_id) return;
	auto& replies = session.copilot_native_state["permissionReplies"];
	if (!replies.is_object()) replies = nlohmann::json::object();
	for (auto it = replies.begin(); it != replies.end();)
		if (Count(*it, "turnSerial").value_or(-1) != session.turn_serial) it = replies.erase(it);
		else ++it;
	const std::string key = id.get<std::string>();
	if (replies.size() >= 65536 && !replies.contains(key))
	{
		const int abort = NextAcpRequestId(session, "session.abort");
		(void)WriteAcpMessage(session, uam::acp_json_rpc::Request(abort, "session.abort", {{"sessionId", session.session_id}}));
		FailAcpTurnOrSession(session, nullptr, "Copilot exceeded the native permission receipt limit; the turn was stopped to preserve reply ownership.");
		return;
	}
	replies[key] = {{"turnSerial", session.turn_serial}, {"activeTurn", session.processing}};
}

bool CopilotCliProviderRuntime::PrepareAcpUserInputResponse(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
    const std::map<std::string, std::vector<std::string>>& answers, nlohmann::json& response, std::string* error) const
{
	return uam::copilot::PrepareUserInputResponse(app, session, chat, answers, response, error);
}

void CopilotCliProviderRuntime::NoteAcpUserInputResponse(uam::AcpSessionState& session, const nlohmann::json& response) const
{
	uam::copilot::NoteUserInputResponse(session, response);
}

void CopilotCliProviderRuntime::NoteAcpPermissionResponse(uam::AcpSessionState& session, const nlohmann::json& response) const
{
	uam::copilot::NotePermissionResponse(session, response);
}

bool CopilotCliProviderRuntime::OnAcpHandleError(uam::AppState& app, uam::AcpSessionState& session,
    ChatSession& chat, const uam::acp_detail::AcpResponseFailureDetails& details) const
{
	const std::string& method = details.failure.method;
	if (uam::copilot::managed_read::HandleError(app, session, chat, details)) return true;
	if (HandleSavedImageResponse(app, session, chat, method, details.failure.request_id, nullptr, &details.formatted_error)) return true;
	if (AutoRoutingMethod(method))
	{
		const int previous_setting_request = session.config_option_change_request_id;
		const bool handled = HandleAutoRoutingFailure(session, chat, details);
		if (method == kAutoRoutingSwitch && previous_setting_request != 0 && session.config_option_change_request_id == 0)
			(void)SendQueuedPromptIfReady(app, session, chat);
		return handled;
	}
	if (method == "copilot/steer")
	{
		FinishSteer(app, session, chat, details.failure.request_id, details.formatted_error);
		return true;
	}
	if (method == "models.list")
	{
		if (app.provider_model_catalog)
			app.provider_model_catalog->RememberRefreshFailure(session.provider_id, details.formatted_error, uam::paths::ResolveWorkspaceRootPath(app, chat).generic_string(), chat.execution_host_id, session.model_catalog_scope_key);
		StopBackgroundModelDiscovery(app, session);
		return true;
	}
	if (method == "session.history.compact")
	{
		if (!session.manual_compaction_pending)
			return true;
		FinishMaintenance(session);
		session.last_error = details.formatted_error;
		return true;
	}
	if (method == "session.create" || method == "session.resume")
	{
		session.session_setup_request_id = 0;
		session.session_ready = false;
		// A rejected saved identity must never silently create an empty session.
		const std::string undelivered_prompt = session.prompt_request_id == 0 ? session.queued_prompt : std::string{};
		const auto undelivered_skill_spans = session.queued_skill_spans;
		FailAcpTurnOrSession(session, &chat, details.formatted_error);
		if (!undelivered_prompt.empty())
		{
			session.queued_prompt = undelivered_prompt;
			session.queued_skill_spans = undelivered_skill_spans;
		}
		session.queue_held = true;
		chat.acp_queue_held = true;
		if (!SaveChatQuietly(app, chat))
		{
			const std::string error = session.last_error + " The chat could not be saved; its native identity and undelivered input were retained.";
			InvalidateAcpTransport(app, session, chat, error);
			session.reconnect_pending = false;
			session.queue_held = true;
			if (!undelivered_prompt.empty()) session.queued_skill_spans = undelivered_skill_spans;
		}
		return true;
	}
	if (method == "session.model.switchTo")
	{
		for (AcpModelState& model : session.available_models)
			if (model.id == session.current_model_id) model.default_reasoning_effort = session.reasoning_change_previous_id;
		ClearAcpReasoningChangeRequest(session);
		session.last_error = details.formatted_error;
		return true;
	}
	return false;
}

bool CopilotCliProviderRuntime::OnAcpHandleResult(uam::AppState& app, uam::AcpSessionState& session,
    ChatSession& chat, const std::string& method, const std::string& request_id, const nlohmann::json& result) const
{
	if (uam::copilot::managed_read::HandleResult(app, session, chat, method, request_id, result)) return true;
	if (HandleSavedImageResponse(app, session, chat, method, request_id, result)) return true;
	if (AutoRoutingMethod(method))
	{
		const int previous_setting_request = session.config_option_change_request_id;
		const bool handled = HandleAutoRoutingResult(app, session, chat, method, request_id, result);
		if (method == kAutoRoutingSwitch && previous_setting_request != 0 && session.config_option_change_request_id == 0)
			(void)SendQueuedPromptIfReady(app, session, chat);
		return handled;
	}
	if (method == "session.create" || method == "session.resume")
	{
		session.session_setup_request_id = 0;
		const std::string id = Text(result, "sessionId");
		if (id.empty() || (method == "session.resume" && id != session.session_id))
		{
			session.session_ready = false;
			FailAcpTurnOrSession(session, &chat, "Copilot did not confirm the requested native session identity.");
			return true;
		}
		session.session_id = id;
		session.current_model_id = chat.model_id;
		session.current_mode_id = uam::approval_modes::EffectiveProviderMode(chat.approval_mode, chat.command_safety_tier);
		session.available_modes = {{"default", "Default", "Request permission before consequential operations."},
		    {"plan", "Plan", "Plan before implementing changes."}, {"acceptEdits", "Accept edits", "Allow edits within the configured command safety policy."}};
		if (!session.goal_internal_session && !session.model_discovery_only)
		{
			const std::string previous = chat.native_session_id;
			SetChatNativeSessionIdIfChanged(chat, id);
			SyncResolvedNativeSessionIdForChat(app, chat, id, previous);
			if (!SaveChatQuietly(app, chat))
			{
				const auto undelivered_skill_spans = session.queued_skill_spans;
				chat.acp_queue_held = true;
				InvalidateAcpTransport(app, session, chat, "Copilot native session could not be saved before sending the queued prompt.");
				session.reconnect_pending = false;
				session.queue_held = true;
				session.queued_skill_spans = undelivered_skill_spans;
				return true;
			}
		}
		session.session_ready = true;
		session.ignore_session_updates_until_ready = false;
		session.lifecycle_state = kAcpLifecycleReady;
		BindAutoRouting(session, chat);
		(void)ResumeQueuedUserPromptsAfterSessionSetup(app, session, chat);
		(void)FlushAutoRoutingRefresh(session, chat);
		return true;
	}
	if (method == "models.list")
	{
		const nlohmann::json* models = uam::nlohmann_json::FindArrayField(result, "models");
		std::vector<AcpModelState> parsed;
		if (models)
		{
			for (const nlohmann::json& model : *models)
			{
				AcpModelState entry;
				entry.id = Text(model, "id");
				if (entry.id.empty() || std::ranges::any_of(parsed, [&](const AcpModelState& old) { return old.id == entry.id; })) continue;
				const nlohmann::json policy = JsonObjectValue(model, "policy");
				if (Text(policy, "state") == "disabled") continue;
				entry.name = Text(model, "name");
				if (entry.name.empty()) entry.name = entry.id;
				entry.default_reasoning_effort = Text(model, "defaultReasoningEffort");
				for (const nlohmann::json& effort : JsonArrayValue(model, "supportedReasoningEfforts"))
					if (effort.is_string()) entry.supported_reasoning_efforts.push_back(effort.get<std::string>());
				const nlohmann::json limits = JsonObjectValue(JsonObjectValue(model, "capabilities"), "limits");
				entry.context_window_tokens = Count(limits, "max_context_window_tokens");
				entry.max_output_tokens = Count(limits, "max_output_tokens");
				parsed.push_back(std::move(entry));
			}
		}
		if (!parsed.empty())
		{
			session.available_models = std::move(parsed);
			RememberDiscoveredModels(app, session, chat);
			if (session.session_ready) (void)OnAcpReconcileModelOptions(app, session, chat);
		}
		else if (app.provider_model_catalog)
			app.provider_model_catalog->RememberRefreshFailure(session.provider_id, "Copilot SDK server returned no available models; check authentication and account model access.",
			    uam::paths::ResolveWorkspaceRootPath(app, chat).generic_string(), chat.execution_host_id, session.model_catalog_scope_key);
		StopBackgroundModelDiscovery(app, session);
		return true;
	}
	if (method == "session.send")
	{
		if (Text(result, "messageId").empty())
		{
			FailAcpTurnOrSession(session, &chat, "Copilot did not acknowledge the submitted message.");
			return true;
		}
		session.prompt_request_id = 0;
		session.copilot_native_state["sendAccepted"] = true;
		return true; // Request acceptance is not session.idle / model completion.
	}
	if (method == "copilot/steer")
	{
		FinishSteer(app, session, chat, request_id, Text(result, "messageId").empty() ? "Copilot did not acknowledge the steering message." : "");
		return true;
	}
	if (method == "session.history.compact")
	{
		// A late response from retired maintenance must never settle a newer user turn.
		if (!session.manual_compaction_pending)
		{
			if (session.processing) return true;
			// Direct protocol fixtures and explicit recovery can have an idle host.
			// Do not manufacture a completed operation without active maintenance.
			return true;
		}
		const nlohmann::json boundary = nlohmann::json::parse(chat.remote_manual_compaction_boundary_json, nullptr, false);
		const bool owned_boundary = boundary.is_object() && Text(boundary, "kind") == "copilot" && Text(boundary, "sessionId") == session.session_id && Text(boundary, "requestId") == request_id;
		if (Flag(session.copilot_native_state, "compactionCancelled") || (owned_boundary && Flag(boundary, "cancelled")))
		{
			FinishMaintenance(session);
			session.last_error = "Copilot context compaction was interrupted.";
			return true;
		}
		bool event_recorded = Flag(session.copilot_native_state, "manualCompactionEvent");
		std::string identity = Flag(session.copilot_native_state, "compactionActive") ? Text(session.copilot_native_state, "compactionIdentity") : std::string{};
		if (owned_boundary)
		{
			identity = Text(boundary, "identity");
			if (Flag(boundary, "completed"))
				for (const Message& message : chat.messages)
					if (std::ranges::any_of(message.blocks, [&](const MessageBlock& block) { return block.type == "context_compaction" && block.request_id_json == identity; })) event_recorded = true;
		}
		if (identity.empty()) identity = "copilot-compaction:request:" + request_id;
		if (!event_recorded || !Flag(result, "success")) ApplyCompaction(app, session, chat, result, identity);
		FinishMaintenance(session);
		session.copilot_native_state["manualCompactionEvent"] = false;
		session.copilot_native_state["compactionActive"] = false;
		(void)SaveChatQuietly(app, chat);
		(void)DrainNextQueuedAcpUserPrompt(app, session, chat);
		return true;
	}
	if (method == "session.abort")
	{
		session.cancel_request_id = 0;
		if (session.inactivity_timeout_pending)
		{
			uam::FinalizeAcpTurnInactivityTimeout(app, session, chat, session.manual_compaction_pending);
			return true;
		}
		if (session.manual_compaction_pending)
		{
			session.copilot_native_state["compactionCancelled"] = true;
			session.context_compaction_in_progress = false;
			session.last_error = "Copilot context compaction was interrupted.";
			RememberRemoteCompaction(chat, session, Text(session.copilot_native_state, "compactionIdentity"), false, true);
			(void)SaveChatQuietly(app, chat);
		}
		else if (!session.processing)
		{
			session.cancel_requested = false;
			session.cancel_requested_time_s = 0;
			(void)DrainNextQueuedAcpUserPrompt(app, session, chat);
		}
		return true; // The terminal session.idle event settles cancellation.
	}
	if (method == "session.model.switchTo")
	{
		ClearAcpReasoningChangeRequest(session);
		return true;
	}
	return false;
}

bool CopilotCliProviderRuntime::OnAcpHandleMessage(uam::AppState& app, uam::AcpSessionState& session,
    ChatSession& chat, const nlohmann::json& message, const CefRefPtr<CefBrowser>& browser) const
{
	if (session.protocol_kind == "copilot-acp") return false; // Retained old-protocol receipts only; new launches always use SDK.
	const std::string method = Text(message, "method");
	if (method.empty())
	{
		const nlohmann::json id = JsonRpcIdOrNull(message);
		if (id.is_string() && id.get_ref<const std::string&>().starts_with("uam-exit-plan:"))
		{
			auto& reviews = session.copilot_native_state["planReviews"];
			if (!reviews.is_object()) return true;
			auto owned = std::find_if(reviews.begin(), reviews.end(), [&](const auto& review)
			{
				return Text(review, "rpcId") == JsonRpcIdToStableString(id) && Flag(review, "ackPending") &&
					Count(review, "turnSerial").value_or(-1) == session.turn_serial;
			});
			if (owned == reviews.end()) return true;
			(*owned)["ackPending"] = false; // A duplicate or delayed error cannot reopen a settled acknowledgement.
			const auto owner_turn = Count(*owned, "turnSerial").value_or(-1);
			if (Flag(*owned, "activeTurn") && owner_turn > 0 &&
				session.last_settled_turn_serial >= static_cast<std::uint64_t>(owner_turn)) return true;
			if (!message.contains("error") && !Flag(JsonObjectValue(message, "result"), "success"))
			{
				reviews.erase(owned); // Native false means expired or already resolved by another client.
				return true;
			}
			if (message.contains("error") || !Flag(JsonObjectValue(message, "result"), "success"))
			{
				const bool replayed = Flag(JsonObjectValue(session.copilot_native_state, "replayedPlanReviews"), id.get_ref<const std::string&>().c_str());
				std::string error = Text(JsonObjectValue(message, "error"), "message");
				const std::string lowered = uam::strings::ToLowerAscii(error);
				reviews.erase(owned);
				if (replayed && (!message.contains("error") || lowered.find("not pending") != std::string::npos || lowered.find("already resolved") != std::string::npos || lowered.find("not found") != std::string::npos)) return true;
				if (error.empty()) error = "Copilot did not accept the plan review decision.";
				FailAcpTurnOrSession(session, &chat, error);
				(void)SaveChatQuietly(app, chat);
			}
			return true;
		}
		if (id.is_string() && id.get_ref<const std::string&>().starts_with("uam-permission:"))
		{
			auto& replies = session.copilot_native_state["permissionReplies"];
			const std::string key = id.get<std::string>();
			if (!replies.is_object()) return true;
			const auto owned = replies.find(key);
			if (owned == replies.end()) return true;
			const auto turn = Count(*owned, "turnSerial").value_or(-1);
			const bool settled = Flag(*owned, "activeTurn") && turn > 0 &&
				session.last_settled_turn_serial >= static_cast<std::uint64_t>(turn);
			replies.erase(owned); // Each admitted response owns at most one acknowledgement.
			if (turn != session.turn_serial || settled) return true;
			if (!message.contains("error") && !Flag(JsonObjectValue(message, "result"), "success")) return true; // Already resolved.
			if (message.contains("error") || !Flag(JsonObjectValue(message, "result"), "success"))
			{
				std::string error = Text(JsonObjectValue(message, "error"), "message");
				const bool replayed = Flag(JsonObjectValue(session.copilot_native_state, "replayedPermissions"), id.get_ref<const std::string&>().c_str());
				const std::string lowered = uam::strings::ToLowerAscii(error);
				if (replayed && (!message.contains("error") || uam::nlohmann_json::IntFieldStrict(JsonObjectValue(message, "error"), "code").value_or(0) == -32601 ||
				    lowered.find("not pending") != std::string::npos || lowered.find("already resolved") != std::string::npos || lowered.find("permission request not found") != std::string::npos)) return true;
				if (error.empty()) error = "Copilot did not accept the permission decision.";
				FailAcpTurnOrSession(session, &chat, error);
				(void)SaveChatQuietly(app, chat);
			}
			return true;
		}
		return false; // Shared JSON-RPC correlation owns ordinary responses.
	}
	const nlohmann::json params = JsonObjectValue(message, "params");
	if (method == "session.event" || method == "permission.request" || method == "userInput.request" || method == "exitPlanMode.request")
	{
		if (session.session_id.empty() || Text(params, "sessionId") != session.session_id)
		{
			if (message.contains("id")) (void)WriteAcpMessage(session, uam::acp_json_rpc::ErrorResponse(message["id"], -32602, "Unknown Copilot session."));
			return true;
		}
	}
	if (method == "permission.request")
	{
		RequestPermission(app, session, chat, JsonRpcIdOrNull(message), JsonObjectValue(params, "permissionRequest"), method);
		return true;
	}
	if (method == "userInput.request")
	{
		if (session.remote_source_exit_reported) return true;
		if (ReplayPersistedInteractionResponseIfMatched(app, session, chat, message)) return true;
		if (session.pending_user_input.request_id_json == JsonRpcIdToStableString(JsonRpcIdOrNull(message))) return true;
		if (uam::AcpSessionHasPendingCancel(session) || session.waiting_for_user_input || Text(params, "question").empty())
		{
			(void)WriteAcpMessage(session, uam::acp_json_rpc::ErrorResponse(JsonRpcIdOrNull(message), -32800, "User input unavailable."));
			return true;
		}
		AcpPendingUserInputState pending;
		pending.request_id_json = JsonRpcIdToStableString(JsonRpcIdOrNull(message));
		pending.provider_input_json = params.dump();
		pending.status = uam::acp_statuses::kPending;
		pending.attention_kind = "question";
		AcpUserInputQuestionState question;
		question.id = "answer";
		question.question = Text(params, "question");
		question.is_other = !params.contains("allowFreeform") || Flag(params, "allowFreeform");
		for (const nlohmann::json& choice : JsonArrayValue(params, "choices"))
			if (choice.is_string()) question.options.push_back({choice.get<std::string>(), ""});
		pending.questions.push_back(std::move(question));
		AppendUserInputTurnEventIfNeeded(session, pending.request_id_json, "");
		session.pending_user_input = std::move(pending);
		session.waiting_for_user_input = true;
		BeginAcpPendingWait(session, kAcpLifecycleWaitingUserInput);
		return true;
	}
	if (method == "exitPlanMode.request")
	{
		RequestPlanReview(app, session, chat, JsonRpcIdOrNull(message), params, false);
		return true;
	}
	if (method != "session.event")
	{
		// Unsupported callbacks must fail explicitly instead of hanging a native
		// tool indefinitely or granting capabilities the host did not advertise.
		if (message.contains("id")) SendJsonRpcError(session, message["id"], -32601, "Copilot callback is not supported by this UAM version.");
		return true;
	}
	const nlohmann::json event = JsonObjectValue(params, "event");
	const nlohmann::json data = JsonObjectValue(event, "data");
	const std::string type = Text(event, "type");
	uam::copilot::managed_read::ObserveEvent(app, session, chat, event);
	if (SavedImageState(session) != nullptr && Text(*SavedImageState(session), "phase") != "rejected" &&
	    (type == "session.model_change" || type == "session.start" || type == "session.resume" || type == "session.auto_tier_switch_failed") &&
	    Text(event, "agentId").empty() && Text(data, "parentToolCallId").empty())
	{
		const auto id = Text(event, "id");
		const auto seen = JsonObjectValue(session.copilot_native_state, "events");
		const bool recorded = !id.empty() && (seen.contains(id) || std::ranges::any_of(chat.messages, [&](const Message& row) { return std::ranges::find(row.provider_event_ids, session.session_id + ":" + id) != row.provider_event_ids.end(); }));
		if (!recorded)
			InvalidateSavedImageEvidence(session, "Native Copilot model or session state changed before image delivery. Retry the saved message again.");
	}
	if (HandleAutoRoutingEvent(session, chat, event))
		return true;
	if (type == "exit_plan_mode.requested")
	{
		RequestPlanReview(app, session, chat, uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(data, "requestId")), data, true);
		return true;
	}
	if (type == "exit_plan_mode.completed")
	{
		if (!Text(event, "agentId").empty() || !Text(data, "parentToolCallId").empty())
			return true;
		const auto request_id = uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(data, "requestId"));
		const std::string native_id = JsonRpcIdToStableString(request_id);
		nlohmann::json decision;
		auto& reviews = session.copilot_native_state["planReviews"];
		if (reviews.is_object())
		{
			const auto owned = std::find_if(reviews.begin(), reviews.end(), [&](const auto& review) { return !request_id.is_null() && Text(review, "nativeId") == native_id && Count(review, "turnSerial").value_or(-1) == session.turn_serial; });
			if (owned != reviews.end())
			{
				decision = *owned;
				reviews.erase(owned);
			}
		}
		const auto pending = nlohmann::json::parse(session.pending_user_input.provider_input_json, nullptr, false);
		if (!request_id.is_null() && Flag(pending, "copilotExitPlan") && JsonRpcIdToStableString(uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(pending, "copilotPlanRequestId"))) == native_id)
		{
			session.pending_user_input = AcpPendingUserInputState{};
			session.waiting_for_user_input = false;
			if (session.waiting_for_permission) BeginAcpPendingWait(session, kAcpLifecycleWaitingPermission);
			else { ClearAcpPendingWait(session); session.lifecycle_state = session.processing ? kAcpLifecycleProcessing : kAcpLifecycleReady; }
		}
		if (Flag(data, "approved") && Text(data, "selectedAction") == "interactive")
			(void)ConfirmPlanImplementation(app, session, chat, decision);
		return true;
	}
	// Unanswered interactions are intentionally replayed after a transport reset;
	// their pending queues and saved decisions provide their own stable-ID dedupe.
	if (type == "permission.requested")
	{
		if (!Flag(data, "resolvedByHook"))
			RequestPermission(app, session, chat, uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(data, "requestId")), JsonObjectValue(data, "permissionRequest"), type);
		return true;
	}
	if (!AcceptEvent(session, chat, event)) return true;
	const std::string parent = Text(data, "parentToolCallId");
	const std::string agent = Text(event, "agentId");
	const bool child = !parent.empty() || !agent.empty();
	// A parent-tool-only child event has no exact agent key and must not mutate root correlation.
	if ((!child || !agent.empty()) && (type == "assistant.turn_start" || type == "user.message"))
		uam::copilot::presentation::AcceptedBoundary(McpPresentationOrigins(session), event, data);
	if (child && !parent.empty() && !agent.empty())
	{
		AcpToolCallState& owner = TrackedTool(session, chat, parent);
		owner.is_sub_agent = true;
		owner.task_id = agent;
		owner.sub_agent_id = BuildCopilotAgentHistoryId(session.session_id, agent, chat.execution_host_id.empty() ? "local" : chat.execution_host_id);
		(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
		ScheduleChatSave(app, chat, 0);
	}
	const bool projects_content = type.starts_with("assistant.message") || type.starts_with("assistant.reasoning") ||
	    type.starts_with("tool.execution_") || type.starts_with("subagent.") || type == "session.compaction_complete";
	if (projects_content)
	{
		const Message* current = CurrentAssistantMessage(chat, session);
		if (current && current->provider_event_ids.size() >= 65536)
		{
			const int request_id = NextAcpRequestId(session, "session.abort");
			(void)WriteAcpMessage(session, uam::acp_json_rpc::Request(request_id, "session.abort", {{"sessionId", session.session_id}}));
			FailAcpTurnOrSession(session, &chat, "Copilot exceeded the native event receipt limit for this turn. The turn was stopped to preserve reliable recovery.");
			return true;
		}
	}
	MarkAcpRuntimeActivity(session);
	if (type == "session.mode_changed" && !child)
	{
		const std::string mode = Text(data, "newMode");
		session.copilot_native_state["nativeMode"] = mode;
		if (mode == "plan") session.current_mode_id = uam::approval_modes::kPlanApprovalMode;
		if (mode == "interactive" && !ReadOnlyPlan(session))
		{
			auto& reviews = session.copilot_native_state["planReviews"];
			if (reviews.is_object())
			{
				const auto owned = std::find_if(reviews.begin(), reviews.end(), [&](const auto& review)
				{
					return Flag(review, "callback") && Text(review, "action") == "interactive" &&
						CurrentPlanDecision(session, review);
				});
				if (owned != reviews.end())
				{
					const nlohmann::json decision = *owned;
					reviews.erase(owned);
					(void)ConfirmPlanImplementation(app, session, chat, decision);
				}
			}
		}
		return true;
	}
	if (type == "assistant.usage")
	{
		if (!child) ApplyTokenUsage(session, data);
		return true;
	}
	if (type == "session.usage_info" && !child)
	{
		const auto used = Count(data, "currentTokens"), limit = Count(data, "tokenLimit");
		if (used && limit && *limit > 0 && *used <= *limit)
		{
			session.provider_usage.context_usage = {true, uam::time::SystemEpochMillisecondsNow(), *used, *limit};
			session.provider_usage.token_usage.model_context_window = *limit;
		}
		return true;
	}
	if (type == "session.compaction_start")
	{
		session.context_compaction_in_progress = true;
		session.copilot_native_state["compactionActive"] = true;
		session.copilot_native_state["compactionCancelled"] = false;
		session.copilot_native_state["manualCompactionEvent"] = false;
		std::string id = Text(data, "compactionAttemptId");
		if (id.empty()) id = Text(event, "id");
		session.copilot_native_state["compactionIdentity"] = "copilot-compaction:" + id;
		RememberRemoteCompaction(chat, session, "copilot-compaction:" + id, false);
		return true;
	}
	if (type == "session.compaction_complete")
	{
		if (Flag(session.copilot_native_state, "compactionCancelled")) return true;
		std::string identity = Text(data, "compactionAttemptId");
		if (!identity.empty()) identity = "copilot-compaction:" + identity;
		else identity = Text(session.copilot_native_state, "compactionIdentity");
		if (identity.empty())
		{
			std::string id = Text(data, "compactionAttemptId");
			if (id.empty()) id = Text(event, "id");
			identity = "copilot-compaction:" + id;
		}
		if (Flag(data, "success"))
		{
			Message* owner = nullptr;
			for (Message& message : chat.messages)
				if (std::ranges::any_of(message.blocks, [&](const MessageBlock& block) { return block.type == "context_compaction" && block.request_id_json == identity; })) { owner = &message; break; }
			if (!owner) owner = &EnsureAssistantMessage(chat, session);
			RecordEventReceipt(session, chat, event, owner);
		}
		ApplyCompaction(app, session, chat, data, identity);
		if (Flag(data, "success")) RememberRemoteCompaction(chat, session, identity, true);
		if (session.manual_compaction_pending) session.copilot_native_state["manualCompactionEvent"] = Flag(data, "success");
		else session.copilot_native_state["compactionActive"] = false;
		(void)SaveChatQuietly(app, chat);
		return true;
	}
	if (type == "session.error")
	{
		if (child) return true;
		std::string error = Text(data, "message");
		if (error.empty()) error = "Copilot reported a native session error.";
		(void)FinalizeActiveAcpToolCallsAsFailed(chat, session);
		FailAcpTurnOrSession(session, &chat, error);
		(void)SaveChatQuietly(app, chat);
		return true;
	}
	if (type == "session.idle")
	{
		if (child || session.manual_compaction_pending || !session.processing ||
		    Count(session.copilot_native_state, "activeTurnSerial").value_or(-1) != session.turn_serial ||
		    (!Flag(session.copilot_native_state, "sendAccepted") && !Flag(session.copilot_native_state, "turnObserved"))) return true;
		const bool cancelled = uam::AcpSessionHasPendingCancel(session) || Flag(data, "aborted");
		session.copilot_native_state["liveAgents"] = nlohmann::json::object();
		if (cancelled && session.inactivity_timeout_pending)
		{
			uam::FinalizeAcpTurnInactivityTimeout(app, session, chat);
			if (browser) uam::PushStreamDone(browser, chat.id);
			return true;
		}
		(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
		if (cancelled)
		{
			(void)FinalizeActiveAcpToolCallsAsCancelled(chat, session);
			if (Message* message = CurrentAssistantMessage(chat, session)) message->interrupted = true;
		}
		if (!cancelled) RecordCompletedAcpPromptRecoveries(app, session, chat);
		CompletePromptTurnAndHandleGoalLoop(app, session, chat, kAcpLifecycleReady, browser, !cancelled);
		(void)FlushAutoRoutingRefresh(session, chat);
		if (browser) uam::PushStreamDone(browser, chat.id);
		(void)SaveChatQuietly(app, chat);
		MarkAcpChatUnseenIfBackground(app, chat);
		return true;
	}
	if (type == "subagent.started" || type == "subagent.completed" || type == "subagent.failed")
	{
		const std::string id = Text(data, "toolCallId");
		if (id.empty()) return true;
		nlohmann::json& live = session.copilot_native_state["liveAgents"];
		if (!live.is_object()) live = nlohmann::json::object();
		if (type == "subagent.started") live[id] = true;
		else live.erase(id);
		AcpToolCallState& tool = TrackedTool(session, chat, id);
		tool.is_sub_agent = true;
		tool.kind = "sub-agent";
		if (!agent.empty()) tool.task_id = agent;
		const std::string child_session = Text(data, "sessionId");
		const std::string child_id = uam::chat_ids::IsSafeStorageChatId(child_session) ? child_session : !agent.empty() ? BuildCopilotAgentHistoryId(session.session_id, agent, chat.execution_host_id.empty() ? "local" : chat.execution_host_id) : "";
		if (!child_id.empty()) tool.sub_agent_id = child_id;
		std::string title = Text(data, "agentDisplayName");
		if (title.empty()) title = Text(data, "agentName");
		if (!title.empty()) tool.sub_agent_title = tool.title = title;
		uam::copilot::ApplyChildLifecycle(tool.sub_agent_children, child_id, title, type, event);
		tool.child_activity = type == "subagent.started" ? "running" : type == "subagent.completed" ? "completed" : "failed";
		tool.status = type == "subagent.started" ? uam::acp_statuses::kInProgress : type == "subagent.completed" ? uam::acp_statuses::kCompleted : uam::acp_statuses::kFailed;
		AppendTrackedToolEvent(session, id);
		(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
		RecordEventReceipt(session, chat, event);
		ScheduleChatSave(app, chat, 0);
		return true;
	}
	if (type.starts_with("tool.execution_"))
	{
		const std::string id = Text(data, "toolCallId");
		const bool known = session.tool_call_message_indices.contains(id) || std::ranges::any_of(chat.messages, [&](const Message& message)
		    { return std::ranges::any_of(message.tool_calls, [&](const ToolCall& tool) { return tool.id == id; }); });
		if (!session.processing && !known) return true;
		session.copilot_native_state["turnObserved"] = true;
		ApplyToolEvent(session, chat, event, data, type);
		RecordEventReceipt(session, chat, event);
		ScheduleChatSave(app, chat, 0);
		return true;
	}
	if (!child && session.processing && !session.manual_compaction_pending && !uam::AcpSessionHasPendingCancel(session))
	{
		if (type == "assistant.turn_start") session.copilot_native_state["turnObserved"] = true;
		else if (type == "assistant.message_delta" || type == "assistant.message" ||
		    type == "assistant.reasoning_delta" || type == "assistant.reasoning")
		{
			session.copilot_native_state["turnObserved"] = true;
			if (type == "assistant.message") uam::copilot::presentation::ObserveRequests(McpPresentationOrigins(session), event, data);
			const std::string appended = AppendNativeText(session, chat, data, type.ends_with("_delta"), type.starts_with("assistant.reasoning"));
			RecordEventReceipt(session, chat, event);
			if (browser && !appended.empty()) uam::PushStreamToken(browser, chat.id, session.current_assistant_message_index, appended);
			ScheduleChatSave(app, chat, 0);
		}
	}
	return true;
}


bool CopilotCliProviderRuntime::OnAcpPollNativeModelOptions(uam::AppState& app, uam::AcpSessionState& session,
    ChatSession& chat) const
{
	const bool policy_changed = uam::copilot::managed_read::Poll(app, session, chat);
	const bool images_changed = PollSavedImageEvidence(app, session, chat) || policy_changed;
	auto* routing = AutoRoutingState(session);
	if (routing == nullptr || !AutoRoutingOwnerMatches(session, *routing, &chat)) return images_changed;
	// Expire even while queued input holds processing/busy; ordinary Poll admission runs after exact gate retirement.
	return ExpireAutoRoutingRequests(session, *routing) || images_changed;
}

bool CopilotCliProviderRuntime::OnAcpRefreshNativeModelOptions(uam::AppState&, uam::AcpSessionState& session,
    ChatSession& chat, std::string* error_out) const
{
	auto* routing = AutoRoutingState(session);
	if (routing == nullptr || !AutoRoutingOwnerMatches(session, *routing, &chat) || chat.imported_read_only)
	{
		if (error_out != nullptr) *error_out = "Native Chat is not ready for an owned Auto routing refresh.";
		return false;
	}
	ExpireAutoRoutingRequests(session, *routing);
	if (AutoRoutingBusy(session))
	{
		if (error_out != nullptr)
			*error_out = "Wait for native Chat work to finish before refreshing Auto routing.";
		return false;
	}
	if (!JsonObjectValue(*routing, "requests").empty())
	{
		if (error_out != nullptr)
			*error_out = "Wait for the native model option request to finish.";
		return false;
	}
	(*routing)["lastError"] = "";
	(*routing)["refreshCatalog"] = true;
	(*routing)["refreshState"] = true;
	const bool written = FlushAutoRoutingRefresh(session, chat);
	if (!written && error_out != nullptr)
		*error_out = Text(*routing, "lastError");
	return written;
}

bool CopilotCliProviderRuntime::OnAcpRequestNativeModelOption(uam::AppState&, uam::AcpSessionState& session, ChatSession& chat, std::string_view option_id, const nlohmann::json& value, const nlohmann::json& expected_ownership, std::string* error_out) const
{
	const auto reject = [&](const std::string& error) { if (error_out != nullptr) *error_out = error; return false; };
	if (option_id != "autoRouting") return reject("This provider did not offer that native model option.");
	if (!value.is_null() && !AutoRoutingString(value, 512)) return reject("Auto routing requires an exact advertised tier ID or null for Default.");
	auto* routing = AutoRoutingState(session);
	if (routing == nullptr || !AutoRoutingOwnerMatches(session, *routing, &chat) || chat.imported_read_only)
		return reject("The native session changed; refresh its model options.");
	const auto projected = OnAcpNativeModelOptionsForFrontend(session).at("autoRouting");
	if (!expected_ownership.is_object() || !projected.contains("ownership") || expected_ownership != projected.at("ownership"))
		return reject("The native session changed; refresh its model options.");
	if (chat.remote_turn_reconnect_pending || chat.remote_restart_pending || chat.remote_stop_cleanup_pending ||
	    (!chat.model_id.empty() && chat.model_id != "auto")) return reject("Native Chat is changing model or execution ownership; wait before changing Auto routing.");
	ExpireAutoRoutingRequests(session, *routing);
	const auto disabled = AutoRoutingDisabled(session, routing);
	if (!disabled.first.empty()) return reject(disabled.second);
	if (!value.is_null())
	{
		const auto catalog = JsonObjectValue(*routing, "eligibleTiers");
		if (!Flag(catalog, "present") || !catalog.contains("value") || !catalog["value"].is_array())
			return reject("The provider did not advertise Auto routing choices.");
		const auto selected = std::ranges::find_if(catalog["value"], [&](const auto& tier) { return tier.value("id", nlohmann::json()) == value; });
		if (selected == catalog["value"].end() || Text(*selected, "type") != "auto") return reject("The provider did not advertise that Auto routing preference.");
		const auto status = JsonObjectValue(*selected, "status");
		if (!Flag(status, "enabled"))
		{
			std::string error = Text(status, "message");
			if (error.empty()) error = Text(status, "reason");
			return reject(error.empty() ? "The provider disabled that Auto routing preference." : error);
		}
	}
	if (!AdvanceAutoRoutingRevision(*routing)) return reject(Text(*routing, "lastError"));
	(*routing)["lastError"] = "";
	(*routing)["lastAckStatus"] = "";
	return IssueAutoRoutingRequest(session, *routing, kAutoRoutingSwitch, {{"autoTier", value}}, error_out);
}

nlohmann::json CopilotCliProviderRuntime::OnAcpNativeModelOptionsForFrontend(const uam::AcpSessionState& session) const
{
	const auto* routing = AutoRoutingState(session);
	nlohmann::json result = {{"catalogStatus", "unknown"}, {"stateStatus", "unknown"},
	    {"eligibleTiers", AutoRoutingAbsent()}, {"providerDefault", AutoRoutingAbsent()}, {"currentModel", AutoRoutingAbsent()},
	    {"effective", AutoRoutingAbsent()}, {"pending", AutoRoutingAbsent()}, {"activating", AutoRoutingAbsent()},
	    {"requestPending", false}, {"available", false}, {"lastAckStatus", ""}, {"lastError", ""}};
	if (routing != nullptr && AutoRoutingOwnerMatches(session, *routing))
	{
		for (const char* field : {"catalogStatus", "stateStatus", "eligibleTiers", "providerDefault", "currentModel", "effective", "pending", "activating", "lastAckStatus", "lastError"})
			if (routing->contains(field)) result[field] = (*routing)[field];
		const std::string token = Text(session.copilot_native_state, "autoRoutingPublicLifetime");
		if (!token.empty()) result["ownership"] = {{"chatId", session.chat_id}, {"nativeSessionId", session.session_id},
		    {"executionHostId", session.process_execution_host_id}, {"lifetime", token}};
		result["requestPending"] = !JsonObjectValue(*routing, "requests").empty();
	}
	const auto disabled = AutoRoutingDisabled(session, routing);
	result["available"] = disabled.first.empty();
	result["disabledCode"] = disabled.first;
	result["disabledReason"] = disabled.second;
	return {{"autoRouting", std::move(result)}};
}
