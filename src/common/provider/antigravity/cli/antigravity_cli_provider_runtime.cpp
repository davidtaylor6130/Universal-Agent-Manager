#include "common/provider/antigravity/cli/antigravity_cli_provider_runtime.h"

#include "common/config/approval_modes.h"
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"

#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_goal_loop.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/utils/uuid.h"
#include "cef/cef_push.h"
#include <algorithm>
#include <limits>
#include <set>

namespace
{
	/// <summary>Preserves expert arguments while enforcing a required native Plan ceiling before positional data.</summary>
	void AppendAntigravityModeArgs(std::vector<std::string>& argv, const ChatSession& chat)
	{
		const std::string mode = uam::approval_modes::AppApprovalModeOrEmpty(chat.approval_mode);
		if (mode != uam::approval_modes::kPlanApprovalMode && mode != uam::approval_modes::kAcceptEditsApprovalMode) return;
		if (mode == uam::approval_modes::kPlanApprovalMode)
		{
			for (std::size_t index = 1; index < argv.size() && argv[index] != "--";)
			{
				const std::string& argument = argv[index];
				const bool mode_flag = argument == "--mode" || argument == "-mode";
				const bool inline_mode = argument.starts_with("--mode=") || argument.starts_with("-mode=");
				const bool skip_flag = argument == "--dangerously-skip-permissions" || argument == "-dangerously-skip-permissions" ||
				    argument.starts_with("--dangerously-skip-permissions=") || argument.starts_with("-dangerously-skip-permissions=");
				if (!mode_flag && !inline_mode && !skip_flag)
				{
					++index;
					continue;
				}
				const std::size_t count = mode_flag && index + 1 < argv.size() && !argv[index + 1].starts_with('-') ? 2 : 1;
				argv.erase(argv.begin() + static_cast<std::ptrdiff_t>(index), argv.begin() + static_cast<std::ptrdiff_t>(index + count));
			}
		}
		const std::vector<std::string>::iterator positional = std::find(argv.begin() + std::min(std::size_t{1}, argv.size()), argv.end(), "--");
		argv.insert(positional, {"--mode", mode == uam::approval_modes::kPlanApprovalMode ? "plan" : "accept-edits"});
	}
}

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
	uam::provider_runtime_internal::AppendArgs(argv, uam::command_line::SplitWords(merged.provider_extra_flags));
	AppendAntigravityModeArgs(argv, chat);
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

	/// <summary>Native denial results identify action kinds, not individual tool steps.</summary>
	void ApplyAntigravityDenialEvidence(const nlohmann::json& payload, uam::AcpSessionState& session, ChatSession& chat)
	{
		const nlohmann::json::const_iterator actions = payload.find("denied_actions");
		if (actions == payload.end() || !actions->is_array()) return;
		std::set<std::string> seen;
		const std::size_t limit = std::min<std::size_t>(actions->size(), 8);
		for (std::size_t index = 0; index < limit; ++index)
		{
			const nlohmann::json& action = (*actions)[index];
			if (!action.is_object()) continue;
			const std::string kind = uam::strings::SafeLine(uam::acp_detail::JsonDiagnosticStringValue(action, "action"), 128, true);
			const std::string name = uam::strings::SafeLine(uam::acp_detail::JsonDiagnosticStringValue(action, "display_name"), 128, true);
			if ((kind.empty() && name.empty()) || !seen.insert(kind + "\n" + name).second) continue;
			std::vector<std::size_t> matches;
			if (kind == "mcp" && name == "CallMcpTool")
				for (std::size_t tool_index = 0; tool_index < session.tool_calls.size(); ++tool_index)
					if (session.tool_calls[tool_index].title == "call_mcp_tool") matches.push_back(tool_index);
			for (const std::size_t tool_index : matches)
			{
				uam::AcpToolCallState& tool = session.tool_calls[tool_index];
				if (matches.size() != 1 && tool.status == "failed") continue;
				if (tool.status != "failed") tool.status = matches.size() == 1 ? "failed" : "unknown";
				const std::string evidence = matches.size() == 1
				    ? "Antigravity denied this MCP action."
				    : "Antigravity reported a denied MCP action but cannot identify which tool step. Outcome unknown.";
				tool.content = uam::strings::JoinNonEmpty(std::vector<std::string>{tool.content, evidence}, "\n");
			}
			if (matches.size() != 1)
			{
				const std::string id = "antigravity:denied-result:" + session.session_id + ":" + std::to_string(index);
				uam::AcpToolCallState& evidence = uam::acp_detail::UpsertToolCall(session, id);
				evidence.title = "Antigravity permission result";
				evidence.kind = "permission";
				evidence.status = "failed";
				evidence.content = "Antigravity reported a denied action: " + (name.empty() ? kind : name) +
				    ". The native result cannot identify an individual tool step.";
				uam::acp_detail::AppendToolTurnEventIfNeeded(session, id);
			}
		}
		if (!seen.empty()) uam::acp_detail::SyncAcpToolCallsToAssistantMessage(chat, session, true);
	}
}

std::vector<std::string> AntigravityCliProviderRuntime::BuildStructuredLaunchArgv(const ProviderProfile& profile, const ChatSession& chat) const
{
	std::vector<std::string> argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "agy");
	argv.insert(argv.end(), {"--input-format", "stream-json", "--output-format", "stream-json"});
	uam::provider_runtime_internal::AppendResumeArgs(argv, profile, chat.native_session_id);
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", chat.model_id);
	AppendAntigravityModeArgs(argv, chat);
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
		ApplyAntigravityDenialEvidence(payload, session, chat);
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

const ProviderCliPolicy* AntigravityCliProviderRuntime::CliVersionPolicy() const
{
	static constexpr ProviderCliPolicy policy
	{
		.provider_id = uam::provider_ids::kAntigravityCli,
		.fallback_title = "Antigravity CLI",
		.executable_name = "agy",
		.version_probe_command = "agy --version",
		.provider_managed = true,
	};
	return &policy;
}
