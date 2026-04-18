#include "common/runtime/acp/acp_session_runtime.h"

#include "app/application_core_helpers.h"
#include "app/chat_domain_service.h"
#include "common/chat/chat_repository.h"
#include "common/platform/platform_services.h"
#include "common/utils/string_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace uam
{
namespace
{
	constexpr std::size_t kMaxRecentStderrBytes = 16 * 1024;
	constexpr const char* kAcpLifecycleStarting = "starting";
	constexpr const char* kAcpLifecycleReady = "ready";
	constexpr const char* kAcpLifecycleProcessing = "processing";
	constexpr const char* kAcpLifecycleWaitingPermission = "waitingPermission";
	constexpr const char* kAcpLifecycleStopped = "stopped";
	constexpr const char* kAcpLifecycleError = "error";
	constexpr std::size_t kMinAssistantReplayPrefixBytes = 32;

	void CompletePromptTurn(AcpSessionState& session, const char* lifecycle_state);
	void FailAcpTurnOrSession(AcpSessionState& session, const std::string& message);
	void MarkAcpChatUnseenIfBackground(AppState& app, const ChatSession& chat);

	std::string TimestampNow()
	{
		const auto now = std::chrono::system_clock::now();
		const std::time_t tt = std::chrono::system_clock::to_time_t(now);
		std::tm tm_snapshot{};
#if defined(_WIN32)
		localtime_s(&tm_snapshot, &tt);
#else
		localtime_r(&tt, &tm_snapshot);
#endif
		std::ostringstream out;
		out << std::put_time(&tm_snapshot, "%Y-%m-%dT%H:%M:%S.000Z");
		return out.str();
	}

	nlohmann::json BuildInitializeRequest(const int request_id)
	{
		return {
			{"jsonrpc", "2.0"},
			{"id", request_id},
			{"method", "initialize"},
			{"params", {
				{"protocolVersion", 1},
				{"clientCapabilities", nlohmann::json::object()},
				{"clientInfo", {
					{"name", "universal-agent-manager"},
					{"title", "Universal Agent Manager"},
					{"version", "1.0.1"},
				}},
			}},
		};
	}

	nlohmann::json BuildNewSessionRequest(const int request_id, const std::string& cwd)
	{
		return {
			{"jsonrpc", "2.0"},
			{"id", request_id},
			{"method", "session/new"},
			{"params", {
				{"cwd", cwd},
				{"mcpServers", nlohmann::json::array()},
			}},
		};
	}

	nlohmann::json BuildLoadSessionRequest(const int request_id, const std::string& session_id, const std::string& cwd)
	{
		return {
			{"jsonrpc", "2.0"},
			{"id", request_id},
			{"method", "session/load"},
			{"params", {
				{"sessionId", session_id},
				{"cwd", cwd},
				{"mcpServers", nlohmann::json::array()},
			}},
		};
	}

	nlohmann::json BuildPromptRequest(const int request_id, const std::string& session_id, const std::string& text)
	{
		return {
			{"jsonrpc", "2.0"},
			{"id", request_id},
			{"method", "session/prompt"},
			{"params", {
				{"sessionId", session_id},
				{"prompt", nlohmann::json::array({
					{
						{"type", "text"},
						{"text", text},
					},
				})},
			}},
		};
	}

	nlohmann::json BuildCancelNotification(const std::string& session_id)
	{
		return {
			{"jsonrpc", "2.0"},
			{"method", "session/cancel"},
			{"params", {
				{"sessionId", session_id},
			}},
		};
	}

	int NextAcpRequestId(AcpSessionState& session, const std::string& method)
	{
		const int id = session.next_request_id++;
		session.pending_request_methods[id] = method;
		return id;
	}

	bool WriteAcpMessage(AcpSessionState& session, const nlohmann::json& message, std::string* error_out = nullptr)
	{
		std::string line = message.dump();
		line.push_back('\n');

		if (!PlatformServicesFactory::Instance().process_service.WriteToStdioProcess(session, line.data(), line.size()))
		{
			session.last_error = "Failed to write ACP message to Gemini CLI.";
			session.lifecycle_state = kAcpLifecycleError;
			if (error_out != nullptr)
			{
				*error_out = session.last_error;
			}
			return false;
		}

		return true;
	}

	bool SendInitialize(AcpSessionState& session, std::string* error_out = nullptr)
	{
		const int id = NextAcpRequestId(session, "initialize");
		session.initialize_request_id = id;
		return WriteAcpMessage(session, BuildInitializeRequest(id), error_out);
	}

	std::string ContentTextFromJson(const nlohmann::json& content)
	{
		if (content.is_string())
		{
			return content.get<std::string>();
		}

		if (content.is_object())
		{
			const std::string type = content.value("type", "");
			if (type == "text" && content.contains("text") && content["text"].is_string())
			{
				return content["text"].get<std::string>();
			}

			if (content.contains("text") && content["text"].is_string())
			{
				return content["text"].get<std::string>();
			}

			if (content.contains("content"))
			{
				return ContentTextFromJson(content["content"]);
			}
		}

		if (content.is_array())
		{
			std::ostringstream out;
			bool first = true;
			for (const nlohmann::json& item : content)
			{
				const std::string piece = uam::strings::Trim(ContentTextFromJson(item));
				if (piece.empty())
				{
					continue;
				}
				if (!first)
				{
					out << '\n';
				}
				out << piece;
				first = false;
			}
			return out.str();
		}

		return "";
	}

	bool StartsWith(const std::string& value, const std::string& prefix)
	{
		return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
	}

		std::string StripLeadingLineBreaks(std::string value)
		{
			while (!value.empty() && (value.front() == '\n' || value.front() == '\r'))
			{
				value.erase(value.begin());
		}
			return value;
		}

		bool StartsWithLineBreak(const std::string& value)
		{
			return !value.empty() && (value.front() == '\n' || value.front() == '\r');
		}

		void AppendThoughtText(std::string& target, const std::string& chunk, const bool starts_new_block)
		{
			if (chunk.empty())
			{
				return;
			}

			if (!target.empty() && starts_new_block)
			{
				target += "\n\n";
			}
			target += chunk;
		}

		void RememberAssistantReplayPrefixes(AcpSessionState& session, const ChatSession& chat, const int turn_user_message_index)
		{
			session.assistant_replay_prefixes.clear();
			const int exclusive_end = std::min(turn_user_message_index, static_cast<int>(chat.messages.size()));
		for (int i = 0; i < exclusive_end; ++i)
		{
			const Message& message = chat.messages[static_cast<std::size_t>(i)];
			if (message.role != MessageRole::Assistant)
			{
				continue;
			}

			const std::string trimmed = uam::strings::Trim(message.content);
			if (trimmed.empty())
			{
				continue;
			}

			session.assistant_replay_prefixes.push_back(message.content);
			if (trimmed != message.content)
			{
				session.assistant_replay_prefixes.push_back(trimmed);
			}
		}

		std::sort(session.assistant_replay_prefixes.begin(), session.assistant_replay_prefixes.end(), [](const std::string& lhs, const std::string& rhs) {
			return lhs.size() > rhs.size();
		});
			session.assistant_replay_prefixes.erase(std::unique(session.assistant_replay_prefixes.begin(), session.assistant_replay_prefixes.end()), session.assistant_replay_prefixes.end());
		}

		void RememberLoadHistoryReplayUpdates(AcpSessionState& session, const ChatSession& chat, const int turn_user_message_index)
		{
			session.load_history_replay_updates.clear();
			const int exclusive_end = std::min(turn_user_message_index, static_cast<int>(chat.messages.size()));
			for (int i = 0; i < exclusive_end; ++i)
			{
				const Message& message = chat.messages[static_cast<std::size_t>(i)];
				if (message.role == MessageRole::User)
				{
					if (!uam::strings::Trim(message.content).empty())
					{
						AcpReplayUpdateState replay;
						replay.session_update = "user_message_chunk";
						replay.text = message.content;
						session.load_history_replay_updates.push_back(std::move(replay));
					}
					continue;
				}

				if (message.role != MessageRole::Assistant)
				{
					continue;
				}

				if (!uam::strings::Trim(message.thoughts).empty())
				{
					AcpReplayUpdateState replay;
					replay.session_update = "agent_thought_chunk";
					replay.text = message.thoughts;
					session.load_history_replay_updates.push_back(std::move(replay));
				}

				if (!uam::strings::Trim(message.content).empty())
				{
					AcpReplayUpdateState replay;
					replay.session_update = "agent_message_chunk";
					replay.text = message.content;
					session.load_history_replay_updates.push_back(std::move(replay));
				}

				for (const ToolCall& tool_call : message.tool_calls)
				{
					AcpReplayUpdateState replay;
					replay.session_update = "tool_call";
					replay.tool_call_id = tool_call.id;
					replay.title = tool_call.name;
					session.load_history_replay_updates.push_back(std::move(replay));
				}
			}
		}

		std::string StripKnownAssistantReplayPrefix(const AcpSessionState& session, const std::string& text)
		{
			for (const std::string& prefix : session.assistant_replay_prefixes)
			{
				if (text == prefix)
				{
					return "";
				}

				if (StartsWith(text, prefix))
				{
					const std::string suffix = text.substr(prefix.size());
					if (prefix.size() >= kMinAssistantReplayPrefixBytes || StartsWithLineBreak(suffix))
					{
						return StripLeadingLineBreaks(suffix);
					}
				}
			}

			return text;
	}

		std::string AssistantDeltaForIncomingText(const AcpSessionState& session, const std::string& current_assistant_text, const std::string& incoming_text)
		{
			std::string candidate = StripKnownAssistantReplayPrefix(session, incoming_text);
			if (candidate.empty())
		{
			return "";
		}

		if (!current_assistant_text.empty())
		{
			if (candidate == current_assistant_text)
			{
				return "";
			}

			if (StartsWith(candidate, current_assistant_text))
			{
				return candidate.substr(current_assistant_text.size());
			}
		}

			return candidate;
		}

		bool ReplayUpdateTypesCompatible(const std::string& expected, const std::string& incoming)
		{
			if (expected == incoming)
			{
				return true;
			}
			return (expected == "tool_call" && incoming == "tool_call_update") ||
			       (expected == "tool_call_update" && incoming == "tool_call");
		}

		bool ReplayToolUpdateMatches(const AcpReplayUpdateState& expected, const nlohmann::json& update, const std::string& update_type)
		{
			if (!ReplayUpdateTypesCompatible(expected.session_update, update_type))
			{
				return false;
			}

			const std::string incoming_id = update.value("toolCallId", "");
			if (!expected.tool_call_id.empty() && !incoming_id.empty())
			{
				return expected.tool_call_id == incoming_id;
			}

			const std::string incoming_title = update.value("title", "");
			return !expected.title.empty() && !incoming_title.empty() && expected.title == incoming_title;
		}

		bool ReplayTextUpdateMatches(const AcpReplayUpdateState& expected,
		                             const std::string& update_type,
		                             const std::string& incoming_text,
		                             std::string& live_suffix)
		{
			live_suffix.clear();
			if (!ReplayUpdateTypesCompatible(expected.session_update, update_type) || incoming_text.empty())
			{
				return false;
			}

			if (expected.text == incoming_text)
			{
				return true;
			}

			if (StartsWith(expected.text, incoming_text))
			{
				return true;
			}

			if (StartsWith(incoming_text, expected.text))
			{
				const std::string suffix = incoming_text.substr(expected.text.size());
				if (StartsWithLineBreak(suffix))
				{
					live_suffix = StripLeadingLineBreaks(suffix);
					return true;
				}
			}

			return false;
		}

		bool TryConsumeLoadHistoryReplayUpdate(AcpSessionState& session,
		                                       const nlohmann::json& update,
		                                       const std::string& update_type,
		                                       const std::string& incoming_text,
		                                       std::string& live_text)
		{
			live_text = incoming_text;
			if (session.load_history_replay_updates.empty() || !update.is_object())
			{
				return false;
			}

			for (std::size_t i = 0; i < session.load_history_replay_updates.size(); ++i)
			{
				AcpReplayUpdateState& expected = session.load_history_replay_updates[i];
				if (!ReplayUpdateTypesCompatible(expected.session_update, update_type))
				{
					continue;
				}

				if (update_type == "tool_call" || update_type == "tool_call_update")
				{
					if (!ReplayToolUpdateMatches(expected, update, update_type))
					{
						continue;
					}
					session.load_history_replay_updates.erase(session.load_history_replay_updates.begin(), session.load_history_replay_updates.begin() + static_cast<std::ptrdiff_t>(i) + 1);
					live_text.clear();
					return true;
				}

				std::string suffix;
				if (!ReplayTextUpdateMatches(expected, update_type, incoming_text, suffix))
				{
					continue;
				}

				if (StartsWith(expected.text, incoming_text) && expected.text != incoming_text)
				{
					expected.text = StripLeadingLineBreaks(expected.text.substr(incoming_text.size()));
					const std::ptrdiff_t erase_count = static_cast<std::ptrdiff_t>(i) + (expected.text.empty() ? 1 : 0);
					if (erase_count > 0)
					{
						session.load_history_replay_updates.erase(session.load_history_replay_updates.begin(), session.load_history_replay_updates.begin() + erase_count);
					}
					live_text.clear();
					return true;
				}

				session.load_history_replay_updates.erase(session.load_history_replay_updates.begin(), session.load_history_replay_updates.begin() + static_cast<std::ptrdiff_t>(i) + 1);
				live_text = suffix;
				return true;
			}

			return false;
		}

		std::string JsonRpcIdToStableString(const nlohmann::json& id)
		{
		if (id.is_null())
		{
			return "";
		}
		return id.dump();
	}

	nlohmann::json StableStringToJsonRpcId(const std::string& request_id_json)
	{
		try
		{
			return nlohmann::json::parse(request_id_json);
		}
		catch (...)
		{
			return request_id_json;
		}
	}

	int JsonRpcNumericId(const nlohmann::json& id)
	{
		if (id.is_number_integer())
		{
			return id.get<int>();
		}
		if (id.is_string())
		{
			try
			{
				return std::stoi(id.get<std::string>());
			}
			catch (...)
			{
				return 0;
			}
		}
		return 0;
	}

	void AppendRecentStderr(AcpSessionState& session, const std::string& chunk)
	{
		session.recent_stderr += chunk;
		if (session.recent_stderr.size() > kMaxRecentStderrBytes)
		{
			session.recent_stderr.erase(0, session.recent_stderr.size() - kMaxRecentStderrBytes);
		}
	}

	void ResetAcpRuntimeState(AcpSessionState& session)
	{
		session.initialized = false;
		session.session_ready = false;
		session.load_session_supported = false;
		session.processing = false;
		session.waiting_for_permission = false;
		session.next_request_id = 1;
		session.initialize_request_id = 0;
		session.session_setup_request_id = 0;
		session.prompt_request_id = 0;
		session.cancel_request_id = 0;
		session.current_assistant_message_index = -1;
		session.turn_user_message_index = -1;
		session.turn_assistant_message_index = -1;
		session.turn_serial = 0;
		session.queued_prompt.clear();
		session.ignore_session_updates_until_ready = false;
		session.stdout_buffer.clear();
		session.stderr_buffer.clear();
			session.recent_stderr.clear();
			session.last_error.clear();
			session.assistant_replay_prefixes.clear();
			session.load_history_replay_updates.clear();
			session.pending_assistant_thoughts.clear();
			session.agent_name.clear();
		session.agent_title.clear();
		session.agent_version.clear();
		session.pending_request_methods.clear();
		session.tool_calls.clear();
		session.plan_entries.clear();
		session.turn_events.clear();
		session.pending_permission = AcpPendingPermissionState{};
	}

	AcpSessionState& EnsureAcpSessionForChat(AppState& app, const ChatSession& chat)
	{
		if (AcpSessionState* existing = FindAcpSessionForChat(app, chat.id); existing != nullptr)
		{
			return *existing;
		}

		auto session = std::make_unique<AcpSessionState>();
		session->chat_id = chat.id;
		app.acp_sessions.push_back(std::move(session));
		return *app.acp_sessions.back();
	}

	bool StartAcpProcessForChat(AppState& app, AcpSessionState& session, const ChatSession& chat, std::string* error_out)
	{
		if (session.running)
		{
			return true;
		}

		PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(session);
		ResetAcpRuntimeState(session);
		session.chat_id = chat.id;
		session.session_id = chat.native_session_id;
		session.lifecycle_state = kAcpLifecycleStarting;

		std::string startup_error;
		const std::filesystem::path workspace_root = ResolveWorkspaceRootPath(app, chat);
		if (!PlatformServicesFactory::Instance().process_service.StartStdioProcess(session, workspace_root, {"gemini", "--acp"}, &startup_error))
		{
			session.lifecycle_state = kAcpLifecycleError;
			session.last_error = startup_error.empty() ? "Failed to start Gemini ACP process." : startup_error;
			if (error_out != nullptr)
			{
				*error_out = session.last_error;
			}
			return false;
		}

		session.running = true;
		if (!SendInitialize(session, error_out))
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(session, true);
			session.running = false;
			return false;
		}

		return true;
	}

	bool SendSessionSetupIfReady(AppState& app, AcpSessionState& session, ChatSession& chat)
	{
		if (!session.running || !session.initialized || session.session_ready || session.session_setup_request_id != 0)
		{
			return false;
		}

		const std::filesystem::path workspace_root = ResolveWorkspaceRootPath(app, chat);
		const std::string cwd = workspace_root.empty() ? std::filesystem::current_path().string() : workspace_root.string();
		const bool can_load = !chat.native_session_id.empty() && session.load_session_supported;
		const int id = NextAcpRequestId(session, can_load ? "session/load" : "session/new");
		session.session_setup_request_id = id;
		session.ignore_session_updates_until_ready = can_load;
		session.lifecycle_state = kAcpLifecycleStarting;
		session.session_id = can_load ? chat.native_session_id : "";

		if (can_load)
		{
			const bool written = WriteAcpMessage(session, BuildLoadSessionRequest(id, chat.native_session_id, cwd));
			if (!written)
			{
				session.session_setup_request_id = 0;
				FailAcpTurnOrSession(session, session.last_error.empty() ? "Failed to load Gemini ACP session." : session.last_error);
				MarkAcpChatUnseenIfBackground(app, chat);
			}
			return written;
		}

		const bool written = WriteAcpMessage(session, BuildNewSessionRequest(id, cwd));
		if (!written)
		{
			session.session_setup_request_id = 0;
			FailAcpTurnOrSession(session, session.last_error.empty() ? "Failed to create Gemini ACP session." : session.last_error);
			MarkAcpChatUnseenIfBackground(app, chat);
		}
		return written;
	}

	bool SendQueuedPromptIfReady(AcpSessionState& session)
	{
		if (!session.running || !session.session_ready || !session.processing || session.waiting_for_permission || session.prompt_request_id != 0 || session.queued_prompt.empty() || session.session_id.empty())
		{
			return false;
		}

		const int id = NextAcpRequestId(session, "session/prompt");
		const std::string prompt = session.queued_prompt;
		session.prompt_request_id = id;
		session.lifecycle_state = kAcpLifecycleProcessing;
		if (!WriteAcpMessage(session, BuildPromptRequest(id, session.session_id, prompt)))
		{
			session.prompt_request_id = 0;
			CompletePromptTurn(session, kAcpLifecycleError);
			return true;
		}

		session.queued_prompt.clear();
		return true;
	}

	void SaveChatQuietly(AppState& app, const ChatSession& chat)
	{
		(void)ChatRepository::SaveChat(app.data_root, chat);
	}

	void AppendAssistantTextTurnEvent(AcpSessionState& session, const std::string& chunk)
	{
		if (!session.turn_events.empty() && session.turn_events.back().type == "assistant_text")
		{
			session.turn_events.back().text += chunk;
			return;
		}

		AcpTurnEventState event;
		event.type = "assistant_text";
		event.text = chunk;
		session.turn_events.push_back(std::move(event));
	}

		bool AppendThoughtTurnEvent(AcpSessionState& session, const std::string& chunk)
		{
			if (chunk.empty())
			{
				return false;
			}

			if (!session.turn_events.empty() && session.turn_events.back().type == "thought")
			{
				session.turn_events.back().text += chunk;
				return false;
			}

			AcpTurnEventState event;
			event.type = "thought";
			event.text = chunk;
			session.turn_events.push_back(std::move(event));
			return true;
		}

	bool HasTurnToolEvent(const AcpSessionState& session, const std::string& tool_call_id)
	{
		return std::any_of(session.turn_events.begin(), session.turn_events.end(), [&](const AcpTurnEventState& event) {
			return event.type == "tool_call" && event.tool_call_id == tool_call_id;
		});
	}

	void AppendToolTurnEventIfNeeded(AcpSessionState& session, const std::string& tool_call_id)
	{
		if (tool_call_id.empty() || HasTurnToolEvent(session, tool_call_id))
		{
			return;
		}

		AcpTurnEventState event;
		event.type = "tool_call";
		event.tool_call_id = tool_call_id;
		session.turn_events.push_back(std::move(event));
	}

	void AppendPermissionTurnEventIfNeeded(AcpSessionState& session, const std::string& request_id_json, const std::string& tool_call_id)
	{
		if (request_id_json.empty())
		{
			return;
		}

		const bool exists = std::any_of(session.turn_events.begin(), session.turn_events.end(), [&](const AcpTurnEventState& event) {
			return event.type == "permission_request" && event.request_id_json == request_id_json;
		});
		if (exists)
		{
			return;
		}

		if (!tool_call_id.empty())
		{
			AppendToolTurnEventIfNeeded(session, tool_call_id);
		}

		AcpTurnEventState event;
		event.type = "permission_request";
		event.request_id_json = request_id_json;
		event.tool_call_id = tool_call_id;
		session.turn_events.push_back(std::move(event));
	}

		void CompletePromptTurn(AcpSessionState& session, const char* lifecycle_state)
		{
			session.prompt_request_id = 0;
			session.processing = false;
			session.waiting_for_permission = false;
			session.queued_prompt.clear();
			session.current_assistant_message_index = -1;
			session.load_history_replay_updates.clear();
			session.pending_assistant_thoughts.clear();
			session.pending_permission = AcpPendingPermissionState{};
			session.lifecycle_state = lifecycle_state;
		}

	void FailAcpTurnOrSession(AcpSessionState& session, const std::string& message)
	{
		session.last_error = message;
		if (session.processing || session.waiting_for_permission || session.prompt_request_id != 0 || !session.queued_prompt.empty())
		{
			CompletePromptTurn(session, kAcpLifecycleError);
			return;
		}

		session.lifecycle_state = kAcpLifecycleError;
	}

		void MarkAcpChatUnseenIfBackground(AppState& app, const ChatSession& chat)
		{
			const ChatSession* selected = ChatDomainService().SelectedChat(app);
			if (selected != nullptr && selected->id == chat.id)
		{
			return;
		}

			app.chats_with_unseen_updates.insert(chat.id);
		}

		bool AppendThoughtChunk(ChatSession& chat, AcpSessionState& session, const std::string& chunk)
		{
			if (chunk.empty())
			{
				return false;
			}

			const bool starts_new_block = AppendThoughtTurnEvent(session, chunk);
			if (session.current_assistant_message_index >= 0 &&
			    session.current_assistant_message_index < static_cast<int>(chat.messages.size()) &&
			    chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)].role == MessageRole::Assistant)
			{
				Message& message = chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)];
				AppendThoughtText(message.thoughts, chunk, starts_new_block);
				chat.updated_at = TimestampNow();
				return true;
			}

			AppendThoughtText(session.pending_assistant_thoughts, chunk, starts_new_block);
			return false;
		}

		void AppendAssistantChunk(ChatSession& chat, AcpSessionState& session, const std::string& chunk)
		{
			if (chunk.empty())
			{
				return;
		}

		std::string current_assistant_text;
		if (session.current_assistant_message_index >= 0 &&
		    session.current_assistant_message_index < static_cast<int>(chat.messages.size()) &&
		    chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)].role == MessageRole::Assistant)
		{
			current_assistant_text = chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)].content;
		}

		const std::string delta = AssistantDeltaForIncomingText(session, current_assistant_text, chunk);
		if (delta.empty())
		{
			return;
		}

		if (session.current_assistant_message_index < 0 ||
		    session.current_assistant_message_index >= static_cast<int>(chat.messages.size()) ||
		    chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)].role != MessageRole::Assistant)
		{
			Message message;
			message.role = MessageRole::Assistant;
			message.provider = "gemini-cli";
			message.created_at = TimestampNow();
			chat.messages.push_back(std::move(message));
			session.current_assistant_message_index = static_cast<int>(chat.messages.size()) - 1;
			session.turn_assistant_message_index = session.current_assistant_message_index;
		}

			Message& message = chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)];
			if (!session.pending_assistant_thoughts.empty())
			{
				AppendThoughtText(message.thoughts, session.pending_assistant_thoughts, !message.thoughts.empty());
				session.pending_assistant_thoughts.clear();
			}
			message.content += delta;
			chat.updated_at = TimestampNow();
			if (session.turn_assistant_message_index < 0)
			{
				session.turn_assistant_message_index = session.current_assistant_message_index;
		}
		AppendAssistantTextTurnEvent(session, delta);
	}

	AcpToolCallState& UpsertToolCall(AcpSessionState& session, const std::string& id)
	{
		for (AcpToolCallState& tool_call : session.tool_calls)
		{
			if (tool_call.id == id)
			{
				return tool_call;
			}
		}

		AcpToolCallState tool_call;
		tool_call.id = id;
		session.tool_calls.push_back(std::move(tool_call));
		return session.tool_calls.back();
	}

		void HandleSessionUpdate(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& params)
		{
			const nlohmann::json update = params.value("update", nlohmann::json::object());
			if (!update.is_object())
			{
				return;
			}

			std::string update_type = update.value("sessionUpdate", "");
			if (update_type.empty() && update.value("thought", false))
			{
				update_type = "agent_thought_chunk";
			}
			if (update_type.empty() && update.contains("toolCallId"))
			{
				update_type = "tool_call_update";
			}
			const std::string content_text = ContentTextFromJson(update.value("content", nlohmann::json::object()));
			std::string live_text;
			if (session.ignore_session_updates_until_ready)
			{
				(void)TryConsumeLoadHistoryReplayUpdate(session, update, update_type, content_text, live_text);
				return;
			}

			const bool active_turn = session.processing || session.waiting_for_permission || session.prompt_request_id != 0;
			if (!active_turn)
			{
				return;
			}

			if (update_type == "user_message_chunk")
			{
				(void)TryConsumeLoadHistoryReplayUpdate(session, update, update_type, content_text, live_text);
				return;
			}

			if (update_type == "agent_thought_chunk" || update.value("thought", false))
			{
				live_text = content_text;
				if (TryConsumeLoadHistoryReplayUpdate(session, update, update_type, content_text, live_text) && live_text.empty())
				{
					return;
				}

				if (AppendThoughtChunk(chat, session, live_text))
				{
					SaveChatQuietly(app, chat);
				}
				return;
			}

			if (update_type == "agent_message_chunk")
			{
				live_text = content_text;
				if (TryConsumeLoadHistoryReplayUpdate(session, update, update_type, content_text, live_text) && live_text.empty())
				{
					return;
				}

				AppendAssistantChunk(chat, session, live_text);
				SaveChatQuietly(app, chat);
				return;
			}

			if (update_type == "tool_call" || update.contains("toolCallId"))
			{
				if (TryConsumeLoadHistoryReplayUpdate(session, update, update_type, content_text, live_text))
				{
					return;
				}

				const std::string id = update.value("toolCallId", "");
				if (!id.empty())
			{
				AcpToolCallState& tool_call = UpsertToolCall(session, id);
				tool_call.title = update.value("title", tool_call.title);
				tool_call.kind = update.value("kind", tool_call.kind.empty() ? "other" : tool_call.kind);
				tool_call.status = update.value("status", tool_call.status.empty() ? "pending" : tool_call.status);
				if (update.contains("content"))
				{
					tool_call.content = ContentTextFromJson(update["content"]);
				}
				AppendToolTurnEventIfNeeded(session, id);
			}
			return;
		}

		if (update_type == "plan" && update.contains("entries") && update["entries"].is_array())
		{
			session.plan_entries.clear();
			for (const nlohmann::json& entry : update["entries"])
			{
				if (!entry.is_object())
				{
					continue;
				}
				AcpPlanEntryState plan_entry;
				plan_entry.content = entry.value("content", "");
				plan_entry.priority = entry.value("priority", "");
				plan_entry.status = entry.value("status", "");
				session.plan_entries.push_back(std::move(plan_entry));
			}
		}
	}

	void SendJsonRpcError(AcpSessionState& session, const nlohmann::json& id, const int code, const std::string& message)
	{
		nlohmann::json response;
		response["jsonrpc"] = "2.0";
		response["id"] = id;
		response["error"] = {
			{"code", code},
			{"message", message},
		};
		(void)WriteAcpMessage(session, response);
	}

	bool SendPermissionResponse(AcpSessionState& session,
	                            const std::string& request_id_json,
	                            const std::string& option_id,
	                            const bool cancelled,
	                            std::string* error_out = nullptr)
	{
		nlohmann::json response;
		response["jsonrpc"] = "2.0";
		response["id"] = StableStringToJsonRpcId(request_id_json);
		if (cancelled)
		{
			response["result"] = {{"outcome", {{"outcome", "cancelled"}}}};
		}
		else
		{
			response["result"] = {{"outcome", {{"outcome", "selected"}, {"optionId", option_id}}}};
		}

		return WriteAcpMessage(session, response, error_out);
	}

	void HandlePermissionRequest(AcpSessionState& session, const nlohmann::json& message)
	{
		const nlohmann::json params = message.value("params", nlohmann::json::object());
		const nlohmann::json tool_call = params.value("toolCall", nlohmann::json::object());

		AcpPendingPermissionState pending;
		pending.request_id_json = JsonRpcIdToStableString(message.value("id", nlohmann::json(nullptr)));
		pending.tool_call_id = tool_call.value("toolCallId", "");
		pending.title = tool_call.value("title", "Permission required");
		pending.kind = tool_call.value("kind", "other");
		pending.status = tool_call.value("status", "pending");
		if (tool_call.contains("content"))
		{
			pending.content = ContentTextFromJson(tool_call["content"]);
		}

		const nlohmann::json options = params.value("options", nlohmann::json::array());
		if (options.is_array())
		{
			for (const nlohmann::json& option : options)
			{
				if (!option.is_object())
				{
					continue;
				}
				AcpPermissionOptionState parsed;
				parsed.id = option.value("optionId", "");
				parsed.name = option.value("name", parsed.id);
				parsed.kind = option.value("kind", "");
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

		session.pending_permission = std::move(pending);
		session.waiting_for_permission = true;
		session.processing = true;
		session.lifecycle_state = kAcpLifecycleWaitingPermission;
	}

	void HandleAcpRequest(AppState&, AcpSessionState& session, const nlohmann::json& message)
	{
		const std::string method = message.value("method", "");
		if (method == "session/update")
		{
			return;
		}

		if (method == "session/request_permission")
		{
			HandlePermissionRequest(session, message);
			return;
		}

		if (message.contains("id"))
		{
			SendJsonRpcError(session, message["id"], -32601, "UAM ACP client does not implement method: " + method);
		}
	}

	void HandleAcpResponse(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message)
	{
		const int id = JsonRpcNumericId(message.value("id", nlohmann::json(nullptr)));
		if (id == 0)
		{
			return;
		}

		std::string method;
		if (const auto it = session.pending_request_methods.find(id); it != session.pending_request_methods.end())
		{
			method = it->second;
			session.pending_request_methods.erase(it);
		}
		if (session.prompt_request_id != 0 && id == session.prompt_request_id)
		{
			method = "session/prompt";
		}

		if (message.contains("error"))
		{
			const nlohmann::json error = message["error"];
			const std::string error_message = error.is_object() ? error.value("message", "Gemini ACP request failed.") : "Gemini ACP request failed.";
			if (method == "session/prompt" || session.processing || session.waiting_for_permission || !session.queued_prompt.empty())
			{
				FailAcpTurnOrSession(session, error_message);
				MarkAcpChatUnseenIfBackground(app, chat);
			}
			else
			{
				session.last_error = error_message;
				session.lifecycle_state = kAcpLifecycleError;
			}
			return;
		}

		const nlohmann::json result = message.value("result", nlohmann::json(nullptr));
		if (method == "initialize")
		{
			session.initialize_request_id = 0;
			session.initialized = true;
			session.lifecycle_state = kAcpLifecycleStarting;
			if (result.is_object())
			{
				const nlohmann::json agent_info = result.value("agentInfo", nlohmann::json::object());
				if (agent_info.is_object())
				{
					session.agent_name = agent_info.value("name", "");
					session.agent_title = agent_info.value("title", "");
					session.agent_version = agent_info.value("version", "");
				}
				const nlohmann::json agent_capabilities = result.value("agentCapabilities", nlohmann::json::object());
				if (agent_capabilities.is_object())
				{
					session.load_session_supported = agent_capabilities.value("loadSession", false);
				}
			}
			return;
		}

		if (method == "session/new")
		{
			session.session_setup_request_id = 0;
			if (result.is_object())
			{
				session.session_id = result.value("sessionId", session.session_id);
			}
			if (!session.session_id.empty() && chat.native_session_id != session.session_id)
			{
				chat.native_session_id = session.session_id;
			}
			session.session_ready = !session.session_id.empty();
			session.lifecycle_state = session.session_ready ? kAcpLifecycleReady : kAcpLifecycleError;
			if (!session.session_ready)
			{
				session.last_error = "Gemini ACP did not return a session id.";
			}
			SaveChatQuietly(app, chat);
			return;
		}

		if (method == "session/load")
		{
			session.session_setup_request_id = 0;
			session.session_ready = true;
			session.ignore_session_updates_until_ready = false;
			session.lifecycle_state = kAcpLifecycleReady;
			return;
		}

		if (method == "session/prompt")
		{
			CompletePromptTurn(session, kAcpLifecycleReady);
			SaveChatQuietly(app, chat);
			MarkAcpChatUnseenIfBackground(app, chat);
			return;
		}

		if (method == "session/cancel")
		{
			session.cancel_request_id = 0;
		}
	}

	bool ProcessAcpLine(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& line)
	{
		const std::string trimmed = uam::strings::Trim(line);
		if (trimmed.empty())
		{
			return false;
		}

		nlohmann::json message;
		try
		{
			message = nlohmann::json::parse(trimmed);
		}
		catch (const std::exception& ex)
		{
			FailAcpTurnOrSession(session, std::string("Invalid ACP JSON from Gemini CLI: ") + ex.what());
			MarkAcpChatUnseenIfBackground(app, chat);
			return true;
		}

		if (message.contains("method"))
		{
			const std::string method = message.value("method", "");
			if (method == "session/update")
			{
				HandleSessionUpdate(app, session, chat, message.value("params", nlohmann::json::object()));
			}
			else
			{
				HandleAcpRequest(app, session, message);
			}
			return true;
		}

		if (message.contains("id"))
		{
			HandleAcpResponse(app, session, chat, message);
			return true;
		}

		return false;
	}

	bool DrainStdout(AppState& app, AcpSessionState& session, ChatSession& chat)
	{
		bool changed = false;
		std::array<char, 8192> buffer{};
		while (true)
		{
			const std::ptrdiff_t read_bytes = PlatformServicesFactory::Instance().process_service.ReadStdioProcessStdout(session, buffer.data(), buffer.size());
			if (read_bytes > 0)
			{
				session.stdout_buffer.append(buffer.data(), static_cast<std::size_t>(read_bytes));
				std::size_t newline_pos = std::string::npos;
				while ((newline_pos = session.stdout_buffer.find('\n')) != std::string::npos)
				{
					std::string line = session.stdout_buffer.substr(0, newline_pos);
					session.stdout_buffer.erase(0, newline_pos + 1);
					changed = ProcessAcpLine(app, session, chat, line) || changed;
				}
				continue;
			}

			if (read_bytes == -2)
			{
				break;
			}

			if (read_bytes == 0)
			{
				break;
			}

			FailAcpTurnOrSession(session, "Failed to read Gemini ACP stdout.");
			MarkAcpChatUnseenIfBackground(app, chat);
			changed = true;
			break;
		}
		return changed;
	}

	bool DrainStderr(AcpSessionState& session)
	{
		bool changed = false;
		std::array<char, 4096> buffer{};
		while (true)
		{
			const std::ptrdiff_t read_bytes = PlatformServicesFactory::Instance().process_service.ReadStdioProcessStderr(session, buffer.data(), buffer.size());
			if (read_bytes > 0)
			{
				AppendRecentStderr(session, std::string(buffer.data(), static_cast<std::size_t>(read_bytes)));
				changed = true;
				continue;
			}

			if (read_bytes == -2 || read_bytes == 0)
			{
				break;
			}

			break;
		}
		return changed;
	}

	void MarkAcpProcessExited(AcpSessionState& session)
	{
		session.running = false;
		session.initialized = false;
		session.session_ready = false;
		if (session.processing || session.waiting_for_permission || session.prompt_request_id != 0 || !session.queued_prompt.empty())
		{
			const std::string message = session.last_error.empty() ? "Gemini ACP process exited during an active turn." : session.last_error;
			FailAcpTurnOrSession(session, message);
		}
		else
		{
			session.lifecycle_state = kAcpLifecycleStopped;
		}
		session.processing = false;
		session.waiting_for_permission = false;
			session.prompt_request_id = 0;
			session.cancel_request_id = 0;
			session.current_assistant_message_index = -1;
			session.pending_assistant_thoughts.clear();
			session.pending_permission = AcpPendingPermissionState{};
		PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(session);
	}
} // namespace

AcpSessionState* FindAcpSessionForChat(AppState& app, const std::string& chat_id)
{
	for (auto& session : app.acp_sessions)
	{
		if (session != nullptr && session->chat_id == chat_id)
		{
			return session.get();
		}
	}
	return nullptr;
}

const AcpSessionState* FindAcpSessionForChat(const AppState& app, const std::string& chat_id)
{
	for (const auto& session : app.acp_sessions)
	{
		if (session != nullptr && session->chat_id == chat_id)
		{
			return session.get();
		}
	}
	return nullptr;
}

bool SendAcpPrompt(AppState& app, const std::string& chat_id, const std::string& text, std::string* error_out)
{
	const std::string prompt = uam::strings::Trim(text);
	if (prompt.empty())
	{
		if (error_out != nullptr)
		{
			*error_out = "Prompt is empty.";
		}
		return false;
	}

	const int chat_index = ChatDomainService().FindChatIndexById(app, chat_id);
	if (chat_index < 0)
	{
		if (error_out != nullptr)
		{
			*error_out = "Chat not found: " + chat_id;
		}
		return false;
	}

	ChatSession& chat = app.chats[static_cast<std::size_t>(chat_index)];
	AcpSessionState& session = EnsureAcpSessionForChat(app, chat);
	if (session.processing)
	{
		if (error_out != nullptr)
		{
			*error_out = "Gemini ACP is already processing this chat.";
		}
		return false;
	}

	if (!StartAcpProcessForChat(app, session, chat, error_out))
	{
		return false;
	}

	ChatDomainService().AddMessageWithAnalytics(chat, MessageRole::User, prompt, "gemini-cli", 0, 0, 0, 0, false);
	SaveChatQuietly(app, chat);

	session.queued_prompt = prompt;
	session.processing = true;
	session.waiting_for_permission = false;
	session.current_assistant_message_index = -1;
	session.turn_user_message_index = static_cast<int>(chat.messages.size()) - 1;
		session.turn_assistant_message_index = -1;
		session.turn_serial += 1;
		RememberAssistantReplayPrefixes(session, chat, session.turn_user_message_index);
		RememberLoadHistoryReplayUpdates(session, chat, session.turn_user_message_index);
		session.pending_assistant_thoughts.clear();
		session.tool_calls.clear();
	session.plan_entries.clear();
	session.turn_events.clear();
	session.pending_permission = AcpPendingPermissionState{};
	session.last_error.clear();
	session.lifecycle_state = session.session_ready ? kAcpLifecycleProcessing : kAcpLifecycleStarting;

	if (session.session_ready)
	{
		(void)SendQueuedPromptIfReady(session);
	}

	return true;
}

bool CancelAcpTurn(AppState& app, const std::string& chat_id, std::string* error_out)
{
	AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
	if (session == nullptr || !session->running)
	{
		return true;
	}

	const std::string pending_permission_request_id = session->pending_permission.request_id_json;
	if (!pending_permission_request_id.empty())
	{
		(void)SendPermissionResponse(*session, pending_permission_request_id, "", true, error_out);
	}

	session->queued_prompt.clear();
	session->processing = false;
	session->waiting_for_permission = false;
		session->pending_permission = AcpPendingPermissionState{};
		session->current_assistant_message_index = -1;
		session->pending_assistant_thoughts.clear();
		session->lifecycle_state = session->session_ready ? kAcpLifecycleReady : kAcpLifecycleStopped;

	if (!session->session_id.empty())
	{
		if (!WriteAcpMessage(*session, BuildCancelNotification(session->session_id), error_out))
		{
			return false;
		}
	}

	return true;
}

bool StopAcpSession(AppState& app, const std::string& chat_id)
{
	AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
	if (session == nullptr)
	{
		return true;
	}

	if (session->running)
	{
		PlatformServicesFactory::Instance().process_service.StopStdioProcess(*session, true);
	}

	session->running = false;
	session->initialized = false;
	session->session_ready = false;
	session->processing = false;
	session->waiting_for_permission = false;
	session->lifecycle_state = kAcpLifecycleStopped;
	session->queued_prompt.clear();
	session->prompt_request_id = 0;
	session->cancel_request_id = 0;
	session->current_assistant_message_index = -1;
	session->turn_user_message_index = -1;
		session->turn_assistant_message_index = -1;
		session->turn_events.clear();
		session->assistant_replay_prefixes.clear();
		session->load_history_replay_updates.clear();
		session->pending_assistant_thoughts.clear();
		session->pending_permission = AcpPendingPermissionState{};
	PlatformServicesFactory::Instance().process_service.CloseStdioProcessHandles(*session);
	return true;
}

bool ResolveAcpPermission(AppState& app,
                          const std::string& chat_id,
                          const std::string& request_id_json,
                          const std::string& option_id,
                          const bool cancelled,
                          std::string* error_out)
{
	AcpSessionState* session = FindAcpSessionForChat(app, chat_id);
	if (session == nullptr || !session->running)
	{
		if (error_out != nullptr)
		{
			*error_out = "ACP session is not running.";
		}
		return false;
	}

	if (session->pending_permission.request_id_json != request_id_json)
	{
		if (error_out != nullptr)
		{
			*error_out = "ACP permission request is no longer active.";
		}
		return false;
	}

	if (!SendPermissionResponse(*session, request_id_json, option_id, cancelled, error_out))
	{
		return false;
	}

	session->pending_permission = AcpPendingPermissionState{};
	session->waiting_for_permission = false;
	session->lifecycle_state = session->processing ? kAcpLifecycleProcessing : kAcpLifecycleReady;
	return true;
}

bool PollAllAcpSessions(AppState& app)
{
	bool changed = false;
	for (auto& session_ptr : app.acp_sessions)
	{
		if (session_ptr == nullptr)
		{
			continue;
		}

		AcpSessionState& session = *session_ptr;
		if (!session.running)
		{
			continue;
		}

		const int chat_index = ChatDomainService().FindChatIndexById(app, session.chat_id);
		if (chat_index < 0)
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(session, true);
			MarkAcpProcessExited(session);
			changed = true;
			continue;
		}

		ChatSession& chat = app.chats[static_cast<std::size_t>(chat_index)];
		changed = DrainStderr(session) || changed;
		changed = DrainStdout(app, session, chat) || changed;

		if (SendSessionSetupIfReady(app, session, chat))
		{
			changed = true;
		}

		if (SendQueuedPromptIfReady(session))
		{
			if (!session.last_error.empty() && session.lifecycle_state == kAcpLifecycleError)
			{
				MarkAcpChatUnseenIfBackground(app, chat);
			}
			changed = true;
		}

		if (PlatformServicesFactory::Instance().process_service.PollStdioProcessExited(session))
		{
			MarkAcpProcessExited(session);
			if (!session.last_error.empty())
			{
				MarkAcpChatUnseenIfBackground(app, chat);
			}
			changed = true;
		}
	}

	return changed;
}

void FastStopAcpSessionsForExit(AppState& app)
{
	for (auto& session : app.acp_sessions)
	{
		if (session != nullptr)
		{
			PlatformServicesFactory::Instance().process_service.StopStdioProcess(*session, true);
			session->running = false;
			session->lifecycle_state = kAcpLifecycleStopped;
		}
	}
	app.acp_sessions.clear();
}

std::string BuildAcpInitializeRequestForTests(const int request_id)
{
	return BuildInitializeRequest(request_id).dump();
}

std::string BuildAcpNewSessionRequestForTests(const int request_id, const std::string& cwd)
{
	return BuildNewSessionRequest(request_id, cwd).dump();
}

std::string BuildAcpPromptRequestForTests(const int request_id, const std::string& session_id, const std::string& text)
{
	return BuildPromptRequest(request_id, session_id, text).dump();
}

bool ProcessAcpLineForTests(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& line)
{
	return ProcessAcpLine(app, session, chat, line);
}

} // namespace uam
