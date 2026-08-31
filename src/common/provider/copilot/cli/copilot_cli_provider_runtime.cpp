#include "common/provider/copilot/cli/copilot_cli_provider_runtime.h"

#include "computer_use/computer_use_mcp_config.h"
#include "common/config/approval_modes.h"
#include "common/chat/chat_ids.h"
#include "common/paths/app_paths.h"
#include "common/paths/path_utils.h"
#include "common/provider/provider_ids.h"
#include "common/provider/runtime/provider_runtime_internal.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "common/utils/env_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"
#include "core/chat_import_utils.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string_view>

namespace
{
	std::vector<std::string> CopilotFlagsFromSettings(const AppSettings& settings)
	{
		return uam::provider_runtime_internal::BuildProviderFlagsArgv(settings);
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

	std::optional<ChatSession> LoadCopilotSessionStateChat(
	    const std::filesystem::path& session_directory,
	    const std::filesystem::path& workspace_filter,
	    const ProviderRuntimeHistoryLoadOptions& options)
	{
		const std::string session_id = session_directory.filename().string();
		if (!uam::chat_ids::IsSafeStorageChatId(session_id))
		{
			return std::nullopt;
		}

		const std::string workspace_directory = ReadCopilotWorkspaceDirectory(session_directory);
		if (workspace_directory.empty() ||
		    (!workspace_filter.empty() && !FolderDirectoryMatches(workspace_directory, workspace_filter)))
		{
			return std::nullopt;
		}

		const std::filesystem::path events_file = session_directory / "events.jsonl";

		ChatSession chat;
		chat.id = session_id;
		chat.native_session_id = session_id;
		chat.branch_root_chat_id = session_id;
		chat.provider_id = uam::provider_ids::kCopilotCli;
		chat.workspace_directory = workspace_directory;
		bool has_user_message = false;
		bool is_subagent = false;

		uam::io::ForEachTextFileLine(
		    events_file,
		    [&](const std::string& line)
		    {
			    try
			    {
				    const nlohmann::json record = nlohmann::json::parse(line);
				    const std::string_view type = uam::nlohmann_json::TrimmedStringViewOrEmpty(record, "type");
				    const auto data_it = record.find("data");
				    if (data_it == record.end() || !data_it->is_object())
				    {
					    return true;
				    }
				    const nlohmann::json& data = *data_it;
				    const std::string timestamp{uam::nlohmann_json::TrimmedStringViewOrEmpty(record, "timestamp")};

				    if (type == "session.start")
				    {
					    const std::string_view parent_session =
					        uam::nlohmann_json::TrimmedStringViewOrEmpty(data, "detachedFromSpawningParentSessionId");
					    is_subagent = !parent_session.empty();
					    chat.created_at = std::string{uam::nlohmann_json::TrimmedStringViewOrEmpty(data, "startTime")};
					    if (chat.created_at.empty()) chat.created_at = timestamp;
					    chat.updated_at = chat.created_at;
					    chat.model_id = std::string{uam::nlohmann_json::TrimmedStringViewOrEmpty(data, "selectedModel")};
					    return true;
				    }

				    if (type != "user.message" && type != "assistant.message")
				    {
					    return true;
				    }
				    if (type == "user.message" &&
				        !uam::nlohmann_json::TrimmedStringViewOrEmpty(data, "parentAgentTaskId").empty())
				    {
					    is_subagent = true;
					    return false;
				    }
				    const std::string content{uam::nlohmann_json::StringViewOrEmpty(data, "content")};
				    if (uam::strings::TrimAsciiView(content).empty())
				    {
					    return true;
				    }
				    if (options.native_max_messages > 0 && chat.messages.size() >= options.native_max_messages)
				    {
					    return false;
				    }

				    Message message;
				    message.role = type == "user.message" ? MessageRole::User : MessageRole::Assistant;
				    message.content = content;
				    message.created_at = timestamp;
				    message.provider = uam::provider_ids::kCopilotCli;
				    has_user_message = has_user_message || message.role == MessageRole::User;
				    if (!timestamp.empty()) chat.updated_at = timestamp;
				    if (message.role == MessageRole::Assistant)
				    {
					    const std::string model{uam::nlohmann_json::TrimmedStringViewOrEmpty(data, "model")};
					    if (!model.empty()) chat.model_id = model;
				    }
				    chat.messages.push_back(std::move(message));
			    }
			    catch (const nlohmann::json::exception&)
			    {
				    // Active Copilot sessions can end with one incomplete append-only JSONL record.
			    }
			    return true;
		    });

		if (is_subagent || !has_user_message)
		{
			return std::nullopt;
		}
		if (chat.created_at.empty()) chat.created_at = chat.messages.front().created_at;
		if (chat.updated_at.empty()) chat.updated_at = chat.created_at;
		chat.title = uam::BuildImportedChatTitle(chat.messages, chat.created_at);
		return chat;
	}
} // namespace

const char* CopilotCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kCopilotCli;
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
	uam::provider_runtime_internal::AppendArgs(argv, CopilotFlagsFromSettings(provider_settings));
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
	std::vector<std::string> argv = {"copilot", "--acp", "--stdio"};
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--effort", NormalizeCopilotReasoningEffort(chat.reasoning_effort));
	if (uam::computer_use::UsesUamBackend(chat) && uam::computer_use::IsPortableMcpChatId(chat.id))
	{
		argv.push_back("--allow-tool=uam-computer(computer_observe),uam-computer(computer_action)");
	}
	return argv;
}

std::string CopilotCliProviderRuntime::OnAcpValidateResumeId(const ChatSession& chat) const
{
	return uam::acp_detail::ValidGenericAcpResumeId(chat);
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

std::filesystem::path CopilotSessionStatePath()
{
	if (const std::optional<std::filesystem::path> copilot_home = uam::env::GetTrimmedPath("COPILOT_HOME"))
	{
		return *copilot_home / "session-state";
	}
	if (const std::optional<std::filesystem::path> home = uam::env::GetUserHomePath())
	{
		return *home / ".copilot" / "session-state";
	}
	return uam::paths::CurrentPathOrDot() / ".copilot" / "session-state";
}

std::vector<ChatSession> LoadCopilotSessionStateChats(
    const std::filesystem::path& session_state_root,
    const std::filesystem::path& workspace_filter,
    const ProviderRuntimeHistoryLoadOptions& options,
    std::string* error_out)
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

	constexpr auto directory_options = std::filesystem::directory_options::skip_permission_denied;
	for (std::filesystem::directory_iterator it(session_state_root, directory_options, error), end;
	     !error && it != end;
	     it.increment(error))
	{
		if (!uam::paths::IsDirectoryEntryNoThrow(*it))
		{
			continue;
		}
		if (std::optional<ChatSession> chat = LoadCopilotSessionStateChat(it->path(), workspace_filter, options))
		{
			chats.push_back(std::move(*chat));
		}
	}
	if (error && error_out != nullptr) *error_out = "Could not finish scanning Copilot history: " + error.message();
	return chats;
}
