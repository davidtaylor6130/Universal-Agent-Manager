#include "cef/state_serializer.h"

#include "app/application_core_helpers.h"
#include "app/memory_service.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/app_time.h"
#include "common/provider/codex/cli/codex_session_index.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/utils/string_utils.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <sstream>
#include <vector>

namespace uam
{

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace
{

	std::string RoleStr(MessageRole role)
	{
		switch (role)
		{
		case MessageRole::User:      return "user";
	case MessageRole::Assistant: return "assistant";
	case MessageRole::System:    return "system";
	}
		return "user";
	}

	std::string ToolCallContentForFrontend(const ToolCall& tool_call)
	{
		if (!tool_call.args_json.empty() && !tool_call.result_text.empty())
		{
			return "Arguments:\n" + tool_call.args_json + "\n\nResult:\n" + tool_call.result_text;
		}
		if (!tool_call.result_text.empty())
		{
			return tool_call.result_text;
		}
		return tool_call.args_json;
	}

	std::string JsonStringValue(const nlohmann::json& object, std::initializer_list<const char*> keys)
	{
		for (const char* key : keys)
		{
			if (object.contains(key) && object[key].is_string())
			{
				const std::string value = uam::strings::Trim(object[key].get<std::string>());
				if (!value.empty())
				{
					return value;
				}
			}
		}
		return "";
	}

	std::string NormalizeAttentionKindForFrontend(const std::string& value, const std::string& fallback)
	{
		const std::string kind = uam::strings::Trim(value);
		if (kind == "question" ||
		    kind == "plan" ||
		    kind == "memory" ||
		    kind == "permission" ||
		    kind == "command" ||
		    kind == "file" ||
		    kind == "error" ||
		    kind == "generic")
		{
			return kind;
		}
		return fallback;
	}

	std::string AcpAttentionKindForFrontend(const AcpSessionState& session)
	{
		if (!session.pending_user_input.request_id_json.empty())
		{
			return NormalizeAttentionKindForFrontend(session.pending_user_input.attention_kind, "question");
		}
		if (!session.pending_permission.request_id_json.empty())
		{
			if (session.pending_permission.kind == "commandExecution")
			{
				return "command";
			}
			if (session.pending_permission.kind == "fileChange")
			{
				return "file";
			}
			return "permission";
		}
		if (session.lifecycle_state == "error" && !session.last_error.empty())
		{
			return "error";
		}
		return "";
	}

	nlohmann::json ReadCachedCodexModelsForFrontend()
	{
		auto models_json = nlohmann::json::array();
		std::ifstream in(uam::codex::CodexHomePath() / "models_cache.json", std::ios::binary);
		if (!in.good())
		{
			return models_json;
		}

		try
		{
			const nlohmann::json cache = nlohmann::json::parse(in);
			const nlohmann::json models = cache.value("models", nlohmann::json::array());
			if (!models.is_array())
			{
				return models_json;
			}

			std::vector<std::string> seen_model_ids;
			for (const nlohmann::json& model : models)
			{
				if (!model.is_object())
				{
					continue;
				}
				const std::string visibility = JsonStringValue(model, {"visibility"});
				if (!visibility.empty() && visibility != "list")
				{
					continue;
				}
				const std::string id = JsonStringValue(model, {"id", "model", "slug", "modelId"});
				if (id.empty() || std::find(seen_model_ids.begin(), seen_model_ids.end(), id) != seen_model_ids.end())
				{
					continue;
				}

				std::string name = JsonStringValue(model, {"displayName", "display_name", "name"});
				if (name.empty())
				{
					name = id;
				}
				models_json.push_back({
					{"id", id},
					{"name", name},
					{"description", JsonStringValue(model, {"description"})},
				});
				seen_model_ids.push_back(id);
			}
		}
		catch (...)
		{
			return nlohmann::json::array();
		}

		return models_json;
	}

	nlohmann::json FallbackAcpModelsForChat(const ChatSession& chat)
	{
		return uam::strings::Trim(chat.provider_id) == "codex-cli" ? ReadCachedCodexModelsForFrontend() : nlohmann::json::array();
	}

	nlohmann::json SerializeToolCallForFrontend(const ToolCall& tool_call)
	{
		nlohmann::json tool_json;
		tool_json["id"] = tool_call.id;
		tool_json["title"] = tool_call.name;
		tool_json["kind"] = tool_call.name.empty() ? "tool" : tool_call.name;
		tool_json["status"] = tool_call.status;
		tool_json["content"] = ToolCallContentForFrontend(tool_call);
		return tool_json;
	}

	nlohmann::json SerializePlanEntryForFrontend(const MessagePlanEntry& entry)
	{
		return {
			{"content", entry.content},
			{"priority", entry.priority},
			{"status", entry.status},
		};
	}

	nlohmann::json SerializePlanEntryForFrontend(const AcpPlanEntryState& entry)
	{
		return {
			{"content", entry.content},
			{"priority", entry.priority},
			{"status", entry.status},
		};
	}

	nlohmann::json SerializeMessageBlockForFrontend(const MessageBlock& block)
	{
		nlohmann::json block_json;
		block_json["type"] = block.type;
		if (!block.text.empty())
		{
			block_json["text"] = block.text;
		}
		if (!block.tool_call_id.empty())
		{
			block_json["toolCallId"] = block.tool_call_id;
		}
		if (!block.request_id_json.empty())
		{
			block_json["requestId"] = block.request_id_json;
		}
		return block_json;
	}

constexpr std::uint64_t kFingerprintHashOffset = 1469598103934665603ull;
constexpr std::uint64_t kFingerprintHashPrime = 1099511628211ull;

void FingerprintHashBytes(std::uint64_t& hash, const unsigned char* data, const std::size_t len)
{
	for (std::size_t i = 0; i < len; ++i)
	{
		hash ^= static_cast<std::uint64_t>(data[i]);
		hash *= kFingerprintHashPrime;
	}
}

void FingerprintHashString(std::uint64_t& hash, const std::string& value)
{
	FingerprintHashBytes(hash, reinterpret_cast<const unsigned char*>(value.data()), value.size());

	const unsigned char separator = 0xFF;
	FingerprintHashBytes(hash, &separator, 1);
}

void FingerprintHashBool(std::uint64_t& hash, const bool value)
{
	const unsigned char byte = value ? 1u : 0u;
	FingerprintHashBytes(hash, &byte, 1);
}

std::string FingerprintHashHex(const std::uint64_t hash)
{
	std::ostringstream out;
	out << std::hex << std::setw(16) << std::setfill('0') << hash;
	return out.str();
}

std::string MessageDigestForFingerprint(const ChatSession& session)
{
	std::uint64_t hash = kFingerprintHashOffset;

	FingerprintHashString(hash, session.updated_at);
	FingerprintHashString(hash, std::to_string(session.messages.size()));

	if (!session.messages.empty())
	{
		const Message& last_message = session.messages.back();
		FingerprintHashString(hash, RoleStr(last_message.role));
		FingerprintHashString(hash, last_message.created_at);
			FingerprintHashString(hash, last_message.provider);
			FingerprintHashString(hash, std::to_string(last_message.content.size()));
			FingerprintHashString(hash, std::to_string(last_message.tool_calls.size()));
			for (const ToolCall& tool_call : last_message.tool_calls)
			{
				FingerprintHashString(hash, tool_call.id);
				FingerprintHashString(hash, tool_call.name);
				FingerprintHashString(hash, tool_call.status);
				FingerprintHashString(hash, std::to_string(tool_call.args_json.size()));
				FingerprintHashString(hash, std::to_string(tool_call.result_text.size()));
			}
			FingerprintHashString(hash, std::to_string(last_message.thoughts.size()));
			FingerprintHashString(hash, std::to_string(last_message.plan_summary.size()));
			FingerprintHashString(hash, std::to_string(last_message.plan_entries.size()));
			for (const MessagePlanEntry& entry : last_message.plan_entries)
			{
				FingerprintHashString(hash, entry.content);
				FingerprintHashString(hash, entry.priority);
				FingerprintHashString(hash, entry.status);
			}
			FingerprintHashString(hash, std::to_string(last_message.blocks.size()));
			for (const MessageBlock& block : last_message.blocks)
			{
				FingerprintHashString(hash, block.type);
				FingerprintHashString(hash, block.text);
				FingerprintHashString(hash, block.tool_call_id);
				FingerprintHashString(hash, block.request_id_json);
			}
			FingerprintHashBool(hash, last_message.interrupted);
		}

	return FingerprintHashHex(hash);
}

const uam::CliTerminalState* FindTerminalForChat(const uam::AppState& app, const ChatSession& chat)
{
	const std::string native_session_id = chat.native_session_id;

	for (const auto& terminal : app.cli_terminals)
	{
		if (terminal == nullptr)
		{
			continue;
		}

		if (CliTerminalMatchesChat(*terminal, chat))
		{
			return terminal.get();
		}
	}

	if (!native_session_id.empty())
	{
		for (const auto& terminal : app.cli_terminals)
		{
			if (terminal == nullptr)
			{
				continue;
			}

			if (CliTerminalMatchesChatId(*terminal, native_session_id))
			{
				return terminal.get();
			}
		}
	}

	return nullptr;
}

nlohmann::json SerializeChatTerminalSummary(const AppState& app, const ChatSession& chat)
{
	const bool ready_since_last_select = app.chats_with_unseen_updates.find(chat.id) != app.chats_with_unseen_updates.end();
	const bool has_pending_call = HasPendingCallForChat(app, chat.id);

	if (const CliTerminalState* terminal = FindTerminalForChat(app, chat); terminal != nullptr)
	{
		const bool terminal_processing = CliTerminalLifecycleIsProcessing(*terminal);
		const bool processing = has_pending_call || terminal_processing;
		nlohmann::json terminal_json;
		terminal_json["terminalId"] = terminal->terminal_id;
		terminal_json["frontendChatId"] = terminal->frontend_chat_id;
		terminal_json["sourceChatId"] = CliTerminalPrimaryChatId(*terminal);
		terminal_json["running"] = terminal->running;
		terminal_json["lifecycleState"] = CliTerminalLifecycleStateLabel(*terminal);
		terminal_json["turnState"] = terminal_processing ? "busy" : "idle";
		terminal_json["processing"] = processing;
		terminal_json["readySinceLastSelect"] = ready_since_last_select;
		terminal_json["active"] = CliTerminalLifecycleIsIdleLive(*terminal);
		terminal_json["lastError"] = terminal->last_error;
		return terminal_json;
	}

	const bool processing = has_pending_call;
	nlohmann::json terminal_json;
	terminal_json["running"] = false;
	terminal_json["lifecycleState"] = "stopped";
	terminal_json["turnState"] = "idle";
	terminal_json["processing"] = processing;
	terminal_json["readySinceLastSelect"] = ready_since_last_select;
	terminal_json["active"] = false;
	terminal_json["lastError"] = "";
	return terminal_json;
}

nlohmann::json SerializeAcpSessionSummary(const AppState& app, const ChatSession& chat)
{
	nlohmann::json acp_json;
	const AcpSessionState* session = FindAcpSessionForChat(app, chat.id);
	const bool ready_since_last_select = app.chats_with_unseen_updates.find(chat.id) != app.chats_with_unseen_updates.end();
	if (session == nullptr)
	{
		acp_json["sessionId"] = chat.native_session_id;
		acp_json["providerId"] = chat.provider_id;
		acp_json["protocolKind"] = "";
		acp_json["threadId"] = chat.native_session_id;
		acp_json["running"] = false;
		acp_json["processing"] = false;
		acp_json["readySinceLastSelect"] = ready_since_last_select;
			acp_json["attentionKind"] = nullptr;
			acp_json["lifecycleState"] = "stopped";
			acp_json["lastError"] = "";
			acp_json["recentStderr"] = "";
			acp_json["lastExitCode"] = nullptr;
			acp_json["diagnostics"] = nlohmann::json::array();
			acp_json["toolCalls"] = nlohmann::json::array();
			acp_json["planSummary"] = "";
			acp_json["planEntries"] = nlohmann::json::array();
			acp_json["availableModes"] = nlohmann::json::array();
			acp_json["currentModeId"] = chat.approval_mode;
			acp_json["availableModels"] = FallbackAcpModelsForChat(chat);
			acp_json["currentModelId"] = chat.model_id;
			acp_json["turnEvents"] = nlohmann::json::array();
			acp_json["turnUserMessageIndex"] = -1;
		acp_json["turnAssistantMessageIndex"] = -1;
			acp_json["turnSerial"] = 0;
			acp_json["waitIsStale"] = false;
			acp_json["waitStaleReason"] = "";
			acp_json["waitSeconds"] = 0;
		acp_json["pendingPermission"] = nullptr;
		acp_json["pendingUserInput"] = nullptr;
		return acp_json;
	}

	acp_json["sessionId"] = session->session_id;
	acp_json["providerId"] = session->provider_id;
	acp_json["protocolKind"] = session->protocol_kind;
	acp_json["threadId"] = session->codex_thread_id.empty() ? session->session_id : session->codex_thread_id;
	acp_json["running"] = session->running;
	acp_json["processing"] = session->processing;
	acp_json["readySinceLastSelect"] = ready_since_last_select;
	const std::string attention_kind = AcpAttentionKindForFrontend(*session);
	acp_json["attentionKind"] = attention_kind.empty() ? nlohmann::json(nullptr) : nlohmann::json(attention_kind);
		acp_json["lifecycleState"] = session->lifecycle_state;
		acp_json["lastError"] = session->last_error;
		acp_json["recentStderr"] = session->recent_stderr;
		acp_json["lastExitCode"] = session->has_last_exit_code ? nlohmann::json(session->last_exit_code) : nlohmann::json(nullptr);
		acp_json["agentInfo"] = {
			{"name", session->agent_name},
			{"title", session->agent_title},
			{"version", session->agent_version},
		};

		auto diagnostics = nlohmann::json::array();
		for (const AcpDiagnosticEntryState& diagnostic : session->diagnostics)
		{
			nlohmann::json diagnostic_json;
			diagnostic_json["time"] = diagnostic.time;
			diagnostic_json["event"] = diagnostic.event;
			diagnostic_json["reason"] = diagnostic.reason;
			diagnostic_json["method"] = diagnostic.method;
			diagnostic_json["requestId"] = diagnostic.request_id;
			diagnostic_json["code"] = diagnostic.has_code ? nlohmann::json(diagnostic.code) : nlohmann::json(nullptr);
			diagnostic_json["message"] = diagnostic.message;
			diagnostic_json["detail"] = diagnostic.detail;
			diagnostic_json["lifecycleState"] = diagnostic.lifecycle_state;
			diagnostics.push_back(std::move(diagnostic_json));
		}
		acp_json["diagnostics"] = std::move(diagnostics);

		auto tool_calls = nlohmann::json::array();
	for (const AcpToolCallState& tool_call : session->tool_calls)
	{
		tool_calls.push_back({
			{"id", tool_call.id},
			{"title", tool_call.title},
			{"kind", tool_call.kind},
			{"status", tool_call.status},
			{"content", tool_call.content},
		});
	}
	acp_json["toolCalls"] = std::move(tool_calls);

		auto plan_entries = nlohmann::json::array();
		for (const AcpPlanEntryState& entry : session->plan_entries)
		{
			plan_entries.push_back(SerializePlanEntryForFrontend(entry));
		}
		acp_json["planSummary"] = session->plan_summary;
		acp_json["planEntries"] = std::move(plan_entries);

		auto available_modes = nlohmann::json::array();
		for (const AcpModeState& mode : session->available_modes)
		{
			available_modes.push_back({
				{"id", mode.id},
				{"name", mode.name},
				{"description", mode.description},
			});
		}
		acp_json["availableModes"] = std::move(available_modes);
		acp_json["currentModeId"] = session->current_mode_id.empty() ? chat.approval_mode : session->current_mode_id;

		auto available_models = nlohmann::json::array();
		for (const AcpModelState& model : session->available_models)
		{
			available_models.push_back({
				{"id", model.id},
				{"name", model.name},
				{"description", model.description},
			});
		}
		acp_json["availableModels"] = available_models.empty() ? FallbackAcpModelsForChat(chat) : std::move(available_models);
		acp_json["currentModelId"] = session->current_model_id.empty() ? chat.model_id : session->current_model_id;

		auto turn_events = nlohmann::json::array();
	for (const AcpTurnEventState& event : session->turn_events)
	{
		nlohmann::json event_json;
		event_json["type"] = event.type;
		if (!event.text.empty())
		{
			event_json["text"] = event.text;
		}
		if (!event.tool_call_id.empty())
		{
			event_json["toolCallId"] = event.tool_call_id;
		}
		if (!event.request_id_json.empty())
		{
			event_json["requestId"] = event.request_id_json;
		}
		turn_events.push_back(std::move(event_json));
	}
	acp_json["turnEvents"] = std::move(turn_events);
	acp_json["turnUserMessageIndex"] = session->turn_user_message_index;
	acp_json["turnAssistantMessageIndex"] = session->turn_assistant_message_index;
	acp_json["turnSerial"] = session->turn_serial;
	acp_json["waitIsStale"] = session->wait_is_stale;
	acp_json["waitStaleReason"] = session->wait_stale_reason;
	acp_json["waitSeconds"] = session->wait_started_time_s > 0.0
		? static_cast<int>(std::max(0.0, GetAppTimeSeconds() - session->wait_started_time_s))
		: 0;

	if (!session->pending_permission.request_id_json.empty())
	{
		nlohmann::json permission_json;
		permission_json["requestId"] = session->pending_permission.request_id_json;
		permission_json["toolCallId"] = session->pending_permission.tool_call_id;
		permission_json["title"] = session->pending_permission.title;
		permission_json["kind"] = session->pending_permission.kind;
		permission_json["status"] = session->pending_permission.status;
		permission_json["content"] = session->pending_permission.content;

		auto options = nlohmann::json::array();
		for (const AcpPermissionOptionState& option : session->pending_permission.options)
		{
			options.push_back({
				{"id", option.id},
				{"name", option.name},
				{"kind", option.kind},
			});
		}
		permission_json["options"] = std::move(options);
		acp_json["pendingPermission"] = std::move(permission_json);
	}
	else
	{
		acp_json["pendingPermission"] = nullptr;
	}

	if (!session->pending_user_input.request_id_json.empty())
	{
		nlohmann::json input_json;
		input_json["requestId"] = session->pending_user_input.request_id_json;
		input_json["itemId"] = session->pending_user_input.item_id;
		input_json["status"] = session->pending_user_input.status;
		input_json["attentionKind"] = NormalizeAttentionKindForFrontend(session->pending_user_input.attention_kind, "question");

		auto questions = nlohmann::json::array();
		for (const AcpUserInputQuestionState& question : session->pending_user_input.questions)
		{
			nlohmann::json question_json;
			question_json["id"] = question.id;
			question_json["header"] = question.header;
			question_json["question"] = question.question;
			question_json["isOther"] = question.is_other;
			question_json["isSecret"] = question.is_secret;

			auto options = nlohmann::json::array();
			for (const AcpUserInputOptionState& option : question.options)
			{
				options.push_back({
					{"label", option.label},
					{"description", option.description},
				});
			}
			question_json["options"] = std::move(options);
			questions.push_back(std::move(question_json));
		}
		input_json["questions"] = std::move(questions);
		acp_json["pendingUserInput"] = std::move(input_json);
	}
	else
	{
		acp_json["pendingUserInput"] = nullptr;
	}

	return acp_json;
}

nlohmann::json SerializeFingerprintSession(const AppState& app, const ChatSession& chat)
{
	nlohmann::json chat_json;
	chat_json["id"] = chat.id;
	chat_json["title"] = chat.title;
	chat_json["folderId"] = chat.folder_id;
	chat_json["pinned"] = chat.pinned;
	chat_json["providerId"] = chat.provider_id;
	chat_json["modelId"] = chat.model_id;
	chat_json["approvalMode"] = chat.approval_mode;
	chat_json["memoryEnabled"] = chat.memory_enabled;
	chat_json["memoryLastProcessedMessageCount"] = chat.memory_last_processed_message_count;
	chat_json["memoryLastProcessedAt"] = chat.memory_last_processed_at;
	chat_json["workspaceDirectory"] = ResolveWorkspaceRootPath(app, chat).string();
	chat_json["createdAt"] = chat.created_at;
	chat_json["updatedAt"] = chat.updated_at;
	chat_json["lastOpenedAt"] = chat.last_opened_at.empty() ? chat.updated_at : chat.last_opened_at;
	chat_json["messageCount"] = chat.messages.size();
	chat_json["messagesDigest"] = MessageDigestForFingerprint(chat);
	chat_json["cliTerminal"] = SerializeChatTerminalSummary(app, chat);
	chat_json["acpSession"] = SerializeAcpSessionSummary(app, chat);
	return chat_json;
}

nlohmann::json SerializeCliDebugState(const AppState& app)
{
	nlohmann::json cli_debug;
	cli_debug["selectedChatId"] = CliSelectedChatId(app);
	cli_debug["terminalCount"] = app.cli_terminals.size();

	std::size_t running_count = 0;
	std::size_t busy_count = 0;
	auto terminals = nlohmann::json::array();

	for (const auto& terminal_ptr : app.cli_terminals)
	{
		if (terminal_ptr == nullptr)
		{
			continue;
		}

		const CliTerminalState& terminal = *terminal_ptr;
		if (terminal.running)
		{
			++running_count;
		}

		if (CliTerminalLifecycleIsProcessing(terminal))
		{
			++busy_count;
		}

		nlohmann::json terminal_json;
		terminal_json["terminalId"] = terminal.terminal_id;
		terminal_json["frontendChatId"] = terminal.frontend_chat_id;
		terminal_json["sourceChatId"] = CliTerminalPrimaryChatId(terminal);
		terminal_json["attachedSessionId"] = terminal.attached_session_id;
		terminal_json["providerId"] = CliProviderIdForDiagnostics(app, terminal);
		terminal_json["nativeSessionId"] = CliNativeSessionIdForDiagnostics(app, terminal);
		terminal_json["processId"] = CliProcessHandleLabel(terminal);
		terminal_json["running"] = terminal.running;
		terminal_json["uiAttached"] = terminal.ui_attached;
		terminal_json["lifecycleState"] = CliTerminalLifecycleStateLabel(terminal);
		terminal_json["turnState"] = CliTurnStateLabel(terminal);
		terminal_json["inputReady"] = terminal.input_ready;
		terminal_json["generationInProgress"] = terminal.generation_in_progress;
		terminal_json["lastUserInputAt"] = terminal.last_user_input_time_s;
		terminal_json["lastAiOutputAt"] = terminal.last_ai_output_time_s;
		terminal_json["lastPolledAt"] = terminal.last_polled_time_s;
		terminal_json["lastError"] = terminal.last_error;
		terminals.push_back(std::move(terminal_json));
	}

	cli_debug["runningTerminalCount"] = running_count;
	cli_debug["busyTerminalCount"] = busy_count;
	cli_debug["terminals"] = std::move(terminals);
	return cli_debug;
}

nlohmann::json SerializeMemoryActivity(const AppState& app)
{
	const uam::MemoryActivityState activity = MemoryService::BuildMemoryActivity(app);
	return {
		{"entryCount", activity.entry_count},
		{"lastCreatedAt", activity.last_created_at},
		{"lastCreatedCount", activity.last_created_count},
		{"runningCount", activity.running_count},
		{"lastStatus", activity.last_status},
		{"lastWorkerChatId", activity.last_worker_chat_id},
		{"lastWorkerProviderId", activity.last_worker_provider_id},
		{"lastWorkerUpdatedAt", activity.last_worker_updated_at},
		{"lastWorkerStatus", activity.last_worker_status},
		{"lastWorkerOutput", activity.last_worker_output},
		{"lastWorkerError", activity.last_worker_error},
		{"lastWorkerTimedOut", activity.last_worker_timed_out},
		{"lastWorkerCanceled", activity.last_worker_canceled},
		{"lastWorkerHasExitCode", activity.last_worker_has_exit_code},
		{"lastWorkerExitCode", activity.last_worker_exit_code},
	};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// StateSerializer implementation
// ---------------------------------------------------------------------------

nlohmann::json StateSerializer::Serialize(const AppState& app)
{
	nlohmann::json j;
	j["stateRevision"] = app.state_revision;

	// Folders
	auto folders_arr = nlohmann::json::array();
	for (const auto& folder : app.folders)
		folders_arr.push_back(SerializeFolder(folder));
	j["folders"] = folders_arr;

	// Chat sessions
	auto chats_arr = nlohmann::json::array();
	for (const auto& chat : app.chats)
	{
		nlohmann::json chat_json = SerializeSession(chat);
		chat_json["workspaceDirectory"] = ResolveWorkspaceRootPath(app, chat).string();

		const bool ready_since_last_select = app.chats_with_unseen_updates.find(chat.id) != app.chats_with_unseen_updates.end();
		const bool has_pending_call = HasPendingCallForChat(app, chat.id);

		if (const CliTerminalState* terminal = FindTerminalForChat(app, chat); terminal != nullptr)
		{
			const bool terminal_processing = CliTerminalLifecycleIsProcessing(*terminal);
			const bool processing = has_pending_call || terminal_processing;
			nlohmann::json terminal_json;
			terminal_json["terminalId"] = terminal->terminal_id;
			terminal_json["frontendChatId"] = terminal->frontend_chat_id;
			terminal_json["sourceChatId"] = CliTerminalPrimaryChatId(*terminal);
			terminal_json["running"] = terminal->running;
			terminal_json["lifecycleState"] = CliTerminalLifecycleStateLabel(*terminal);
			terminal_json["turnState"] = terminal_processing ? "busy" : "idle";
			terminal_json["processing"] = processing;
			terminal_json["readySinceLastSelect"] = ready_since_last_select;
			terminal_json["active"] = CliTerminalLifecycleIsIdleLive(*terminal);
			terminal_json["lastError"] = terminal->last_error;
			chat_json["cliTerminal"] = terminal_json;
		}
		else
		{
			const bool processing = has_pending_call;
			nlohmann::json terminal_json;
			terminal_json["running"] = false;
			terminal_json["lifecycleState"] = "stopped";
			terminal_json["turnState"] = "idle";
			terminal_json["processing"] = processing;
			terminal_json["readySinceLastSelect"] = ready_since_last_select;
			terminal_json["active"] = false;
			terminal_json["lastError"] = "";
			chat_json["cliTerminal"] = terminal_json;
		}

		chat_json["acpSession"] = SerializeAcpSessionSummary(app, chat);
		chats_arr.push_back(std::move(chat_json));
	}
	j["chats"] = chats_arr;
	j["cliDebug"] = SerializeCliDebugState(app);
	j["memoryActivity"] = SerializeMemoryActivity(app);

	// Selected chat id (resolved from index)
	if (app.selected_chat_index >= 0 &&
	    app.selected_chat_index < static_cast<int>(app.chats.size()))
	{
		j["selectedChatId"] = app.chats[static_cast<std::size_t>(app.selected_chat_index)].id;
	}
	else
	{
		j["selectedChatId"] = nullptr;
	}

	// Provider profiles
	auto providers_arr = nlohmann::json::array();
	for (const auto& profile : app.provider_profiles)
	{
		if (ProviderRuntime::IsRuntimeEnabled(profile))
		{
			providers_arr.push_back(SerializeProvider(profile));
		}
	}
	j["providers"] = providers_arr;

	// Settings slice that the UI cares about
	{
		nlohmann::json settings;
		settings["activeProviderId"] = app.settings.active_provider_id;
		settings["theme"]            = app.settings.ui_theme;
		settings["memoryEnabledDefault"] = app.settings.memory_enabled_default;
		settings["memoryIdleDelaySeconds"] = app.settings.memory_idle_delay_seconds;
		settings["memoryRecallBudgetBytes"] = app.settings.memory_recall_budget_bytes;
		settings["memoryLastStatus"] = app.memory_last_status;
		nlohmann::json bindings = nlohmann::json::object();
		for (const auto& entry : app.settings.memory_worker_bindings)
		{
			bindings[entry.first] = {
				{"workerProviderId", entry.second.worker_provider_id},
				{"workerModelId", entry.second.worker_model_id},
			};
		}
		settings["memoryWorkerBindings"] = std::move(bindings);
		j["settings"]                = settings;
	}

	return j;
}

nlohmann::json StateSerializer::SerializeFingerprint(const AppState& app)
{
	nlohmann::json j;

	auto folders_arr = nlohmann::json::array();
	for (const auto& folder : app.folders)
	{
		folders_arr.push_back(SerializeFolder(folder));
	}
	j["folders"] = folders_arr;

	auto chats_arr = nlohmann::json::array();
	for (const auto& chat : app.chats)
	{
		chats_arr.push_back(SerializeFingerprintSession(app, chat));
	}
	j["chats"] = chats_arr;

	if (app.selected_chat_index >= 0 &&
	    app.selected_chat_index < static_cast<int>(app.chats.size()))
	{
		j["selectedChatId"] = app.chats[static_cast<std::size_t>(app.selected_chat_index)].id;
	}
	else
	{
		j["selectedChatId"] = nullptr;
	}

	auto providers_arr = nlohmann::json::array();
	for (const auto& profile : app.provider_profiles)
	{
		if (ProviderRuntime::IsRuntimeEnabled(profile))
		{
			providers_arr.push_back(SerializeProvider(profile));
		}
	}
	j["providers"] = providers_arr;
	j["memoryActivity"] = SerializeMemoryActivity(app);

	{
		nlohmann::json settings;
		settings["activeProviderId"] = app.settings.active_provider_id;
		settings["theme"] = app.settings.ui_theme;
		settings["memoryEnabledDefault"] = app.settings.memory_enabled_default;
		settings["memoryIdleDelaySeconds"] = app.settings.memory_idle_delay_seconds;
		settings["memoryRecallBudgetBytes"] = app.settings.memory_recall_budget_bytes;
		j["settings"] = settings;
	}

	return j;
}

nlohmann::json StateSerializer::SerializeSession(const ChatSession& session)
{
	nlohmann::json j;
	j["id"]         = session.id;
	j["title"]      = session.title;
	j["folderId"]   = session.folder_id;
	j["pinned"]     = session.pinned;
	j["providerId"] = session.provider_id;
	j["modelId"]    = session.model_id;
	j["approvalMode"] = session.approval_mode;
	j["memoryEnabled"] = session.memory_enabled;
	j["memoryLastProcessedMessageCount"] = session.memory_last_processed_message_count;
	j["memoryLastProcessedAt"] = session.memory_last_processed_at;
	j["workspaceDirectory"] = session.workspace_directory;
	j["createdAt"]  = session.created_at;
	j["updatedAt"]  = session.updated_at;
	j["lastOpenedAt"] = session.last_opened_at.empty() ? session.updated_at : session.last_opened_at;
	j["messageCount"] = session.messages.size();
	j["messagesDigest"] = MessageDigestForFingerprint(session);

	auto msgs = nlohmann::json::array();
		for (const auto& msg : session.messages)
		{
			nlohmann::json m;
			m["role"]      = RoleStr(msg.role);
			m["content"]   = msg.content;
			m["createdAt"] = msg.created_at;
			if (!msg.thoughts.empty())
			{
				m["thoughts"] = msg.thoughts;
			}
			if (!msg.plan_summary.empty())
			{
				m["planSummary"] = msg.plan_summary;
			}
			if (!msg.plan_entries.empty())
			{
				auto plan_entries = nlohmann::json::array();
				for (const MessagePlanEntry& entry : msg.plan_entries)
				{
					plan_entries.push_back(SerializePlanEntryForFrontend(entry));
				}
				m["planEntries"] = std::move(plan_entries);
			}
			if (!msg.tool_calls.empty())
			{
				auto tool_calls = nlohmann::json::array();
				for (const ToolCall& tool_call : msg.tool_calls)
				{
					tool_calls.push_back(SerializeToolCallForFrontend(tool_call));
				}
				m["toolCalls"] = std::move(tool_calls);
			}
			if (!msg.blocks.empty())
			{
				auto blocks = nlohmann::json::array();
				for (const MessageBlock& block : msg.blocks)
				{
					if (!block.type.empty())
					{
						blocks.push_back(SerializeMessageBlockForFrontend(block));
					}
				}
				if (!blocks.empty())
				{
					m["blocks"] = std::move(blocks);
				}
			}
			msgs.push_back(m);
		}
	j["messages"] = msgs;

	return j;
}

nlohmann::json StateSerializer::SerializeFolder(const ChatFolder& folder)
{
	nlohmann::json j;
	j["id"]        = folder.id;
	j["title"]     = folder.title;
	j["directory"] = folder.directory;
	j["collapsed"] = folder.collapsed;
	return j;
}

nlohmann::json StateSerializer::SerializeProvider(const ProviderProfile& profile)
{
	nlohmann::json j;
	j["id"]        = profile.id;
	j["name"]      = profile.title;
	j["shortName"] = profile.title;  // React derives a short name from this
	j["outputMode"] = profile.output_mode;
	j["supportsCli"] = profile.supports_cli;
	j["supportsStructured"] = profile.supports_structured;
	j["structuredProtocol"] = profile.structured_protocol;
	return j;
}

} // namespace uam
