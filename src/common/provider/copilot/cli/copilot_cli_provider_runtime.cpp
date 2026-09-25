#include "common/provider/copilot/cli/copilot_cli_provider_runtime.h"

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
		.fallback_version = "1.0.80",
		.minimum_version = "1.0.60",
		.version_policy = ProviderCliVersionPolicy::MinimumSemver,
		.verified_at = "2026-08-27",
	};
	return &policy;
}

bool CopilotCliProviderRuntime::OnAcpHandleError(uam::AppState& app, uam::AcpSessionState& session, ChatSession& chat,
    const uam::acp_detail::AcpResponseFailureDetails& details) const
{
	using namespace uam::acp_detail;
	if (!details.failure.has_code || details.failure.code != -32002) return false;
	return RetrySessionNewAfterInvalidLoad(app, session, chat, details);
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
	if (execution_host.id != uam::execution_hosts::kLocalHostId)
	{
		return true;
	}
	ProviderCliCompatibilityService().Poll(app);
	if (const std::string compatibility_error = ProviderRuntimeRegistry::ResolveById(uam::provider_ids::kCopilotCli).LocalCliCompatibilityError(app); !compatibility_error.empty())
	{
		if (error_out != nullptr)
			*error_out = compatibility_error;
		return false;
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
	std::vector<std::string> argv = {"copilot", "--acp", "--stdio"};
	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--effort", NormalizeCopilotReasoningEffort(chat.reasoning_effort));
	if (uam::computer_use::UsesUamBackend(chat) && uam::computer_use::IsPortableMcpChatId(chat.id))
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

		const int id = uam::acp_detail::NextAcpRequestId(session, uam::acp_methods::kSessionSetConfigOption);
		session.reasoning_change_request_id = id;
		session.reasoning_change_previous_id = selected_model->default_reasoning_effort;
		session.reasoning_change_previous_chat_id = selected_model->default_reasoning_effort;
		session.reasoning_change_requested_id = desired_effort;
		if (!uam::acp_detail::WriteAcpMessage(session, uam::acp_detail::BuildSetConfigOptionRequest(id, session.session_id, "reasoning_effort", desired_effort)))
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
