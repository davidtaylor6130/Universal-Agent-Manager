#include "common/provider/copilot/cli/copilot_cli_provider_runtime.h"
#include "common/provider/copilot/cli/copilot_child_lifecycle.h"
#include "common/provider/copilot/cli/copilot_mcp_presentation.h"

#include "computer_use/computer_use_mcp_config.h"
#include "common/config/execution_host_config.h"
#include "common/platform/platform_services.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/state/app_state.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/config/approval_modes.h"
#include "common/chat/chat_ids.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/provider/provider_ids.h"
#include "common/provider/native_agent_history.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/utils/env_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"
#include "common/runtime/terminal/terminal_idle_classifier.h"
#include "core/chat_import_utils.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace
{
	std::string NormalizeCopilotReasoningEffort(std::string_view value)
	{
		constexpr auto efforts = std::to_array<std::string_view>({"none", "minimal", "low", "medium", "high", "xhigh", "max"});
		const auto found = uam::strings::FindEqualIgnoreCase(efforts, uam::strings::TrimAsciiView(value));
		return found ? std::string(*found) : std::string{};
	}

	void AppendCopilotModeArgs(std::vector<std::string>& argv, const ChatSession& chat)
	{
		uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", chat.model_id);
		uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--effort", NormalizeCopilotReasoningEffort(chat.reasoning_effort));

		const std::string approval_mode = uam::strings::Trim(chat.approval_mode);
		if (approval_mode == uam::approval_modes::kPlanApprovalMode)
		{
			argv.push_back("--plan");
		}
	}

	std::string ParseCopilotWorkspaceValue(std::string_view value)
	{
		value = uam::strings::TrimAsciiView(value);
		if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
		{
			try
			{
				return nlohmann::json::parse(value).get<std::string>();
			}
			catch (const nlohmann::json::exception&)
			{
				return {};
			}
		}
		if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'')
		{
			std::string parsed(value.substr(1, value.size() - 2));
			std::size_t escaped_quote = 0;
			while ((escaped_quote = parsed.find("''", escaped_quote)) != std::string::npos)
			{
				parsed.replace(escaped_quote, 2, "'");
				++escaped_quote;
			}
			return parsed;
		}
		return std::string(value);
	}

	std::string ReadCopilotWorkspaceDirectory(const std::filesystem::path& session_directory)
	{
		std::string workspace_directory;
		uam::io::ForEachTextFileLine(
		    session_directory / "workspace.yaml",
		    [&](const std::string& line)
		    {
			    if (!uam::strings::StartsWith(line, "cwd:"))
			    {
				    return true;
			    }
			    workspace_directory = ParseCopilotWorkspaceValue(std::string_view(line).substr(4));
			    return false;
		    });
		return uam::strings::Trim(workspace_directory);
	}

	std::optional<ChatSession> ReadCopilotSessionStateChat(
	    const std::filesystem::path& session_directory, const std::filesystem::path& workspace_filter,
	    const ProviderRuntimeHistoryLoadOptions& options, std::string* error_out,
	    std::stop_token stop_token, bool allow_child, std::string_view agent_filter = {}, std::string_view execution_host_id = "local")
	{
		if (error_out) error_out->clear();
		const std::string session_id = session_directory.filename().string();
		if (!uam::chat_ids::IsSafeStorageChatId(session_id)) return std::nullopt;
		std::string workspace_directory = ReadCopilotWorkspaceDirectory(session_directory);
		const std::filesystem::path events_file = session_directory / "events.jsonl";
		std::error_code file_error;
		const auto bytes = std::filesystem::file_size(events_file, file_error);
		if (file_error || bytes > 64U * 1024U * 1024U)
		{
			if (error_out && file_error != std::errc::no_such_file_or_directory)
				*error_out = "Copilot transcript could not be read or exceeds the history size limit.";
			return std::nullopt;
		}
		ChatSession chat;
		chat.id = session_id;
		chat.native_session_id = session_id;
		chat.branch_root_chat_id = session_id;
		chat.provider_id = uam::provider_ids::kCopilotCli;
		bool has_user = false;
		bool matching_agent = false;
		bool child_session = false;
		bool invalid_identity = false;
		bool malformed_last = false;
		bool malformed_interior = false;
		bool completed_text = false;
		std::string completed_text_id;
		std::unordered_map<std::string, std::pair<std::size_t, std::size_t>> tools;
		nlohmann::json mcp_origins = nlohmann::json::object();
		std::unordered_map<std::string, std::string> texts;
		std::unordered_map<std::string, std::string> thoughts;
		std::unordered_set<std::string> finalized_text_ids;
		std::unordered_set<std::string> events;
		const auto value = [](const nlohmann::json& object, std::string_view key)
		{
			return std::string(uam::nlohmann_json::StringViewOrEmpty(object, key));
		};
		const auto assistant = [&](const std::string& timestamp, bool separate = false) -> Message*
		{
			if (separate || chat.messages.empty() || chat.messages.back().role != MessageRole::Assistant)
			{
				if (options.native_max_messages && chat.messages.size() >= options.native_max_messages) return nullptr;
				Message message;
				message.role = MessageRole::Assistant;
				message.created_at = timestamp;
				message.provider = chat.provider_id;
				chat.messages.push_back(std::move(message));
			}
			return &chat.messages.back();
		};
		const auto block = [](Message& message, const std::string& type, const std::string& text, const std::string& identity, const std::string& tool = {})
		{
			MessageBlock entry;
			entry.type = type;
			entry.text = text;
			entry.request_id_json = identity;
			entry.tool_call_id = tool;
			message.blocks.push_back(std::move(entry));
		};
		const bool read = uam::io::ForEachTextFileLine(events_file, [&](const std::string& line)
		{
			if (stop_token.stop_requested()) return false;
			if (uam::strings::TrimAsciiView(line).empty()) return true;
			const nlohmann::json record = nlohmann::json::parse(line, nullptr, false);
			if (!record.is_object()) { malformed_last = true; return true; }
			if (malformed_last) { malformed_interior = true; malformed_last = false; }
			const auto data_it = record.find("data");
			if (data_it == record.end() || !data_it->is_object()) return true;
			const auto& data = *data_it;
			const std::string type = value(record, "type");
			const std::string id = value(record, "id");
			if (!id.empty() && !events.insert(id).second) return true;
			const std::string timestamp = value(record, "timestamp");
			if (!timestamp.empty()) chat.updated_at = timestamp;
			if (type == "session.start" && value(record, "agentId").empty())
			{
				const std::string native_id = value(data, "sessionId");
				if (!native_id.empty() && native_id != session_id) { invalid_identity = true; return false; }
				if (uam::copilot::presentation::ValidSessionReset(data, type, session_id)) mcp_origins = nlohmann::json::object();
				chat.parent_chat_id = value(data, "detachedFromSpawningParentSessionId");
				child_session = !chat.parent_chat_id.empty();
				chat.created_at = value(data, "startTime");
				if (chat.created_at.empty()) chat.created_at = timestamp;
				chat.model_id = value(data, "selectedModel");
				if (workspace_directory.empty()) workspace_directory = value(uam::acp_detail::JsonObjectValue(data, "context"), "cwd");
				return true;
			}
			const std::string record_agent = value(record, "agentId");
			const std::string parent_tool = value(data, "parentToolCallId");
			if (!agent_filter.empty())
			{
				if (record_agent != agent_filter) return true;
				matching_agent = true;
				if (type.starts_with("subagent."))
				{
					const std::string title = value(data, "agentDisplayName");
					if (!title.empty()) chat.title = title;
					return true;
				}
			}
			else if (!type.starts_with("subagent.") && (!record_agent.empty() || !parent_tool.empty()))
			{
				const auto owner = tools.find(parent_tool);
				if (owner != tools.end() && !record_agent.empty())
				{
					ToolCall& tool = chat.messages[owner->second.first].tool_calls[owner->second.second];
					tool.task_id = record_agent;
					tool.sub_agent_id = BuildCopilotAgentHistoryId(session_id, record_agent, execution_host_id);
				}
				return true;
			}
			if (uam::copilot::presentation::ValidSessionReset(data, type, session_id)) mcp_origins = nlohmann::json::object();
			if (type == "assistant.turn_start") uam::copilot::presentation::AcceptedBoundary(mcp_origins, record, data);
			if (type == "user.message")
			{
				completed_text = false;
				completed_text_id.clear();
				// Copilot CLI 1.0.10x stamps parentAgentTaskId on every top-level user message; it does not mark a sub-agent session.
				const std::string content = value(data, "content");
				if (uam::strings::TrimAsciiView(content).empty()) return true;
				uam::copilot::presentation::AcceptedBoundary(mcp_origins, record, data);
				if (options.native_max_messages && chat.messages.size() >= options.native_max_messages) return false;
				Message message;
				message.role = MessageRole::User;
				message.content = content;
				message.created_at = timestamp;
				message.provider = chat.provider_id;
				chat.messages.push_back(std::move(message));
				has_user = true;
				return true;
			}
			if (type == "assistant.message") uam::copilot::presentation::ObserveRequests(mcp_origins, record, data);
			if (type == "assistant.message" || type == "assistant.message_delta" || type == "assistant.reasoning" || type == "assistant.reasoning_delta")
			{
				const bool thought = type.starts_with("assistant.reasoning");
				const bool delta = type.ends_with("_delta");
				const std::string key = value(data, thought ? "reasoningId" : "messageId");
				const std::string incoming = value(data, delta ? "deltaContent" : "content");
				if (incoming.empty()) return true;
				// A replayed older final must not complete or split a newer response.
				if (!thought && !delta && !key.empty() && finalized_text_ids.contains(key))
				{
					const auto previous = texts.find(key);
					if (previous != texts.end() && previous->second == incoming) return true;
				}
				std::string piece = incoming;
				if (!key.empty())
				{
					std::string& previous = (thought ? thoughts : texts)[key];
					if (delta) previous += incoming;
					else { if (!incoming.starts_with(previous)) return true; piece = incoming.substr(previous.size()); previous = incoming; }
				}
				if (piece.empty())
				{
					if (!thought && !delta)
					{
						completed_text = true;
						completed_text_id = key;
						if (!key.empty()) finalized_text_ids.insert(key);
					}
					return true;
				}
				const bool separate = completed_text && (thought || key.empty() || key != completed_text_id);
				Message* message = assistant(timestamp, separate);
				if (!message) return false;
				if (separate) { completed_text = false; completed_text_id.clear(); }
				if (thought) message->thoughts += piece;
				else message->content += piece;
				if (!thought && !delta)
				{
					completed_text = true;
					completed_text_id = key;
					if (!key.empty()) finalized_text_ids.insert(key);
				}
				block(*message, thought ? "thought" : "assistant_text", piece, (thought ? "copilot-reasoning:" : "copilot:") + key);
				const std::string model = value(data, "model");
				if (!model.empty()) { chat.model_id = model; message->model_id = model; }
				return true;
			}
			if (type == "tool.execution_start" || type == "tool.execution_complete" || type.starts_with("subagent."))
			{
				const std::string tool_id = value(data, "toolCallId");
				if (tool_id.empty()) return true;
				auto found = tools.find(tool_id);
				if (found == tools.end())
				{
					Message* message = assistant(timestamp);
					if (!message) return false;
					const std::size_t index = message->tool_calls.size();
					ToolCall tool;
					tool.id = tool_id;
					message->tool_calls.push_back(std::move(tool));
					block(*message, "tool_call", "", "", tool_id);
					found = tools.emplace(tool_id, std::pair{chat.messages.size() - 1, index}).first;
				}
				ToolCall& tool = chat.messages[found->second.first].tool_calls[found->second.second];
				if (type == "tool.execution_start") uam::copilot::presentation::Observe(mcp_origins, record, data);
				const std::string name = value(data, "toolName");
				if (!name.empty())
				{
					tool.name = value(data, "toolTitle");
					if (tool.name.empty()) tool.name = name;
					tool.is_sub_agent = GetCopilotCliProviderRuntime().ProviderRecognizesSubagentTool(name);
					tool.kind = tool.is_sub_agent ? "sub-agent" : name == "bash" || name == "powershell" || name == "shell" ? "execute" : name == "edit" || name == "create" ? "edit" : "other";
				}
				if (data.contains("arguments")) tool.args_json = data["arguments"].dump();
				if (type == "tool.execution_complete")
				{
					tool.status = uam::nlohmann_json::BoolFieldStrict(data, "success").value_or(false) ? "completed" : "failed";
					tool.result_text = uam::acp_detail::ContentTextFromJson(uam::acp_detail::JsonObjectValue(data, "result"));
					const std::string error = value(uam::acp_detail::JsonObjectValue(data, "error"), "message");
					if (!error.empty()) tool.result_text = error;
					tool.result_text = uam::copilot::presentation::Consume(mcp_origins, record, data, uam::nlohmann_json::BoolFieldStrict(data, "success").value_or(false), tool.result_text);
				}
				else if (type.starts_with("subagent."))
				{
					tool.is_sub_agent = true;
					tool.kind = "sub-agent";
					const std::string task = value(record, "agentId");
					if (!task.empty()) tool.task_id = task;
					std::string title = value(data, "agentDisplayName");
					if (title.empty()) title = value(data, "agentName");
					if (!title.empty()) tool.sub_agent_title = tool.name = title;
					const std::string child = value(data, "sessionId");
					if (uam::chat_ids::IsSafeStorageChatId(child)) tool.sub_agent_id = child;
					else if (!task.empty()) tool.sub_agent_id = BuildCopilotAgentHistoryId(session_id, task, execution_host_id);
					const std::string child_id = uam::chat_ids::IsSafeStorageChatId(child) ? child : !task.empty() ? BuildCopilotAgentHistoryId(session_id, task, execution_host_id) : "";
					uam::copilot::ApplyChildLifecycle(tool.sub_agent_children, child_id, title, type, record);
					tool.status = type == "subagent.started" ? "in_progress" : type == "subagent.completed" ? "completed" : type == "subagent.failed" ? "failed" : tool.status;
				}
				else tool.status = "in_progress";
				return true;
			}
			if (type == "session.compaction_complete" && uam::nlohmann_json::BoolFieldStrict(data, "success").value_or(false))
			{
				Message* message = assistant(timestamp);
				if (!message) return false;
				const std::string identity = value(data, "compactionAttemptId");
				block(*message, "context_compaction", value(data, "summaryContent"), "copilot-compaction:" + (identity.empty() ? id : identity));
				message->blocks.back().compaction_tokens_before = uam::nlohmann_json::IntFieldStrict(data, "preCompactionTokens").value_or(-1);
				message->blocks.back().compaction_tokens_after = uam::nlohmann_json::IntFieldStrict(data, "postCompactionTokens").value_or(-1);
				return true;
			}
			return true;
		});
		if (stop_token.stop_requested()) return std::nullopt;
		if (!read || malformed_interior)
		{
			if (error_out) *error_out = malformed_interior ? "Copilot history contains an invalid interior record; refresh was not applied." : "Copilot history could not be read.";
			return std::nullopt;
		}
		if (invalid_identity || (agent_filter.empty() ? !has_user : !matching_agent || chat.messages.empty()) || (!allow_child && child_session) || workspace_directory.empty() ||
		    (!workspace_filter.empty() && !uam::ImportedWorkspaceMatchesFolder(uam::paths::PathFromUtf8(workspace_directory), workspace_filter))) return std::nullopt;
		chat.workspace_directory = workspace_directory;
		if (chat.created_at.empty()) chat.created_at = chat.messages.front().created_at;
		if (chat.updated_at.empty()) chat.updated_at = chat.created_at;
		if (chat.title.empty()) chat.title = uam::BuildImportedChatTitle(chat.messages, chat.created_at);
		if (!agent_filter.empty())
		{
			chat.id = BuildCopilotAgentHistoryId(session_id, agent_filter, execution_host_id);
			if (chat.id.empty()) return std::nullopt;
			chat.native_session_id.clear();
			chat.branch_root_chat_id = chat.id;
			chat.parent_chat_id = session_id;
			chat.execution_host_id = execution_host_id;
			chat.imported_read_only = true;
		}
		return chat;
	}
} // namespace

