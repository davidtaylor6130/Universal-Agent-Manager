#include "common/provider/antigravity/cli/antigravity_cli_provider_runtime.h"

#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"

#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/utils/uuid.h"
#include "cef/cef_push.h"
#include <algorithm>
#include <limits>

const char* AntigravityCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kAntigravityCli;
}

std::vector<std::string> AntigravityCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive) return {};
	std::vector<std::string> argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "agy");
	uam::provider_runtime_internal::AppendResumeArgs(argv, profile, chat.native_session_id);
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", chat.model_id);
	const AppSettings merged = uam::provider_runtime_internal::MergeProviderSettings(profile, settings);
	// Permission policy remains with the CLI. Keep explicit extra flags in the existing launch contract.
	uam::provider_runtime_internal::AppendArgs(argv, uam::command_line::SplitWords(merged.provider_extra_flags));
	return argv;
}

MessageRole AntigravityCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, std::string_view type) const
{
	return uam::provider_runtime_internal::RoleFromNativeType(profile, type);
}

std::vector<ChatSession> AntigravityCliProviderRuntime::LoadHistory(const ProviderProfile&, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	return uam::provider_runtime_internal::LoadLocalChats(data_root);
}

bool AntigravityCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return uam::provider_runtime_internal::SaveLocalChat(data_root, chat);
}

const IProviderRuntime& GetAntigravityCliProviderRuntime()
{
	static const AntigravityCliProviderRuntime runtime;
	return runtime;
}


namespace
{
	/// <summary>Native counters must be nonnegative signed integers; avoid narrowing provider-controlled values.</summary>
	int64_t AntigravityCounter(const nlohmann::json& object, const char* key)
	{
		const nlohmann::json::const_iterator value = object.find(key);
		if (value == object.end() || !value->is_number_integer()) return 0;
		if (value->is_number_unsigned())
		{
			const uint64_t number = value->get<uint64_t>();
			return number <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ? static_cast<int64_t>(number) : 0;
		}
		return std::max<int64_t>(0, value->get<int64_t>());
	}
}

std::vector<std::string> AntigravityCliProviderRuntime::BuildStructuredLaunchArgv(const ProviderProfile& profile, const ChatSession& chat) const
{
	std::vector<std::string> argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "agy");
	argv.insert(argv.end(), {"--input-format", "stream-json", "--output-format", "stream-json"});
	uam::provider_runtime_internal::AppendResumeArgs(argv, profile, chat.native_session_id);
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", chat.model_id);
	if (chat.approval_mode == "plan") argv.insert(argv.end(), {"--mode", "plan"});
	return argv;
}

nlohmann::json AntigravityCliProviderRuntime::OnAcpBuildInitialize(uam::AcpSessionState& session, int) const
{
	// This native protocol has no control RPC. The first prompt causes its init event.
	session.initialized = true;
	session.antigravity_input_tokens = 0;
	session.antigravity_output_tokens = 0;
	session.antigravity_completed_turns = 0;
	session.antigravity_completed_steps.clear();
	session.agent_name = "antigravity";
	session.agent_title = "Antigravity CLI";
	return nullptr;
}

nlohmann::json AntigravityCliProviderRuntime::OnAcpBuildSetupRequest(int, const ChatSession&, const std::string&, bool, std::string& method) const
{
	method.clear();
	return nullptr;
}

nlohmann::json AntigravityCliProviderRuntime::OnAcpBuildPrompt(uam::AcpSessionState& session, int, const std::string& prompt, const ChatSession&, std::string& method) const
{
	method.clear();
	session.antigravity_input_tokens = 0;
	session.antigravity_output_tokens = 0;
	session.antigravity_completed_steps.clear();
	return {{"event", "user"}, {"message", {{"content", prompt}}}};
}

nlohmann::json AntigravityCliProviderRuntime::OnAcpBuildCancel(const uam::AcpSessionState&, int, std::string& method) const
{
	// Shared cancellation stops this owned process; sending control envelopes aborts the native protocol.
	method.clear();
	return nullptr;
}

