#include "common/utils/time_utils.h"
#include "common/provider/claude/cli/claude_preinput_identity.h"
#include "common/runtime/acp/claude_quota_continuation.h"
#include "common/provider/claude/cli/claude_acp_message_handlers.h"
#include "common/provider/claude/cli/claude_tool_projection.h"
#include "common/provider/native_agent_history.h"
#include "common/provider/native_child_timing.h"
#include "remote/native_history_snapshot.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_session_runtime.h"

#include "app/provider_model_catalog_service.h"
#include "app/agent_run_scheduler.h"
#include "common/security/command_safety.h"
#include "cef/cef_push.h"
#include "common/config/approval_modes.h"
#include "common/runtime/acp/acp_claude_stream.h"
#include "common/runtime/acp/acp_content.h"
#include "common/runtime/acp/acp_permissions.h"
#include "common/runtime/acp/acp_statuses.h"
#include "common/utils/string_utils.h"
#include "common/utils/uuid.h"
#include "common/usage/transcript_usage.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace uam::acp_detail
{

	namespace
	{
		void ObserveManagedInitialClaudeExecution(AppState& app, AcpSessionState& session, const ChatSession& chat, const nlohmann::json& message, bool complete_init)
		{
			if (!session.managed_initial_owner)
				return;
			// Subagent result metadata does not describe the owning top-level native conversation.
			const auto parent = message.find("parent_tool_use_id");
			if (parent != message.end() && !parent->is_null())
			{
				if (!parent->is_string())
				{
					AgentRunScheduler::InitialExecutionObserved(app, session, chat, {}, {}, {}, true);
					return;
				}
				if (!parent->get_ref<const std::string&>().empty())
					return;
			}
			bool valid = true;
			const auto raw = [&](const char* name, std::size_t limit) -> std::string_view
			{
				const auto field = message.find(name);
				if (field == message.end())
				{
					if (complete_init)
						valid = false;
					return {};
				}
				if (!field->is_string())
				{
					valid = false;
					return {};
				}
				const auto& value = field->get_ref<const std::string&>();
				if (value.empty() || value.size() > limit || strings::TrimAsciiView(value) != value || std::ranges::any_of(value, [](unsigned char ch) { return ch < 32 || ch == 127; }))
				{
					valid = false;
					return {};
				}
				return value;
			};
			const auto native = raw("session_id", 256);
			const auto model = raw("model", 160);
			const auto mode = raw("permissionMode", 64);
			if (complete_init && valid)
			{
				// Same native permission semantics as the Claude structured launcher; never a current-mode fallback.
				const auto tier = command_safety::ParseTier(chat.command_safety_tier);
				const std::string_view expected = strings::TrimAsciiView(chat.approval_mode) == approval_modes::kPlanApprovalMode ? approval_modes::kPlanApprovalMode : tier == command_safety::Tier::AiReview ? approval_modes::kProviderAutoApprovalMode : tier == command_safety::Tier::AcceptEdits ? approval_modes::kAcceptEditsApprovalMode : approval_modes::kDefaultApprovalMode;
				valid = mode == expected;
			}
			// Invalid-present fields permanently withhold this attempt; result omission cannot mint missing init proof.
			AgentRunScheduler::InitialExecutionObserved(app, session, chat, valid ? native : std::string_view{}, valid ? model : std::string_view{}, valid ? mode : std::string_view{}, valid ? complete_init : true);
		}

		nlohmann::json ClaudeMaintenanceTerminalReceipt(const AcpSessionState& session, const ChatSession& chat)
		{
			if (chat.remote_manual_compaction_boundary_json.empty() || chat.remote_manual_compaction_boundary_json.size() > 2048 || chat.provider_id != "claude-cli" || session.provider_id != chat.provider_id || session.chat_id != chat.id || !chat.remote_process_exists || chat.execution_host_id.empty() || chat.execution_host_id == "local" || session.process_execution_host_id != chat.execution_host_id || chat.remote_process_control_token.empty() || session.remote_output_delivery_token != chat.remote_process_control_token || session.session_id != chat.native_session_id || !uam::uuid::IsCanonicalUuid(session.session_id))
				return {};
			const auto receipt = nlohmann::json::parse(chat.remote_manual_compaction_boundary_json, nullptr, false);
			if (!receipt.is_object() || JsonDiagnosticStringValue(receipt, "kind") != "claude_manual_compaction_terminal" || JsonDiagnosticStringValue(receipt, "providerId") != chat.provider_id || JsonDiagnosticStringValue(receipt, "sessionId") != session.session_id || JsonDiagnosticStringValue(receipt, "executionHostId") != chat.execution_host_id || JsonDiagnosticStringValue(receipt, "processControlToken") != chat.remote_process_control_token || !uam::uuid::IsCanonicalUuid(JsonDiagnosticStringValue(receipt, "resultId")) || uam::nlohmann_json::IntFieldStrict(receipt, "requestId").value_or(0) <= 0 || JsonDiagnosticStringValue(receipt, "deliveryId").empty())
				return {};
			return receipt;
		}

		nlohmann::json MakeClaudeMaintenanceTerminalReceipt(const AcpSessionState& session, const ChatSession& chat, const std::string& result_id, const std::string& boundary_id)
		{
			if (!uam::uuid::IsCanonicalUuid(result_id) || chat.provider_id != "claude-cli" || session.provider_id != chat.provider_id || session.chat_id != chat.id || !chat.remote_process_exists || chat.execution_host_id.empty() || chat.execution_host_id == "local" || session.process_execution_host_id != chat.execution_host_id || chat.execution_host_id.size() > 128 || chat.remote_process_control_token.empty() || chat.remote_process_control_token.size() > 256 || session.remote_output_delivery_token != chat.remote_process_control_token || session.session_id != chat.native_session_id || !uam::uuid::IsCanonicalUuid(session.session_id))
				return {};
			for (const auto& request : chat.remote_pending_requests)
			{
				if (request.method != "claude/compact" || request.response_consumed || request.request_id <= 0 || request.delivery_id.empty() || request.delivery_id.size() > 128 || !session.pending_request_methods.contains(request.request_id) || session.pending_request_methods.at(request.request_id) != "claude/compact")
					continue;
				return {{"kind", "claude_manual_compaction_terminal"}, {"providerId", chat.provider_id}, {"sessionId", session.session_id}, {"executionHostId", chat.execution_host_id}, {"processControlToken", chat.remote_process_control_token}, {"resultId", result_id}, {"requestId", request.request_id}, {"deliveryId", request.delivery_id}, {"boundaryId", boundary_id}};
			}
			return {};
		}

		/// <summary>Projects only declared generic subscription windows from the owning native session.</summary>
		void ObserveClaudeAllowance(AcpSessionState& session, const ChatSession& chat, const nlohmann::json& message)
		{
			const nlohmann::json* identity = uam::nlohmann_json::FindStringField(message, "session_id");
			if (session.provider_id != "claude-cli" || chat.provider_id != "claude-cli" || session.chat_id != chat.id ||
			    session.goal_internal_session || session.model_discovery_only || session.session_id.empty() ||
			    session.session_id != chat.native_session_id || identity == nullptr || identity->get_ref<const std::string&>() != session.session_id)
				return;
			const nlohmann::json* info = uam::nlohmann_json::FindObjectField(message, "rate_limit_info");
			if (info == nullptr) return;
			const std::string status = JsonDiagnosticStringValue(*info, "status");
			if (status != "allowed" && status != "allowed_warning" && status != "rejected") return;
			const std::string type = JsonDiagnosticStringValue(*info, "rateLimitType");
			if (type != "five_hour" && type != "seven_day") return;
			const nlohmann::json* utilization = uam::nlohmann_json::FindField(*info, "utilization");
			if (utilization == nullptr || !utilization->is_number()) return;
			const double fraction = utilization->get<double>();
			if (!std::isfinite(fraction) || fraction < 0 || fraction > 1) return;
			AcpRateLimitsState& limits = session.provider_usage.rate_limits;
			for (AcpRateLimitWindowState* observed : {&limits.primary, &limits.secondary})
				if (observed->available && observed->updated_at_ms == 0) observed->updated_at_ms = limits.updated_at_ms;
			AcpRateLimitWindowState& window = type == "five_hour" ? limits.primary : limits.secondary;
			// The SDK forwards a fraction; unlike the OAuth usage endpoint, it does not send an integer percentage.
			// One ULP corrects products such as 0.29 * 100 without rounding a partial percentage up to exhaustion.
			const int percent = static_cast<int>(std::floor(std::nextafter(fraction * 100, 101.0)));
			window.used_percent = fraction < 1 ? std::min(percent, 99) : 100;
			window.window_duration_minutes = type == "five_hour" ? 300 : 10080;
			if (const nlohmann::json* reset = uam::nlohmann_json::FindField(*info, "resetsAt"); reset != nullptr && reset->is_number_integer())
			{
				constexpr int64_t maximum_reset = 253402300799;
				if ((!reset->is_number_unsigned() || reset->get<uint64_t>() <= static_cast<uint64_t>(maximum_reset)) &&
				    reset->get<int64_t>() >= 0 && reset->get<int64_t>() <= maximum_reset)
					window.resets_at = reset->get<int64_t>();
			}
			window.available = true;
			window.updated_at_ms = uam::time::SystemEpochMillisecondsNow();
			limits.available = true;
			// The shared wire timestamp describes every visible window, including the older one in a partial update.
			limits.updated_at_ms = window.updated_at_ms;
			for (const AcpRateLimitWindowState* observed : {&limits.primary, &limits.secondary})
				if (observed->available) limits.updated_at_ms = std::min(limits.updated_at_ms, observed->updated_at_ms);
		}

		/// <summary>Render Claude result blocks without exposing binary payloads or protocol envelopes.</summary>
		std::string ClaudeToolResultText(const nlohmann::json& content)
		{
			if (content.is_string())
			{
				return content.get<std::string>();
			}
			if (content.is_array())
			{
				std::vector<std::string> pieces;
				for (const nlohmann::json& block : content)
				{
					pieces.push_back(ClaudeToolResultText(block));
				}
				return uam::strings::JoinNonEmpty(pieces, "\n");
			}
			if (!content.is_object())
			{
				return content.is_null() ? std::string{} : content.dump();
			}
			const std::string type = JsonDiagnosticStringValue(content, "type");
			if (type == "text")
			{
				return JsonDiagnosticStringValue(content, "text");
			}
			if (type == "image")
			{
				return "[Image]";
			}
			if (type == "document")
			{
				return "[Document]";
			}
			return type.empty() ? content.dump(2) : "[" + type + "]";
		}

		/// <summary>Late task/child events update their original message even after the next turn starts.</summary>
		AcpToolCallState& RestoreClaudeTool(AcpSessionState& session, const ChatSession& chat, const std::string& id)
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

		/// <summary>Only an attested native child can supply its own terminal duration.</summary>
		void ApplyClaudeChildDuration(const AcpSessionState& session, const ChatSession& chat,
		    AcpToolCallState& tool, const nlohmann::json& message, const std::string& subtype)
		{
			const std::string task_id = JsonDiagnosticStringValue(message, "task_id");
			const std::string host = chat.execution_host_id.empty() ? "local" : chat.execution_host_id;
			if (!tool.is_sub_agent || session.provider_id != "claude-cli" || chat.provider_id != session.provider_id ||
			    session.chat_id != chat.id || session.session_id != chat.native_session_id ||
			    JsonDiagnosticStringValue(message, "session_id") != session.session_id ||
			    !uam::uuid::IsCanonicalUuid(session.session_id) || !uam::remote::native_history::SafeId(task_id)) return;
			const auto reference = uam::native_agents::ParseHistoryId(tool.sub_agent_id);
			if (!reference || reference->provider_id != "claude-cli" || reference->parent_session_id != session.session_id ||
			    reference->agent_id != task_id || reference->execution_host_id != host) return;

			auto found = std::ranges::find(tool.sub_agent_children, tool.sub_agent_id, &SubAgentChild::id);
			if (found == tool.sub_agent_children.end())
			{
				tool.sub_agent_children.push_back({tool.sub_agent_id, tool.sub_agent_title.empty() ? tool.title : tool.sub_agent_title, ""});
				found = std::prev(tool.sub_agent_children.end());
			}
			SubAgentChild& child = *found;
			const auto terminal = [](const std::string& status)
			{
				return status == uam::acp_statuses::kCompleted || status == uam::acp_statuses::kFailed || status == uam::acp_statuses::kCancelled;
			};
			std::string observed_status;
			if (subtype == "task_started") observed_status = uam::acp_statuses::kRunning;
			else if (subtype == "task_progress") observed_status = child.status.empty() ? uam::acp_statuses::kRunning : child.status;
			else
			{
				observed_status = JsonDiagnosticStringValueOr(JsonObjectValue(message, "patch"), "status", JsonDiagnosticStringValue(message, "status"));
				if (observed_status == "stopped" || observed_status == "killed") observed_status = uam::acp_statuses::kCancelled;
			}
			if (!terminal(child.status)) child.status = observed_status;
			// Native progress supplies only a sampled duration. It is not a start epoch
			// or a terminal receipt, and cannot create a live clock or replace a result.
			if (subtype != "task_notification" || !terminal(observed_status) || child.status != observed_status || child.duration_ms) return;
			child.duration_ms = uam::native_agents::ReportedDurationMilliseconds(JsonObjectValue(message, "usage"), "duration_ms");
		}

		bool HandleClaudeTaskEvent(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
		{
			const std::string subtype = JsonDiagnosticStringValue(message, "subtype");
			if (subtype != "task_started" && subtype != "task_progress" && subtype != "task_notification" && subtype != "task_updated") return false;
			if (subtype == "task_updated")
			{
				const std::string status = JsonDiagnosticStringValueOr(JsonObjectValue(message, "patch"), "status", JsonDiagnosticStringValue(message, "status"));
				if (status != "completed" && status != "failed" && status != "stopped" && status != "cancelled" && status != "killed" && status != "pending" && status != "running" && status != "paused") return true;
			}
			const std::string task_id = JsonDiagnosticStringValue(message, "task_id");
			std::string tool_id = JsonDiagnosticStringValue(message, "tool_use_id");
			if (tool_id.empty() && !task_id.empty())
			{
				for (const AcpToolCallState& tool : session.tool_calls) if (tool.task_id == task_id) tool_id = tool.id;
				if (tool_id.empty()) for (const Message& saved : chat.messages)
					for (const ToolCall& tool : saved.tool_calls) if (tool.task_id == task_id) tool_id = tool.id;
			}
			if (tool_id.empty())
			{
				if (task_id.empty()) return true;
				tool_id = "claude-task:" + task_id;
			}
			// Reject conflicting identity before restoring or mutating the launch.
			// Normal progress uses the live association without rescanning history.
			if (!task_id.empty())
			{
				const auto known = std::ranges::find(session.tool_calls, tool_id, &AcpToolCallState::id);
				if (known != session.tool_calls.end() && !known->task_id.empty())
				{
					if (known->task_id != task_id) return true;
				}
				else
				{
					for (const Message& saved : chat.messages)
						for (const ToolCall& launch : saved.tool_calls)
							if (launch.id == tool_id && !launch.task_id.empty() && launch.task_id != task_id) return true;
				}
			}
			AcpToolCallState& tool = RestoreClaudeTool(session, chat, tool_id);
			const bool first_task_event = tool.task_id.empty() && !task_id.empty();
			if (!task_id.empty()) tool.task_id = task_id;
			if (tool.title.empty()) tool.title = JsonDiagnosticStringValueOr(message, "description", "Background task");
			if (JsonDiagnosticStringValue(message, "task_type") == "local_agent")
			{
				tool.is_sub_agent = true;
				if (JsonDiagnosticStringValue(message, "session_id") == session.session_id && session.session_id == chat.native_session_id && uam::uuid::IsCanonicalUuid(session.session_id) && uam::remote::native_history::SafeId(task_id))
					tool.sub_agent_id = uam::native_agents::BuildHistoryId("claude-cli", session.session_id, task_id, chat.execution_host_id.empty() ? "local" : chat.execution_host_id);
			}
			if (subtype == "task_notification" || subtype == "task_updated")
			{
				const nlohmann::json patch = JsonObjectValue(message, "patch");
				const std::string status = JsonDiagnosticStringValueOr(patch, "status", JsonDiagnosticStringValue(message, "status"));
				if (status == "completed") tool.status = uam::acp_statuses::kCompleted;
				else if (status == "failed") tool.status = uam::acp_statuses::kFailed;
				else if (status == "stopped" || status == "cancelled" || status == "killed") tool.status = uam::acp_statuses::kCancelled;
				else if (status == "pending" || status == "running" || status == "paused")
				{
					if (tool.status.empty() || tool.status == "paused" || uam::acp_statuses::IsActiveStatus(tool.status) ||
					    (first_task_event && !uam::AcpSessionHasPendingCancel(session))) tool.status = status;
				}
				else return true;
				const std::string summary = JsonDiagnosticStringValue(message, "summary");
				if (!summary.empty() && !tool.content.ends_with(summary))
				{
					if (!tool.content.empty()) tool.content += "\n\n";
					tool.content += summary;
				}
			}
			else if (tool.status.empty() || tool.status == "paused" || uam::acp_statuses::IsActiveStatus(tool.status) ||
			         (first_task_event && !uam::AcpSessionHasPendingCancel(session))) tool.status = uam::acp_statuses::kRunning;
			if (subtype == "task_progress" && uam::acp_statuses::IsActiveStatus(tool.status))
			{
				std::string activity = JsonDiagnosticStringValue(message, "description");
				if (activity.empty()) activity = JsonDiagnosticStringValue(message, "last_tool_name");
				if (!activity.empty() && !tool.child_activity.ends_with(activity))
				{
					if (!tool.child_activity.empty()) tool.child_activity += "\n\n";
					tool.child_activity += activity;
				}
			}
			if (!tool.task_id.empty())
			{
				if (uam::acp_statuses::IsActiveStatus(tool.status) || uam::strings::TrimAsciiView(tool.status) == "paused")
				{
					if (!task_id.empty()) session.claude_live_background_task_ids.insert(tool.task_id);
				}
				else session.claude_live_background_task_ids.erase(tool.task_id);
			}
			ApplyClaudeChildDuration(session, chat, tool, message, subtype);
			AppendToolTurnEventIfNeeded(session, tool_id);
			(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
			if (subtype == "task_progress" || (subtype == "task_updated" &&
			    (uam::acp_statuses::IsActiveStatus(tool.status) || tool.status == "paused")))
				ScheduleChatSave(app, chat, 0.0);
			else SaveChatQuietly(app, chat);
			MarkAcpChatUnseenIfBackground(app, chat);
			return true;
		}

		/// <summary>Child narration and tools belong to the launching tool, never the parent timeline.</summary>
		bool HandleClaudeChildMessage(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
		{
			const std::string parent = JsonDiagnosticStringValue(message, "parent_tool_use_id");
			if (parent.empty()) return false;
			const std::string type = JsonDiagnosticStringValue(message, "type");
			if (type != "assistant" && type != "user") return false;
			bool known_parent = std::ranges::any_of(session.tool_calls, [&parent](const AcpToolCallState& tool) { return tool.id == parent; });
			if (!known_parent)
			{
				for (const Message& saved : chat.messages)
					if (std::ranges::any_of(saved.tool_calls, [&parent](const ToolCall& tool) { return tool.id == parent; })) { known_parent = true; break; }
			}
			if (!known_parent) return true;
			const std::string identity = JsonDiagnosticStringValue(message, "uuid");
			if (!identity.empty() && !session.claude_seen_child_messages.insert(identity).second) return true;
			AcpToolCallState& tool = RestoreClaudeTool(session, chat, parent);
			tool.is_sub_agent = true;
			if (tool.title.empty()) tool.title = "Sub-agent";
			const nlohmann::json content = uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(JsonObjectValue(message, "message"), "content"));
			if (content.is_string() && !content.get_ref<const std::string&>().empty())
			{
				if (!tool.child_activity.empty()) tool.child_activity += "\n\n";
				tool.child_activity += content.get_ref<const std::string&>();
			}
			for (const nlohmann::json& block : JsonArrayValue(JsonObjectValue(message, "message"), "content"))
			{
				const std::string block_type = JsonDiagnosticStringValue(block, "type");
				std::string activity;
				if (block_type == "text") activity = ContentTextFromJson(block);
				else if (block_type == "thinking") activity = JsonDiagnosticStringValue(block, "thinking");
				else if (block_type == "tool_use") activity = uam::claude::ToolTitle(JsonDiagnosticStringValue(block, "name"), JsonObjectValue(block, "input"));
				else if (block_type == "tool_result")
				{
					const nlohmann::json output = uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(block, "content"));
					activity = ClaudeToolResultText(output);
				}
				if (!activity.empty())
				{
					if (!tool.child_activity.empty()) tool.child_activity += "\n\n";
					tool.child_activity += activity;
				}
			}
			AppendToolTurnEventIfNeeded(session, parent);
			(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
			ScheduleChatSave(app, chat, 0.0);
			return true;
		}

		void HandleClaudeControlRequest(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
		{
			const nlohmann::json request = JsonObjectValue(message, "request");
			const std::string id = uam::nlohmann_json::TrimmedStringValue(message, {"request_id"});
			if (id.empty())
				return;
			const std::string subtype = JsonDiagnosticStringValue(request, "subtype");
			if (subtype != "can_use_tool")
			{
				(void)WriteAcpMessage(session, {{"type", "control_response"}, {"response", {{"subtype", "error"}, {"request_id", id}, {"error", "Unsupported Claude control request: " + subtype}}}});
				return;
			}
			if (uam::nlohmann_json::FindObjectField(request, "input") == nullptr || JsonDiagnosticStringValue(request, "tool_name").empty())
			{
				(void)WriteAcpMessage(session, uam::acp_claude_stream::ControlResponse(id, {{"behavior", "deny"}, {"message", "Malformed Claude tool request."}}));
				return;
			}
			const nlohmann::json input = JsonObjectValue(request, "input");
			const std::string tool_name = JsonDiagnosticStringValue(request, "tool_name");
			const std::string tool_id = JsonDiagnosticStringValueOr(request, "tool_use_id", id);
			if (uam::AcpSessionHasPendingCancel(session))
			{
				(void)WriteAcpMessage(session, uam::acp_claude_stream::ControlResponse(id, {{"behavior", "deny"}, {"message", "Turn cancelled."}}));
				return;
			}
			AcpPendingPermissionState permission;
			if (tool_name != "AskUserQuestion")
			{
				permission.request_id_json = JsonRpcIdToStableString(id);
				permission.provider_request_method = "can_use_tool";
				permission.provider_request_kind = "claude-tool";
				permission.provider_input_json = input.dump();
				permission.tool_call_id = tool_id;
				permission.title = uam::claude::ToolTitle(tool_name, input);
				permission.kind = uam::claude::ToolKind(tool_name);
				permission.status = uam::acp_statuses::kPending;
				permission.content = tool_name == "Bash" ? JsonDiagnosticStringValue(input, "command") : input.dump(2);
				if (tool_name == "Bash")
					if (const nlohmann::json* command = uam::nlohmann_json::FindStringField(input, "command"))
						permission.execution_command = command->get_ref<const std::string&>();
				permission.options = {{"allow", "Allow", "allow_once"}, {"deny", "Deny", "reject_once"}};
				if (ReplayPersistedInteractionResponseIfMatched(app, session, chat, {{"id", id}}, &permission)) return;
			}
			AcpToolCallState& tool = UpsertToolCall(session, tool_id);
			tool.title = uam::claude::ToolTitle(tool_name, input);
			tool.kind = uam::claude::ToolKind(tool_name);
			tool.is_sub_agent = tool.kind == "sub-agent";
			tool.args_json = input.dump();
			tool.status = uam::acp_statuses::kPending;
			AppendToolTurnEventIfNeeded(session, tool_id);
			if (tool_name == "AskUserQuestion")
			{
				if (!session.pending_user_input.request_id_json.empty())
				{
					(void)WriteAcpMessage(session, uam::acp_claude_stream::ControlResponse(id, {{"behavior", "deny"}, {"message", "Another question is awaiting an answer."}}));
					return;
				}
				AcpPendingUserInputState pending;
				pending.request_id_json = JsonRpcIdToStableString(id);
				pending.provider_input_json = input.dump();
				pending.item_id = tool_id;
				pending.status = uam::acp_statuses::kPending;
				pending.attention_kind = "question";
				for (const nlohmann::json& item : JsonArrayValue(input, "questions"))
				{
					if (!item.is_object())
						continue;
					AcpUserInputQuestionState question;
					question.id = std::to_string(pending.questions.size());
					question.header = JsonDiagnosticStringValue(item, "header");
					question.question = JsonDiagnosticStringValue(item, "question");
					question.is_other = true;
					question.is_multiple = item.contains("multiSelect") && item["multiSelect"].is_boolean() && item["multiSelect"].get<bool>();
					for (const nlohmann::json& option : JsonArrayValue(item, "options"))
					{
						if (!option.is_object())
							continue;
						question.options.push_back({JsonDiagnosticStringValue(option, "label"), JsonDiagnosticStringValue(option, "description")});
					}
					if (!question.question.empty())
						pending.questions.push_back(std::move(question));
				}
				if (pending.questions.empty())
				{
					(void)WriteAcpMessage(session, uam::acp_claude_stream::ControlResponse(id, {{"behavior", "deny"}, {"message", "Claude sent no answerable questions."}}));
					return;
				}
				AppendUserInputTurnEventIfNeeded(session, pending.request_id_json, tool_id);
				session.pending_user_input = std::move(pending);
				session.waiting_for_user_input = true;
				BeginAcpPendingWait(session, kAcpLifecycleWaitingUserInput);
			}
			else
			{
				ApplyCommandSafetyDecision(app, chat, permission);
				QueueAcpPermission(app, session, chat, std::move(permission));
			}
			(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
			SaveChatQuietly(app, chat);
			MarkAcpChatUnseenIfBackground(app, chat);
		}
	} // namespace

void HandleClaudeAssistantMessage(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message, CefRefPtr<CefBrowser> browser)
{
	if (claude_preinput::Consume(app, session, chat, message)) return;
	const std::string identity = JsonDiagnosticStringValue(message, "uuid");
	if (JsonDiagnosticStringValue(message, "parent_tool_use_id").empty() && !identity.empty() &&
	    !session.claude_seen_parent_messages.insert(identity).second) return;
	const nlohmann::json assistant_message = JsonObjectValue(message, "message");
	const nlohmann::json content = uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(assistant_message, "content"));
	const bool child_message = !JsonDiagnosticStringValue(message, "parent_tool_use_id").empty();
	if (!content.is_array())
	{
		if (child_message) return;
		const std::string fallback_text = ClaudeContentTextFromMessage(assistant_message);
		if (!fallback_text.empty())
		{
			const std::string appended = AppendAssistantChunk(chat, session, fallback_text);
			if (browser && !appended.empty())
			{
				uam::PushStreamToken(browser, chat.id, session.current_assistant_message_index, appended);
			}
			ScheduleChatSave(app, chat, 0.5);
		}
		return;
	}

	bool changed = false;
	for (const nlohmann::json& item : content)
	{
		if (!item.is_object())
		{
			continue;
		}

		const std::string type = JsonDiagnosticStringValue(item, "type");
		if (type == uam::acp_content::kTextType)
		{
			if (child_message)
				continue;
			const std::string text = ContentTextFromJson(item);
			if (!text.empty())
			{
				const std::string appended = AppendAssistantChunk(chat, session, text);
				if (browser && !appended.empty())
				{
					uam::PushStreamToken(browser, chat.id, session.current_assistant_message_index, appended);
				}
				changed = true;
			}
			continue;
		}

		if (type == uam::acp_claude_stream::kContentThinking)
		{
			if (child_message)
				continue;
			const std::string thought = JsonDiagnosticStringValue(item, "thinking");
			if (!thought.empty())
			{
				changed = AppendThoughtChunk(chat, session, thought) || changed;
			}
			continue;
		}

		if (type == uam::acp_claude_stream::kContentToolUse)
		{
			const std::string tool_id = JsonDiagnosticStringValue(item, "id");
			if (tool_id.empty())
			{
				continue;
			}

			AcpToolCallState& tool_call = RestoreClaudeTool(session, chat, tool_id);
			const std::string tool_name = JsonDiagnosticStringValueOr(item, "name", tool_call.kind);
			tool_call.kind = uam::claude::ToolKind(tool_name);
			tool_call.title = uam::claude::ToolTitle(tool_name, JsonObjectValue(item, "input"));
			tool_call.is_sub_agent = tool_call.kind == "sub-agent" || tool_call.is_sub_agent;
			if (tool_call.status.empty() || uam::acp_statuses::IsActiveStatus(tool_call.status)) tool_call.status = uam::acp_statuses::kRunning;
			if (const nlohmann::json* input = uam::nlohmann_json::FindField(item, "input"); input != nullptr)
			{
				tool_call.args_json = input->dump();
			}
			ApplySubAgentMetadata(tool_call, item, ProviderRuntimeRegistry::ResolveById(session.provider_id));
			AppendToolTurnEventIfNeeded(session, tool_id);
			changed = SyncAcpToolCallsToAssistantMessage(chat, session, true) || changed;
		}
	}

	if (changed)
	{
		SaveChatQuietly(app, chat);
		MarkAcpChatUnseenIfBackground(app, chat);
	}
}

void HandleClaudeUserMessage(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
{
	if (claude_preinput::Consume(app, session, chat, message)) return;
	const nlohmann::json user_message = JsonObjectValue(message, "message");
	const nlohmann::json content = JsonArrayValue(user_message, "content");
	if (!content.is_array())
	{
		return;
	}

	bool changed = false;
	for (const nlohmann::json& item : content)
	{
		if (!item.is_object())
		{
			continue;
		}

		if (JsonDiagnosticStringValue(item, "type") != uam::acp_claude_stream::kContentToolResult)
		{
			continue;
		}

		const std::string tool_id = JsonDiagnosticStringValue(item, "tool_use_id");
		if (tool_id.empty())
		{
			continue;
		}

		AcpToolCallState& tool_call = RestoreClaudeTool(session, chat, tool_id);
		const std::string native_agent = JsonDiagnosticStringValue(JsonObjectValue(message, "tool_use_result"), "agentId");
		if (tool_call.is_sub_agent && JsonDiagnosticStringValue(message, "session_id") == session.session_id && session.session_id == chat.native_session_id && uam::uuid::IsCanonicalUuid(session.session_id) && uam::remote::native_history::SafeId(native_agent))
			tool_call.sub_agent_id = uam::native_agents::BuildHistoryId("claude-cli", session.session_id, native_agent, chat.execution_host_id.empty() ? "local" : chat.execution_host_id);

		if (JsonBooleanValueOr(item, "is_error", false)) tool_call.status = uam::acp_statuses::kFailed;
		else if (tool_call.task_id.empty()) tool_call.status = uam::acp_statuses::kCompleted;
		const nlohmann::json* content_value = uam::nlohmann_json::FindField(item, "content");
		const std::string result_text = content_value == nullptr ? std::string{} : ClaudeToolResultText(*content_value);
		if (!tool_call.task_id.empty() && !uam::acp_statuses::IsActiveStatus(tool_call.status) && !tool_call.content.empty())
		{
			if (!result_text.empty() && !tool_call.content.ends_with(result_text)) tool_call.content += "\n\n" + result_text;
		}
		else tool_call.content = result_text;
		AppendToolTurnEventIfNeeded(session, tool_id);
		changed = SyncAcpToolCallsToAssistantMessage(chat, session, true) || changed;
	}

	if (changed)
	{
		SaveChatQuietly(app, chat);
	}
}

/// <summary>Publish whole-tree cumulative tokens; per-turn totals remain unknown on resumed streams.</summary>
static void HandleClaudeResultUsage(AcpSessionState& session, const nlohmann::json& message)
{
	if (JsonDiagnosticStringValue(message, "type") != "result") return;
	const nlohmann::json::const_iterator parent = message.find("parent_tool_use_id");
	if (parent != message.end() && !parent->is_null() && (!parent->is_string() || !parent->get_ref<const std::string&>().empty())) return;
	const nlohmann::json* scope_field = uam::nlohmann_json::FindStringField(message, "session_id");
	if (scope_field == nullptr) return;
	const std::string& scope = scope_field->get_ref<const std::string&>();
	if (scope.empty() || scope.size() > 512 || scope != session.session_id) return;
	AcpTokenUsageState& usage = session.provider_usage.token_usage;
	if (usage.session_id != scope) usage = {};
	usage.session_id = scope;
	const nlohmann::json* models = uam::nlohmann_json::FindObjectField(message, "modelUsage");
	if (models == nullptr || models->empty() || models->size() > 128) return;
	AcpTokenUsageBreakdownState next;
	for (const nlohmann::json::value_type& model : *models)
	{
		if (!model.is_object()) return;
		std::int64_t counts[4]{};
		const char* fields[4] = {"inputTokens", "cacheReadInputTokens", "cacheCreationInputTokens", "outputTokens"};
		for (std::size_t index = 0; index < 4; ++index)
		{
			const nlohmann::json::const_iterator value = model.find(fields[index]);
			if (value == model.end() || !value->is_number_integer()) return;
			if (value->is_number_unsigned())
			{
				const std::uint64_t count = value->get<std::uint64_t>();
				if (count > static_cast<std::uint64_t>(uam::usage::kMaxExactTokenCount)) return;
				counts[index] = static_cast<std::int64_t>(count);
			}
			else
			{
				counts[index] = value->get<std::int64_t>();
				if (counts[index] < 0 || counts[index] > uam::usage::kMaxExactTokenCount) return;
			}
			if (counts[index] > uam::usage::kMaxExactTokenCount - next.total_tokens) return;
			next.total_tokens += counts[index];
		}
		next.input_tokens += counts[0] + counts[1] + counts[2];
		next.cached_input_tokens += counts[1];
		next.cache_write_input_tokens += counts[2];
		next.output_tokens += counts[3];
	}
	const bool error = JsonBooleanValueOr(message, "is_error", false) || uam::acp_claude_stream::IsResultErrorSubtype(JsonDiagnosticStringValue(message, "subtype"));
	if (error && next.total_tokens == 0 && usage.available && usage.total.total_tokens > 0) return;
	usage.available = true;
	usage.last_available = false;
	usage.updated_at_ms = uam::time::SystemEpochMillisecondsNow();
	usage.total = next;
	usage.last = {};
}

void HandleClaudeResult(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message, CefRefPtr<CefBrowser> browser)
{
	if (claude_preinput::Consume(app, session, chat, message)) return;
	const nlohmann::json* reported_native = uam::nlohmann_json::FindStringField(message, "session_id");
	if (!AgentRunScheduler::ManagedResumeIdentityAllowed(app, session, chat,
	        reported_native ? std::string_view(reported_native->get_ref<const std::string&>()) : std::string_view{}))
		return;
	const auto retired = ClaudeMaintenanceTerminalReceipt(session, chat);
	if (retired.is_object() && JsonDiagnosticStringValue(message, "session_id") == session.session_id && JsonDiagnosticStringValue(message, "parent_tool_use_id").empty() && JsonDiagnosticStringValue(message, "uuid") == JsonDiagnosticStringValue(retired, "resultId"))
		return;
	if (session.manual_compaction_pending && (JsonDiagnosticStringValue(message, "session_id") != session.session_id || session.session_id != chat.native_session_id || !JsonDiagnosticStringValue(message, "parent_tool_use_id").empty()))
		return;
	const bool completed_native_scope = !session.session_id.empty() && session.session_id == chat.native_session_id && JsonDiagnosticStringValue(message, "session_id") == session.session_id && JsonDiagnosticStringValue(message, "parent_tool_use_id").empty();
	const std::string result_uuid = JsonDiagnosticStringValue(message, "uuid");
	bool completion_result_fresh = false;
	if (!result_uuid.empty())
	{
		const bool fresh = session.claude_seen_parent_messages.insert("result:" + result_uuid).second;
		completion_result_fresh = fresh;
		if (session.manual_compaction_pending && !fresh)
			return;
	}
	if (session.manual_compaction_pending)
	{
		const bool cancelled = uam::AcpSessionHasPendingCancel(session);
		const bool failed = JsonBooleanValueOr(message, "is_error", false) || uam::acp_claude_stream::IsResultErrorSubtype(JsonDiagnosticStringValue(message, "subtype"));
		const auto boundary = std::ranges::find_if(session.turn_events, [](const AcpTurnEventState& event) { return event.type == "claude_manual_compaction_boundary"; });
		const bool completed = !cancelled && !failed && JsonDiagnosticStringValue(message, "subtype") == "success" && session.claude_manual_compaction_boundary_seen && boundary != session.turn_events.end();
		const auto terminal_receipt = MakeClaudeMaintenanceTerminalReceipt(session, chat, result_uuid, boundary == session.turn_events.end() ? std::string{} : boundary->request_id_json);
		std::optional<AcpTurnEventState> settled;
		if (completed)
			settled = *boundary;
		std::erase_if(session.turn_events, [](const AcpTurnEventState& event) { return event.type == "claude_manual_compaction_boundary"; });
		if (settled)
			AppendContextCompactionEvent(app, session, chat, settled->text, settled->request_id_json, settled->compaction_tokens_before, settled->compaction_tokens_after);
		else
			session.last_error = cancelled ? "Claude manual compaction was interrupted." : failed ? "Claude manual compaction failed." : "Claude completed the command without a compaction boundary; context was not confirmed compacted.";
		for (AcpRemotePendingRequestState& request : chat.remote_pending_requests)
		{
			if (request.method != "claude/compact" && (session.cancel_request_id == 0 || request.request_id != session.cancel_request_id))
				continue;
			request.response_consumed = true;
			request.payload.clear();
		}
		std::erase_if(session.pending_request_methods, [](const auto& request) { return request.second == "claude/compact"; });
		session.pending_request_methods.erase(session.cancel_request_id);
		session.manual_compaction_pending = false;
		session.claude_manual_compaction_boundary_seen = false;
		session.context_compaction_in_progress = false;
		session.processing = false;
		session.recovering_remote_turn = false;
		session.inactivity_timeout_pending = false;
		session.cancel_requested = false;
		session.cancel_requested_time_s = 0.0;
		session.cancel_request_id = 0;
		session.lifecycle_state = failed ? kAcpLifecycleError : kAcpLifecycleReady;
		if (chat.execution_host_id != "local" && !chat.execution_host_id.empty())
		{
			chat.remote_manual_compaction_pending = false;
			// Keep the exact terminal identity across a save/ACK gap. Its UUID can
			// never settle a later ordinary result or a new maintenance request.
			chat.remote_manual_compaction_boundary_json = terminal_receipt.is_object() ? terminal_receipt.dump() : std::string{};
			chat.remote_turn_reconnect_pending = false;
			if (!SaveChatQuietly(app, chat))
				return;
		}
		// This maintenance result owns no user turn, goal iteration or checkpoint.
		(void)DrainNextQueuedAcpUserPrompt(app, session, chat);
		return;
	}
	ObserveManagedInitialClaudeExecution(app, session, chat, message, false);
	const bool quota_wait = OwnClaudeQuotaWait(app, session, chat, message, uam::time::TimestampNowSec());
	if (!quota_wait && chat.claude_quota_continuation.state == "continued")
		chat.claude_quota_continuation.state = "completed";
	const std::string session_id = uam::nlohmann_json::TrimmedStringValueOr(message, "session_id", "");
	if (!session_id.empty())
	{
		session.session_id = session_id;
		if (!session.goal_internal_session)
		{
			const std::string previous_native_session_id = chat.native_session_id;
			SetChatNativeSessionIdIfChanged(chat, session_id);
			SyncResolvedNativeSessionIdForChat(app, chat, session_id, previous_native_session_id);
		}
	}

	HandleClaudeResultUsage(session, message);

	const std::string model_id = uam::nlohmann_json::TrimmedStringValueOr(message, "model", "");
	if (!model_id.empty())
	{
		session.current_model_id = model_id;
	}

	if (session.available_models.empty() && !session.current_model_id.empty())
	{
		session.available_models.push_back(AcpModelState{session.current_model_id, session.current_model_id, ""});
	}

	const bool has_answer = std::ranges::any_of(session.turn_events, [](const AcpTurnEventState& event)
	{
		return event.type == "assistant_text" && !event.text.empty();
	});
	if (!has_answer && !uam::AcpSessionHasPendingCancel(session) && !JsonBooleanValueOr(message, "is_error", false) && !uam::acp_claude_stream::IsResultErrorSubtype(JsonDiagnosticStringValue(message, "subtype")))
	{
		const std::string result_text = uam::nlohmann_json::TrimmedStringValueOr(message, "result", "");
		if (!result_text.empty())
		{
			AppendAssistantChunk(chat, session, result_text);
		}
	}

	const bool is_error = JsonBooleanValueOr(message, "is_error", false);
	const std::string subtype = JsonDiagnosticStringValue(message, "subtype");
	const bool cancelled = uam::AcpSessionHasPendingCancel(session);
	if (cancelled && session.inactivity_timeout_pending)
	{
		FinalizeAcpTurnInactivityTimeout(app, session, chat);
		return;
	}
	session.pending_request_methods.erase(session.cancel_request_id);
	session.cancel_request_id = 0;
	if (!cancelled && (is_error || uam::acp_claude_stream::IsResultErrorSubtype(subtype)))
	{
		const std::string result_text = uam::strings::JoinNonEmpty(std::vector<std::string>{ContentTextFromJson(JsonArrayValue(message, "errors")), JsonDiagnosticStringValue(message, "error"), uam::nlohmann_json::TrimmedStringValueOr(message, "result", "")}, "\n");
		// Parent failure does not prove that its process-owned background tasks failed.
		for (AcpToolCallState& tool : session.tool_calls)
		{
			if (uam::acp_statuses::IsActiveStatus(tool.status) &&
			    !session.claude_live_background_task_ids.contains(tool.task_id))
			{
				tool.status = uam::acp_statuses::kFailed;
			}
		}
		(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
		FailAcpTurnOrSession(session, &chat,
		                     uam::strings::NonEmptyOrFallback(result_text, "Claude stream-json turn failed."));
	}
	else
	{
		(void)SyncAcpToolCallsToAssistantMessage(chat, session, true);
		if (completed_native_scope && completion_result_fresh && !cancelled && !is_error && !quota_wait && subtype == "success")
			RecordCompletedAcpPromptRecoveries(app, session, chat);
		CompletePromptTurnAndHandleGoalLoop(app, session, chat, kAcpLifecycleReady, browser, !cancelled);
	}

	if (browser)
	{
		uam::PushStreamDone(browser, chat.id);
	}
	if (quota_wait)
	{
		chat.claude_quota_continuation.message_count = chat.messages.size();
		session.lifecycle_state = chat.claude_quota_continuation.state == "waiting" ? "waiting_quota" : "quota_held";
		session.goal_resume_suppressed = true;
	}
	SaveChatQuietly(app, chat);
	MarkAcpChatUnseenIfBackground(app, chat);
}

void HandleClaudeMessage(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message, CefRefPtr<CefBrowser> browser)
{
	if (claude_preinput::Consume(app, session, chat, message)) return;
	const std::string type = JsonDiagnosticStringValue(message, "type");
	if (session.manual_compaction_pending && type == "result")
	{
		HandleClaudeResult(app, session, chat, message, browser);
		return;
	}
	if (session.manual_compaction_pending && (type == "assistant" || type == "user"))
		return;
	if (type == "assistant" && session.processing && session.turn_serial > 0 && session.queued_prompt.empty() &&
	    JsonDiagnosticStringValue(message, "session_id") == session.session_id && session.session_id == chat.native_session_id &&
	    JsonDiagnosticStringValue(message, "parent_tool_use_id").empty() && JsonDiagnosticStringValue(message, "error") == "rate_limit")
		session.claude_quota_usage_error = true;
	if (type == "rate_limit_event")
	{
		ObserveClaudeQuota(session, chat, message, uam::time::TimestampNowSec());
		ObserveClaudeAllowance(session, chat, message);
		return;
	}
	if (type == "system" && HandleClaudeTaskEvent(app, session, chat, message)) return;
	if (HandleClaudeChildMessage(app, session, chat, message)) return;
	if (type == "user")
	{
		const nlohmann::json notification = uam::claude::TaskNotification(ClaudeContentTextFromMessage(JsonObjectValue(message, "message")));
		if (notification.is_object() && HandleClaudeTaskEvent(app, session, chat, notification)) return;
	}
	if (type == "system" && JsonDiagnosticStringValue(message, "subtype") == "status")
	{
		if (session.manual_compaction_pending && JsonDiagnosticStringValue(message, "session_id") != session.session_id)
			return;
		session.context_compaction_in_progress = JsonDiagnosticStringValue(message, "status") == "compacting";
		return;
	}
	if (type == uam::acp_claude_stream::kMessageTypeSystem && JsonDiagnosticStringValue(message, "subtype") == "compact_boundary")
	{
		const auto retired = ClaudeMaintenanceTerminalReceipt(session, chat);
		if (retired.is_object() && JsonDiagnosticStringValue(message, "session_id") == session.session_id && JsonDiagnosticStringValue(message, "parent_tool_use_id").empty() && !JsonDiagnosticStringValue(retired, "boundaryId").empty() && JsonDiagnosticStringValue(message, "uuid") == JsonDiagnosticStringValue(retired, "boundaryId"))
			return;
		const nlohmann::json metadata = JsonObjectValue(message, "compact_metadata");
		const auto reported_count = [&metadata](const char* key) -> int64_t
		{
			const nlohmann::json* value = uam::nlohmann_json::FindField(metadata, key);
			if (!value || !value->is_number_integer()) return -1;
			if (value->is_number_unsigned())
			{
				const uint64_t count = value->get<uint64_t>();
				return count <= 9'007'199'254'740'991ULL ? static_cast<int64_t>(count) : -1;
			}
			const int64_t count = value->get<int64_t>();
			return count >= 0 && count <= 9'007'199'254'740'991LL ? count : -1;
		};
		if (session.manual_compaction_pending)
		{
			const std::string identity = JsonDiagnosticStringValue(message, "uuid");
			if (JsonDiagnosticStringValue(message, "session_id") != session.session_id || session.session_id != chat.native_session_id || !JsonDiagnosticStringValue(message, "parent_tool_use_id").empty() || !uam::uuid::IsCanonicalUuid(identity) || JsonDiagnosticStringValue(metadata, "trigger") != "manual" || reported_count("pre_tokens") < 0)
				return;
			AcpTurnEventState boundary;
			boundary.type = "claude_manual_compaction_boundary";
			boundary.text = JsonDiagnosticStringValue(message, "summary");
			boundary.request_id_json = identity;
			boundary.compaction_tokens_before = reported_count("pre_tokens");
			boundary.compaction_tokens_after = reported_count("post_tokens");
			std::erase_if(session.turn_events, [](const AcpTurnEventState& event) { return event.type == "claude_manual_compaction_boundary"; });
			session.turn_events.push_back(std::move(boundary));
			session.claude_manual_compaction_boundary_seen = true;
			session.context_compaction_in_progress = true;
			if (chat.execution_host_id != "local" && !chat.execution_host_id.empty())
			{
				chat.remote_manual_compaction_boundary_json = nlohmann::json{{"sessionId", session.session_id}, {"identity", identity}, {"summary", JsonDiagnosticStringValue(message, "summary")}, {"tokensBefore", reported_count("pre_tokens")}, {"tokensAfter", reported_count("post_tokens")}}.dump();
				(void)SaveChatQuietly(app, chat);
			}
			return;
		}
		AppendContextCompactionEvent(app, session, chat, JsonDiagnosticStringValue(message, "summary"), JsonDiagnosticStringValue(message, "uuid"), reported_count("pre_tokens"), reported_count("post_tokens"));
		return;
	}
	if (type == "control_response")
	{
		const nlohmann::json response = JsonObjectValue(message, "response");
		const int id = JsonRpcNumericId(uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(response, "request_id")));
		if (id <= 0 || !session.pending_request_methods.contains(id))
			return;
		if (session.pending_request_methods.at(id) == "claude/interrupt" && JsonDiagnosticStringValue(response, "subtype") == "success")
		{
			// The acknowledgement precedes the terminal result; keep cancellation pending until it arrives.
			session.pending_request_methods.erase(id);
			return;
		}
		const bool initialization = session.pending_request_methods.at(id) == "initialize";
		nlohmann::json normalized = {{"id", id}};
		if (JsonDiagnosticStringValue(response, "subtype") == "error")
			normalized["error"] = {{"message", JsonDiagnosticStringValue(response, "error")}};
		else
			normalized["result"] = JsonObjectValue(response, "response");
		HandleAcpResponse(app, session, chat, normalized);
		if (initialization && session.initialized)
		{
			RememberDiscoveredModels(app, session, chat);
			if (app.provider_model_catalog != nullptr && session.available_models.empty())
			{
				app.provider_model_catalog->RememberRefreshFailure(session.provider_id, "Claude reported no available models.", chat.workspace_directory, chat.execution_host_id, session.model_catalog_scope_key);
			}
			StopBackgroundModelDiscovery(app, session);
		}
		return;
	}
	if (type == "control_request")
	{
		if (ReplayPersistedInteractionResponseIfMatched(app, session, chat, {{"id", JsonDiagnosticStringValue(message, "request_id")}}))
			return;
		HandleClaudeControlRequest(app, session, chat, message);
		return;
	}
	if (type == "control_cancel_request")
	{
		const std::string id = JsonRpcIdToStableString(JsonDiagnosticStringValue(message, "request_id"));
		const auto cancel_tool = [&session](const std::string& tool_id)
		{
			for (AcpToolCallState& tool : session.tool_calls)
				if (tool.id == tool_id && uam::acp_statuses::IsActiveStatus(tool.status)) tool.status = uam::acp_statuses::kCancelled;
		};
		if (session.pending_permission.request_id_json == id) cancel_tool(session.pending_permission.tool_call_id);
		for (const AcpPendingPermissionState& queued : session.queued_permissions)
			if (queued.request_id_json == id) cancel_tool(queued.tool_call_id);
		if (session.pending_user_input.request_id_json == id) cancel_tool(session.pending_user_input.item_id);
		std::erase_if(session.queued_permissions, [&id](const AcpPendingPermissionState& pending) { return pending.request_id_json == id; });
		if (session.pending_permission.request_id_json == id)
		{
			StopPermissionReviewTasks(app, chat.id, id);
			AdvanceAcpPermissionQueue(app, session, chat);
		}
		if (session.pending_user_input.request_id_json == id)
		{
			session.pending_user_input = {};
			session.waiting_for_user_input = false;
			if (session.waiting_for_permission)
			{
				BeginAcpPendingWait(session, kAcpLifecycleWaitingPermission);
			}
			else
			{
				ClearAcpPendingWait(session);
				session.lifecycle_state = session.processing ? kAcpLifecycleProcessing : kAcpLifecycleReady;
			}
		}
		(void)SyncAcpToolCallsToAssistantMessage(chat, session, false);
		SaveChatQuietly(app, chat);
		return;
	}
	if (type == uam::acp_claude_stream::kMessageTypeSystem && JsonDiagnosticStringValue(message, "subtype") == uam::acp_claude_stream::kSubtypeInit)
	{
		const nlohmann::json* reported_native = uam::nlohmann_json::FindStringField(message, "session_id");
		if (!AgentRunScheduler::ManagedResumeIdentityAllowed(app, session, chat,
		        reported_native ? std::string_view(reported_native->get_ref<const std::string&>()) : std::string_view{}))
			return;
		if (session.manual_compaction_pending && JsonDiagnosticStringValue(message, "session_id") != session.session_id)
			return;
		ObserveManagedInitialClaudeExecution(app, session, chat, message, true);
		if (const auto* commands = uam::nlohmann_json::FindArrayField(message, "slash_commands"))
			session.claude_manual_compaction_supported = std::ranges::any_of(*commands, [](const nlohmann::json& command) { return command.is_string() && command == "compact"; });
		if (session.initialize_request_id == 0)
			session.initialized = true;
		const std::string session_id = uam::nlohmann_json::TrimmedStringValueOr(message, "session_id", "");
		if (!session_id.empty())
		{
			if (session.session_id != session_id) session.provider_usage.rate_limits = {};
			session.session_id = session_id;
			const std::string previous_native_session_id = chat.native_session_id;
			if (!session.goal_internal_session && SetChatNativeSessionIdIfChanged(chat, session_id))
			{
				SyncResolvedNativeSessionIdForChat(app, chat, session_id, previous_native_session_id);
				SaveChatQuietly(app, chat);
			}
		}

		// A native reset must not expose the previous conversation's cumulative tokens.
		const nlohmann::json* usage_scope = uam::nlohmann_json::FindStringField(message, "session_id");
		if (usage_scope != nullptr && !usage_scope->get_ref<const std::string&>().empty() &&
		    usage_scope->get_ref<const std::string&>().size() <= 512 &&
		    session.provider_usage.token_usage.session_id != usage_scope->get_ref<const std::string&>())
		{
			session.provider_usage.token_usage = {};
			session.provider_usage.token_usage.session_id = usage_scope->get_ref<const std::string&>();
		}

		session.current_model_id = uam::nlohmann_json::TrimmedStringValueOr(message, "model", session.current_model_id);
		session.current_mode_id = uam::nlohmann_json::TrimmedStringValueOr(message, "permissionMode", uam::strings::NonEmptyOrFallback(session.current_mode_id, uam::approval_modes::kDefaultApprovalMode));
		if (!session.current_model_id.empty() && session.available_models.empty())
		{
			session.available_models.push_back(AcpModelState{session.current_model_id, session.current_model_id, ""});
		}
		return;
	}

	if (type == uam::acp_claude_stream::kMessageTypeAssistant ||
	    type == uam::acp_claude_stream::kMessageTypeResult)
	{
		// Claude emits live answers, rather than ACP session/load history replay.
		session.assistant_replay_prefixes.clear();
		session.load_history_replay_updates.clear();
	}
	if (type == uam::acp_claude_stream::kMessageTypeAssistant)
	{
		if (!uam::AcpSessionHasPendingCancel(session))
			HandleClaudeAssistantMessage(app, session, chat, message, browser);
		return;
	}

	if (type == uam::acp_claude_stream::kMessageTypeUser)
	{
		if (!uam::AcpSessionHasPendingCancel(session))
			HandleClaudeUserMessage(app, session, chat, message);
		return;
	}

	if (type == uam::acp_claude_stream::kMessageTypeResult)
	{
		session.context_compaction_in_progress = false;
		HandleClaudeResult(app, session, chat, message, browser);
		return;
	}

	AppendAcpDiagnostic(session, "message", "ignored_claude_message", "", "", false, 0, "", CapDiagnosticString(message.dump(), kMaxAcpDiagnosticDetailBytes));
}

} // namespace uam::acp_detail