std::optional<ChatSession> LoadCopilotSessionStateChat(
    const std::filesystem::path& session_directory, const std::filesystem::path& workspace_filter,
    const ProviderRuntimeHistoryLoadOptions& options, std::string* error_out, std::stop_token stop_token, std::string_view execution_host_id)
{
	return ReadCopilotSessionStateChat(session_directory, workspace_filter, options, error_out, stop_token, true, {}, execution_host_id);
}

std::string BuildCopilotAgentHistoryId(std::string_view parent_session_id, std::string_view agent_id, std::string_view execution_host_id)
{
	return uam::native_agents::BuildHistoryId(uam::provider_ids::kCopilotCli, parent_session_id, agent_id, execution_host_id);
}

std::optional<CopilotAgentHistoryReference> ParseCopilotAgentHistoryId(std::string_view reference)
{
	const auto parsed = uam::native_agents::ParseHistoryId(reference);
	if (!parsed || parsed->provider_id != uam::provider_ids::kCopilotCli) return std::nullopt;
	return CopilotAgentHistoryReference{parsed->parent_session_id, parsed->agent_id, parsed->execution_host_id};
}

std::optional<ChatSession> LoadCopilotAgentSessionStateChat(
	const std::filesystem::path& parent_session_directory, std::string_view agent_id,
	const std::filesystem::path& workspace_filter, std::string_view execution_host_id,
	std::string* error_out, std::stop_token stop_token)
{
	if (BuildCopilotAgentHistoryId(parent_session_directory.filename().string(), agent_id, execution_host_id).empty())
	{
		if (error_out) *error_out = "The Copilot agent history identity is invalid or exceeds its size limit.";
		return std::nullopt;
	}
	return ReadCopilotSessionStateChat(parent_session_directory, workspace_filter, {}, error_out, stop_token, true, agent_id, execution_host_id);
}