bool AntigravityCliProviderRuntime::OnAcpHandleMessage(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
	const nlohmann::json& message, const CefRefPtr<CefBrowser>& browser) const
{
	using namespace uam::acp_detail;
	try
	{
		const std::string event = JsonDiagnosticStringValue(message, "event");
		const nlohmann::json payload = event == "result" ? JsonObjectValue(message, "result") :
			event == "step_update" ? JsonObjectValue(message, "step_update") : message;
		if (event != "init" && event != "step_update" && event != "result") return true;
		const std::string identity = JsonDiagnosticStringValue(payload, "conversation_id");
		if (!uam::uuid::IsCanonicalUuid(identity) || (!session.session_id.empty() && session.session_id != identity))
		{
			FailAcpTurnOrSession(session, &chat, "Antigravity returned an invalid or changed conversation identity.");
			SaveChatQuietly(app, chat);
			return true;
		}
		if (session.session_id.empty())
		{
			session.session_id = identity;
			if (!session.goal_internal_session)
			{
				const std::string previous = chat.native_session_id;
				SetChatNativeSessionIdIfChanged(chat, identity);
				SyncResolvedNativeSessionIdForChat(app, chat, identity, previous);
				SaveChatQuietly(app, chat);
			}
		}
		MarkAcpRuntimeActivity(session);
		if (event == "init") return true;
		if (!session.processing || uam::AcpSessionHasPendingCancel(session)) return true;
		// Native events contain current-turn text, never ACP session/load transcript replay.
		session.assistant_replay_prefixes.clear();
		session.load_history_replay_updates.clear();
		if (event == "step_update")
		{
			const std::string state = JsonDiagnosticStringValue(payload, "state");
			if (state != "ACTIVE" && state != "DONE") return true;
			if (state == "DONE")
			{
				const int64_t index = AntigravityCounter(payload, "step_index");
				if (!session.antigravity_completed_steps.insert(index).second) return true;
				const nlohmann::json usage = JsonObjectValue(payload, "usage");
				const int64_t input = AntigravityCounter(usage, "input_tokens");
				const int64_t output = AntigravityCounter(usage, "output_tokens");
				session.antigravity_input_tokens += std::min(input, std::numeric_limits<int64_t>::max() - session.antigravity_input_tokens);
				session.antigravity_output_tokens += std::min(output, std::numeric_limits<int64_t>::max() - session.antigravity_output_tokens);
			}
			if (JsonDiagnosticStringValue(payload, "step_type") == "agent_response")
			{
				const std::string text = JsonDiagnosticStringValue(payload, "text_delta");
				if (!text.empty())
				{
					const std::string appended = AppendAssistantChunk(chat, session, text);
					if (browser && !appended.empty()) uam::PushStreamToken(browser, chat.id, session.current_assistant_message_index, appended);
					ScheduleChatSave(app, chat, 0.5);
				}
			}
			else if (JsonDiagnosticStringValue(payload, "step_type") == "tool")
			{
				const int64_t index = AntigravityCounter(payload, "step_index");
				const nlohmann::json details = JsonObjectValue(payload, "tool_info");
				uam::AcpToolCallState& tool = UpsertToolCall(session, "antigravity:" + identity + ":" + std::to_string(index));
				tool.title = JsonDiagnosticStringValue(payload, "tool_name");
				tool.kind = "tool";
				tool.args_json = JsonObjectValue(details, "parameters").dump();
				tool.content = JsonDiagnosticStringValue(details, "output");
				const bool failed = !JsonObjectValue(details, "error").empty();
				tool.status = failed ? "failed" : JsonDiagnosticStringValue(payload, "state") == "DONE" ? "completed" : "in_progress";
				if (failed) tool.content = uam::strings::JoinNonEmpty(std::vector<std::string>{tool.content, JsonDiagnosticStringValue(JsonObjectValue(details, "error"), "message")}, "\n");
				AppendToolTurnEventIfNeeded(session, tool.id);
				SyncAcpToolCallsToAssistantMessage(chat, session, true);
				ScheduleChatSave(app, chat, 0.5);
			}
			return true;
		}
		const int64_t turn = AntigravityCounter(payload, "num_turns");
		const std::string status = JsonDiagnosticStringValue(payload, "status");
		if (status == "SUCCESS" && turn <= 0)
		{
			FailAcpTurnOrSession(session, &chat, "Antigravity returned an invalid successful result turn counter.");
			SaveChatQuietly(app, chat);
			return true;
		}
		if (turn > 0 && turn <= session.antigravity_completed_turns) return true;
		if (status != "SUCCESS")
		{
			FinalizeActiveAcpToolCallsAsFailed(chat, session);
			FailAcpTurnOrSession(session, &chat, uam::strings::NonEmptyOrFallback(JsonDiagnosticStringValue(payload, "error"), "Antigravity turn ended with status " + status + "."));
		}
		else
		{
			const bool has_answer = std::ranges::any_of(session.turn_events, [](const uam::AcpTurnEventState& item) { return item.type == "assistant_text" && !item.text.empty(); });
			if (!has_answer) AppendAssistantChunk(chat, session, JsonDiagnosticStringValue(payload, "response"));
			// Result usage includes the whole native conversation, even across resume.
			// Only observed completed-step counters describe this UAM turn accurately.
			if (session.current_assistant_message_index >= 0 && session.current_assistant_message_index < static_cast<int>(chat.messages.size()))
			{
				Message& answer = chat.messages[static_cast<std::size_t>(session.current_assistant_message_index)];
				answer.tokens_input = static_cast<int>(std::min<int64_t>(session.antigravity_input_tokens, std::numeric_limits<int>::max()));
				answer.tokens_output = static_cast<int>(std::min<int64_t>(session.antigravity_output_tokens, std::numeric_limits<int>::max()));
			}
			session.antigravity_completed_turns = turn;
			CompletePromptTurnAndHandleGoalLoop(app, session, chat, kAcpLifecycleReady, browser);
		}
		if (browser) uam::PushStreamDone(browser, chat.id);
		SaveChatQuietly(app, chat);
		MarkAcpChatUnseenIfBackground(app, chat);
	}
	catch (const std::exception& error)
	{
		InvalidateAcpTransport(app, session, chat, std::string("Antigravity stream message failed: ") + error.what());
	}
	return true;
}