namespace
{
constexpr auto kCopilotPromptCueTexts = std::to_array<std::string_view>({"/ commands", "? help"});
}

bool CopilotCliProviderRuntime::RecentOutputIndicatesInputPrompt(std::string_view recent_output) const
{
	const std::string stripped = uam::RecentTerminalPromptScanText(recent_output);
	return uam::strings::Contains(stripped, "\xE2\x9D\xAF") && uam::strings::ContainsAny(stripped, kCopilotPromptCueTexts);
}

const ProviderCliPolicy* CopilotCliProviderRuntime::CliVersionPolicy() const
{
	static constexpr ProviderCliPolicy policy
	{
		.provider_id = uam::provider_ids::kCopilotCli,
		.npm_package = "@github/copilot",
		.fallback_title = "GitHub Copilot CLI",
		.executable_name = "copilot",
		.version_probe_command = "copilot --version",
		.homebrew_package = "copilot-cli",
		.winget_package = "GitHub.Copilot",
		.homebrew_cask = true,
		.preferred_version = "latest",
		.fallback_version = "1.0.98",
		.minimum_version = "1.0.60",
		.version_policy = ProviderCliVersionPolicy::MinimumSemver,
		.verified_at = "2026-10-16",
	};
	return &policy;
}

const char* CopilotCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kCopilotCli;
}


bool CopilotCliProviderRuntime::PrepareInteractiveSession(uam::AppState& app, ChatSession& chat, const ProviderProfile& provider, const std::string& resume_id, const ExecutionHost& execution_host, std::string* error_out) const
{
	if (error_out != nullptr)
	{
		error_out->clear();
	}
	if (execution_host.id == uam::execution_hosts::kLocalHostId)
	{
		ProviderCliCompatibilityService().Poll(app);
		if (const std::string compatibility_error = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli).LocalCliCompatibilityError(app); !compatibility_error.empty())
		{
			if (error_out != nullptr)
				*error_out = compatibility_error;
			return false;
		}
	}

	if (!resume_id.empty())
	{
		return true;
	}

	const std::string session_id = PlatformServicesFactory::Instance().process_service.GenerateUuid();
	if (session_id.empty())
	{
		if (error_out != nullptr)
			*error_out = "Failed to create a Copilot session id.";
		return false;
	}

	const std::string previous_session_id = chat.native_session_id;
	const std::unordered_map<std::string, std::string>::const_iterator previous_resolved_session = app.resolved_native_sessions_by_chat_id.find(chat.id);
	const std::string previous_resolved_session_id = previous_resolved_session == app.resolved_native_sessions_by_chat_id.end() ? std::string() : previous_resolved_session->second;
	const bool had_resolved_session = previous_resolved_session != app.resolved_native_sessions_by_chat_id.end();
	app.resolved_native_sessions_by_chat_id.erase(chat.id);
	chat.native_session_id = session_id;
	if (ProviderRuntime::SaveHistory(provider, app.data_root, chat))
	{
		return true;
	}

	chat.native_session_id = previous_session_id;
	if (had_resolved_session)
	{
		app.resolved_native_sessions_by_chat_id[chat.id] = previous_resolved_session_id;
	}
	if (error_out != nullptr)
		*error_out = "Failed to persist the Copilot session id.";
	return false;
}


std::vector<std::string> CopilotCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	const AppSettings provider_settings = uam::provider_runtime_internal::MergeProviderSettings(profile, settings);
	std::vector<std::string> argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "copilot");

	uam::provider_runtime_internal::AppendResumeArgs(argv, profile, chat.native_session_id);

	AppendCopilotModeArgs(argv, chat);
	uam::provider_runtime_internal::AppendArgs(argv, uam::provider_runtime_internal::BuildProviderFlagsArgv(provider_settings));
	return argv;
}

MessageRole CopilotCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const
{
	return uam::provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

std::vector<ChatSession> CopilotCliProviderRuntime::LoadHistory(const ProviderProfile&, const std::filesystem::path& data_root, const std::filesystem::path& native_history_chats_dir, const ProviderRuntimeHistoryLoadOptions& options) const
{
	if (!native_history_chats_dir.empty())
	{
		return LoadCopilotSessionStateChats(native_history_chats_dir, {}, options);
	}
	return uam::provider_runtime_internal::LoadLocalChats(data_root);
}

bool CopilotCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return uam::provider_runtime_internal::SaveLocalChat(data_root, chat);
}

std::vector<std::string> CopilotCliProviderRuntime::BuildWorkerArgv(const ProviderProfile&, const AppSettings&, std::string_view prompt, std::string_view model_id) const
{
	std::vector<std::string> argv = {"copilot", "-p", std::string(prompt)};
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", model_id);
	uam::provider_runtime_internal::AppendArgs(argv, {
	                                                     "--no-auto-update",
	                                                     "--allow-all-tools",
	                                                     "--available-tools=__uam_text_only_worker_no_tools_7f4938d1__",
	                                                     "--disable-builtin-mcps",
	                                                     "--no-custom-instructions",
	                                                     "--no-remote",
	                                                     "--no-remote-export",
	                                                     "--disallow-temp-dir",
	                                                     "--silent",
	                                                 });
	return argv;
}

std::vector<std::string> CopilotCliProviderRuntime::BuildStructuredLaunchArgv(const ProviderProfile&, const ChatSession& chat) const
{
	std::vector<std::string> argv = {"copilot", "--headless", "--stdio", "--no-auto-update"};
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--effort", NormalizeCopilotReasoningEffort(chat.reasoning_effort));
	if (uam::computer_use::AvailableForChat(chat) && uam::computer_use::UsesUamBackend(chat) && uam::computer_use::IsPortableMcpChatId(chat.id))
	{
		argv.push_back("--allow-tool=uam-computer(computer_observe),uam-computer(computer_action)");
	}
	return argv;
}

std::string CopilotCliProviderRuntime::OnAcpMapApprovalModeId(const std::string& mode_id) const
{
	const std::string_view normalized = uam::strings::TrimAsciiView(mode_id);
	if (normalized == uam::approval_modes::kPlanApprovalMode || normalized == uam::approval_modes::kAcpPlanMode)
	{
		return uam::approval_modes::kAcpPlanMode;
	}
	if (normalized == uam::approval_modes::kDefaultApprovalMode || normalized == uam::approval_modes::kAcceptEditsApprovalMode || normalized == uam::approval_modes::kAcpAgentMode)
	{
		return uam::approval_modes::kAcpAgentMode;
	}
	return std::string(normalized);
}

const IProviderRuntime& GetCopilotCliProviderRuntime()
{
	static const CopilotCliProviderRuntime runtime;
	return runtime;
}


std::vector<ChatSession> LoadCopilotSessionStateChats(
    const std::filesystem::path& session_state_root,
    const std::filesystem::path& workspace_filter,
    const ProviderRuntimeHistoryLoadOptions& options,
    std::string* error_out, std::stop_token stop_token)
{
	std::vector<ChatSession> chats;
	if (error_out != nullptr) error_out->clear();
	std::error_code error;
	const bool root_exists = std::filesystem::exists(session_state_root, error);
	if (error)
	{
		if (error_out != nullptr) *error_out = "Could not inspect Copilot history: " + error.message();
		return chats;
	}
	if (!root_exists)
	{
		return chats;
	}
	if (!std::filesystem::is_directory(session_state_root, error) || error)
	{
		if (error_out != nullptr) *error_out = error ? "Could not inspect Copilot history: " + error.message() : "Copilot history path is not a directory.";
		return chats;
	}

	constexpr auto directory_options = std::filesystem::directory_options::none;
	for (std::filesystem::directory_iterator it(session_state_root, directory_options, error), end;
	     !error && !stop_token.stop_requested() && it != end;
	     it.increment(error))
	{
		if (!uam::paths::IsDirectoryEntryNoThrow(*it))
		{
			continue;
		}
		std::string session_error;
		if (std::optional<ChatSession> chat = ReadCopilotSessionStateChat(it->path(), workspace_filter, options, &session_error, stop_token, false))
		{
			chats.push_back(std::move(*chat));
		}
		if (error_out && error_out->empty() && !session_error.empty()) *error_out = std::move(session_error);
	}
	if (error && error_out != nullptr) *error_out = "Could not finish scanning Copilot history: " + error.message();
	return chats;
}

bool CopilotCliProviderRuntime::OnAcpConfigOptionsUpdated(uam::AcpSessionState& session, const nlohmann::json& config_options) const
{
	if (!config_options.is_array())
	{
		return false;
	}

	uam::AcpModelState* selected_model = nullptr;
	for (uam::AcpModelState& model : session.available_models)
	{
		if (model.id == session.current_model_id)
		{
			selected_model = &model;
			break;
		}
	}
	if (selected_model == nullptr)
	{
		return false;
	}

	std::vector<std::string> supported_efforts;
	std::string current_effort;
	for (const nlohmann::json& option : config_options)
	{
		if (uam::nlohmann_json::TrimmedStringValue(option, {"id"}) != "reasoning_effort")
		{
			continue;
		}
		for (const nlohmann::json& choice : uam::acp_detail::JsonArrayValue(option, "options"))
		{
			uam::ranges::PushUniqueNonEmptyString(supported_efforts, NormalizeCopilotReasoningEffort(uam::nlohmann_json::TrimmedStringValue(choice, {"value"})));
		}
		current_effort = NormalizeCopilotReasoningEffort(uam::nlohmann_json::TrimmedStringValue(option, {"currentValue"}));
		if (!uam::ranges::Contains(supported_efforts, current_effort))
		{
			current_effort.clear();
		}
		break;
	}

	const bool changed = selected_model->supported_reasoning_efforts != supported_efforts || selected_model->default_reasoning_effort != current_effort;
	selected_model->supported_reasoning_efforts = std::move(supported_efforts);
	selected_model->default_reasoning_effort = current_effort;
	return changed;
}

bool CopilotCliProviderRuntime::OnAcpReconcileModelOptions(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat) const
{
	if (session.model_discovery_only || (!chat.model_id.empty() && chat.model_id != session.current_model_id))
	{
		return false;
	}

	uam::AcpModelState* selected_model = nullptr;
	for (uam::AcpModelState& model : session.available_models)
	{
		if (model.id == session.current_model_id)
		{
			selected_model = &model;
			break;
		}
	}
	if (selected_model == nullptr || selected_model->supported_reasoning_efforts.empty())
	{
		return false;
	}

	const std::string desired_effort = NormalizeCopilotReasoningEffort(chat.reasoning_effort);
	if (uam::ranges::Contains(selected_model->supported_reasoning_efforts, desired_effort))
	{
		if (desired_effort == selected_model->default_reasoning_effort || !session.running || !session.session_ready || session.session_id.empty() || session.startup_model_request_id != 0 || session.mode_change_request_id != 0 || session.model_change_request_id != 0 || session.awaiting_model_config_options || session.reasoning_change_request_id != 0 || session.config_option_change_request_id != 0)
		{
			return false;
		}
		const bool prompt_is_queued_but_not_sent = session.processing && session.prompt_request_id == 0 && !session.queued_prompt.empty() && !session.waiting_for_permission && !session.waiting_for_user_input;
		if (uam::AcpSessionHasCancelableWork(session) && !prompt_is_queued_but_not_sent) return false;

		const int id = uam::acp_detail::NextAcpRequestId(session, "session.model.switchTo");
		session.reasoning_change_request_id = id;
		session.reasoning_change_previous_id = selected_model->default_reasoning_effort;
		session.reasoning_change_previous_chat_id = selected_model->default_reasoning_effort;
		session.reasoning_change_requested_id = desired_effort;
		const nlohmann::json request = {{"jsonrpc", "2.0"}, {"id", id}, {"method", "session.model.switchTo"},
		    {"params", {{"sessionId", session.session_id}, {"modelId", session.current_model_id}, {"reasoningEffort", desired_effort}}}};
		if (!uam::acp_detail::WriteAcpMessage(session, request))
		{
			session.pending_request_methods.erase(id);
			uam::acp_detail::ClearAcpReasoningChangeRequest(session);
			return false;
		}
		selected_model->default_reasoning_effort = desired_effort;
		return true;
	}

	if (chat.reasoning_effort == selected_model->default_reasoning_effort)
	{
		return false;
	}
	chat.reasoning_effort = selected_model->default_reasoning_effort;
	uam::acp_detail::SaveChatQuietly(app, chat);
	return true;
}

std::string CopilotCliProviderRuntime::LocalCliCompatibilityError(const uam::AppState& app) const
{
	const auto state_it = app.runtime_cli_versions_by_provider_id.find(uam::provider_ids::kCopilotCli);
	if (state_it == app.runtime_cli_versions_by_provider_id.end())
	{
		return "";
	}

	const uam::CliProviderVersionState& state = state_it->second;
	if (!state.checked)
	{
		return "Checking GitHub Copilot CLI compatibility. Try again in a moment.";
	}
	if (state.supported)
	{
		return "";
	}
	if (!state.installed_version.empty())
	{
		return "GitHub Copilot CLI 1.0.60 or newer is required (installed " + state.installed_version + "). Update it in Settings.";
	}
	return uam::strings::NonEmptyOrFallback(state.message, "GitHub Copilot CLI is not installed or its version could not be determined.") + " Open Settings to check or update it.";
}
