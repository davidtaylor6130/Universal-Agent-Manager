#include "common/provider/opencode/cli/opencode_cli_provider_runtime.h"
#include "common/provider/opencode/opencode_permission_policy.h"
#include "computer_use/computer_use_mcp_config.h"
#include "common/runtime/acp/acp_session_internal.h"
#include "app/opencode_native_activity_service.h"
#include "app/attachment_preview_service.h"
#include "common/provider/structured_attachments.h"
#include "common/utils/base64.h"
#include "common/provider/opencode_terminal_activity.h"
#include "common/paths/workspace_root.h"

#include "common/provider/provider_ids.h"
#include "common/utils/string_utils.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/config/execution_host_config.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/platform/platform_state_fields.h"
#include "common/state/app_state.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include "common/runtime/acp/acp_protocol_methods.h"
#include "common/runtime/acp/acp_statuses.h"
#include "common/usage/transcript_usage.h"
#include "common/utils/time_utils.h"
#include <thread>
#include <unordered_set>
#include "remote/runner_client.h"
#include "common/provider/runtime/provider_runtime_internal.h"

bool OpenCodeCliProviderRuntime::PrepareInteractiveLaunch(uam::AppState& app, uam::CliTerminalState& terminal, const ChatSession& chat,
    const ExecutionHost& host, std::vector<std::string>& argv, std::vector<std::pair<std::string, std::string>>& environment, std::string*) const
{
	ChatSession binding;
	binding.native_session_id = chat.native_session_id;
	binding.workspace_directory = uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, chat));
	uam::opencode_terminal::PrepareLaunch(terminal, binding, host, argv, environment);
	return true;
}

ProviderTerminalActivity OpenCodeCliProviderRuntime::PollInteractiveActivity(uam::CliTerminalState& terminal, std::string_view native,
    const std::filesystem::path& workspace, bool ambiguous) const
{
	return uam::opencode_terminal::Poll(terminal, native, workspace, ambiguous);
}

bool OpenCodeCliProviderRuntime::InteractiveObservationMatchesHost(const uam::CliTerminalState& terminal, const ExecutionHost& host) const
{
	return uam::opencode_terminal::MatchesHost(terminal, host);
}

void OpenCodeCliProviderRuntime::CheckpointInteractiveSubmission(uam::CliTerminalState& terminal) const
{
	uam::opencode_terminal::Checkpoint(terminal);
}

/// <summary>Count observed OpenCode prompts; its cache and reasoning counts are separate token buckets.</summary>
bool OpenCodeCliProviderRuntime::OnAcpHandleResult(uam::AppState&, uam::AcpSessionState& session, ChatSession&,
    const std::string& method, const std::string&, const nlohmann::json& result) const
{
	if (method != uam::acp_methods::kSessionPrompt) return false;
	uam::AcpTokenUsageState& usage = session.provider_usage.token_usage;
	if (usage.session_id != session.session_id) usage = {};
	if (session.session_id.empty() || session.session_id.size() > 512) return false;
	usage.last_available = false;
	const nlohmann::json* tokens = uam::nlohmann_json::FindObjectField(result, "usage");
	if (!tokens) return false;
	const std::function<std::optional<std::int64_t>(const char*, bool)> count = [&](const char* key, bool required) -> std::optional<std::int64_t>
	{
		const nlohmann::json* field = uam::nlohmann_json::FindField(*tokens, key);
		if (!field || field->is_null()) return required ? std::nullopt : std::optional<std::int64_t>{0};
		if (!field->is_number_integer()) return std::nullopt;
		if (field->is_number_unsigned() && field->get<std::uint64_t>() > static_cast<std::uint64_t>(uam::usage::kMaxExactTokenCount)) return std::nullopt;
		const std::int64_t parsed = field->get<std::int64_t>();
		return parsed >= 0 && parsed <= uam::usage::kMaxExactTokenCount ? std::optional<std::int64_t>{parsed} : std::nullopt;
	};
	const std::optional<std::int64_t> input = count("inputTokens", true), output = count("outputTokens", true);
	const std::optional<std::int64_t> cached = count("cachedReadTokens", false), written = count("cachedWriteTokens", false);
	const std::optional<std::int64_t> thought = count("thoughtTokens", false), reported = count("totalTokens", true);
	if (!input || !output || !cached || !written || !thought || !reported) return false;
	uam::AcpTokenUsageBreakdownState last;
	// Validate each addition before computing it, including counts outside signed-integer range.
	std::int64_t total = 0;
	for (const std::int64_t bucket : {*input, *cached, *written, *output, *thought})
	{
		if (bucket > uam::usage::kMaxExactTokenCount - total) return false;
		total += bucket;
	}
	if (*reported != total || total > uam::usage::kMaxExactTokenCount - usage.total.total_tokens) return false;
	last.input_tokens = *input + *cached + *written;
	last.cached_input_tokens = *cached;
	last.cache_write_input_tokens = *written;
	last.output_tokens = *output + *thought;
	last.reasoning_output_tokens = *thought;
	last.total_tokens = total;
	usage.session_id = session.session_id;
	usage.available = true;
	usage.last_available = true;
	usage.last = last;
	usage.total.input_tokens += last.input_tokens;
	usage.total.cached_input_tokens += last.cached_input_tokens;
	usage.total.cache_write_input_tokens += last.cache_write_input_tokens;
	usage.total.output_tokens += last.output_tokens;
	usage.total.reasoning_output_tokens += last.reasoning_output_tokens;
	usage.total.total_tokens += last.total_tokens;
	usage.updated_at_ms = uam::time::SystemEpochMillisecondsNow();
	return false;
}

bool OpenCodeCliProviderRuntime::AcpPermissionRequiresUserDecision(const uam::AcpPendingPermissionState& pending) const
{
	// Both the built-in question and a colliding MCP action require an explicit decision.
	return uam::strings::ToLowerAscii(uam::strings::Trim(pending.title)) == "doom_loop";
}

bool OpenCodeCliProviderRuntime::AcpPermissionIsNonActionQuestion(const uam::AcpPendingPermissionState& pending) const
{
	return uam::opencode_permissions::AcpDoomLoopQuestion(pending.provider_request_method, pending.title, pending.kind, pending.provider_input_json);
}

std::optional<bool> OpenCodeCliProviderRuntime::TryWriteAcpOutOfBandMessage(uam::AcpSessionState& session,
	const nlohmann::json& message, std::string* error) const
{
	return uam::OpenCodeNativeActivityService::TryWritePermissionResponse(session, message, error);
}

std::size_t OpenCodeCliProviderRuntime::AcpBackgroundTaskCount(const uam::AcpSessionState& session) const
{
	if (!session.running || session.last_process_id.empty() || session.session_id.empty()) return 0;
	std::unordered_set<std::string> children;
	for (const std::pair<const std::string, uam::AcpSessionState::OpenCodeChildActivity>& entry : session.opencode_child_activity_by_tool)
	{
		const uam::AcpSessionState::OpenCodeChildActivity& child = entry.second;
		if (child.process_id == session.last_process_id && child.parent_session_id == session.session_id &&
			child.native_session_id != session.session_id && child.native_session_id.starts_with("ses_") &&
			uam::execution_hosts::IsPortableId(child.native_session_id) && uam::acp_statuses::IsActiveStatus(child.status))
			children.insert(child.native_session_id);
	}
	return children.size();
}

nlohmann::json OpenCodeCliProviderRuntime::OnAcpBuildInitialize(uam::AcpSessionState& session, int request_id) const
{
	// Initialization is a new receipt incarnation even when an OS PID/runner ID is reused.
	session.opencode_child_activity_by_tool.clear();
	return IProviderRuntime::OnAcpBuildInitialize(session, request_id);
}

bool OpenCodeCliProviderRuntime::OnAcpHandleMessage(uam::AppState&, uam::AcpSessionState& session, ChatSession&,
	const nlohmann::json& message, const CefRefPtr<CefBrowser>&) const
{
	using uam::nlohmann_json::StringViewOrEmpty;
	if (!session.running || !session.session_ready || session.last_process_id.empty() || session.session_id.empty() ||
		StringViewOrEmpty(message, "method") != "session/update") return false;
	bool replaying = session.manual_compaction_pending || session.context_compaction_in_progress;
	for (const std::pair<const int, std::string>& request : session.pending_request_methods)
		if (request.second == "session/load" || request.second == "session/fork") replaying = true;
	const nlohmann::json* params = uam::nlohmann_json::FindObjectField(message, "params");
	if (params == nullptr || StringViewOrEmpty(*params, "sessionId") != session.session_id) return false;
	const nlohmann::json* update = uam::nlohmann_json::FindObjectField(*params, "update");
	if (update == nullptr) return false;
	const std::string_view kind = StringViewOrEmpty(*update, "sessionUpdate");
	if (kind != "tool_call" && kind != "tool_call_update") return false;
	const std::string_view tool_id = StringViewOrEmpty(*update, "toolCallId");
	const std::string_view status = StringViewOrEmpty(*update, "status");
	if (tool_id.empty() || (!uam::acp_statuses::IsActiveStatus(status) && status != "completed" &&
		status != "failed" && status != "cancelled" && status != "canceled" && status != "paused")) return false;
	const nlohmann::json* output = uam::nlohmann_json::FindObjectField(*update, "rawOutput");
	const nlohmann::json* metadata = output == nullptr ? nullptr : uam::nlohmann_json::FindObjectField(*output, "metadata");
	const std::string_view child_id = metadata == nullptr ? std::string_view{} : StringViewOrEmpty(*metadata, "sessionId");
	std::unordered_map<std::string, uam::AcpSessionState::OpenCodeChildActivity>::iterator found = session.opencode_child_activity_by_tool.find(std::string(tool_id));
	// Historical frames cannot establish current liveness or settlement, even for a known tool ID.
	if (replaying) return false;
	if (status == "completed" || status == "failed" || status == "cancelled" || status == "canceled")
	{
		if (found != session.opencode_child_activity_by_tool.end() && found->second.parent_session_id == session.session_id &&
			found->second.process_id == session.last_process_id)
			session.opencode_child_activity_by_tool.erase(found);
		return false;
	}
	if (child_id.starts_with("ses_") && child_id != session.session_id && uam::execution_hosts::IsPortableId(child_id))
	{
		session.opencode_child_activity_by_tool[std::string(tool_id)] = {std::string(child_id), std::string(status), session.session_id, session.last_process_id};
	}
	else if (found != session.opencode_child_activity_by_tool.end() && found->second.parent_session_id == session.session_id &&
		found->second.process_id == session.last_process_id)
	{
		found->second.status = status;
	}
	return false; // Preserve generic ACP tool projection and transcript handling.
}

namespace
{
	void ApplyOpenCodeChildIdentity(std::string& child_id, std::string& child_title,
	                              const nlohmann::json& metadata, std::string_view title)
	{
		const std::string_view parsed_id = uam::nlohmann_json::StringViewOrEmpty(metadata, "sessionId");
		if (!uam::execution_hosts::IsPortableId(parsed_id) || parsed_id.starts_with('-')) return;
		if (child_id != parsed_id) child_title.clear();
		child_id = parsed_id;
		if (!title.empty()) child_title = title;
	}

	/// Remote RPC/cleanup timeouts may extend the 30-second handshake deadline.
	std::string CreateOpenCodeNativeSession(const std::filesystem::path& workspace,
	                                       const std::vector<std::string>& argv,
	                                       const std::vector<std::pair<std::string, std::string>>& environment,
	                                       std::stop_token stop_token, std::string* error_out,
	                                       const ExecutionHost* remote_host);
}

ProviderAcpSettingChangeAction OpenCodeCliProviderRuntime::AcpModeChangeAction(const uam::AcpSessionState& session) const
{
	// The isolated config owns agent selection; permissions remain UAM-mediated.
	return session.active_uam_agent_execution_capability == "opencode-native-agent-config"
	           ? ProviderAcpSettingChangeAction::KeepCurrent
	           : ProviderAcpSettingChangeAction::SendRequest;
}

std::string OpenCodeCliProviderRuntime::CreateNativeSession(const ProviderProfile& profile, const std::filesystem::path& workspace, std::stop_token stop_token, std::string* error_out, const ExecutionHost* remote_host) const
{
	std::vector<std::string> argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "opencode");
	argv.push_back("acp");
	const std::vector<std::pair<std::string, std::string>> environment = remote_host != nullptr
	    ? std::vector<std::pair<std::string, std::string>>{}
	    : uam::provider_runtime_internal::ProviderChildEnvironmentOverrides(profile);
	return CreateOpenCodeNativeSession(workspace, argv, environment, stop_token, error_out, remote_host);
}

std::string OpenCodeCliProviderRuntime::InteractiveConfigurationError(const ProviderProfile& profile, const AppSettings& settings) const
{
	const std::string common_error = IProviderRuntime::InteractiveConfigurationError(profile, settings);
	if (!common_error.empty()) return common_error;
	const AppSettings provider_settings = uam::provider_runtime_internal::MergeProviderSettings(profile, settings);
	std::vector<std::string> flags = uam::command_line::SplitWords(provider_settings.provider_extra_flags);
	const std::vector<std::string> command = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "opencode");
	if (!command.empty()) flags.insert(flags.end(), command.begin() + 1, command.end());
	for (const std::string& flag : flags)
	{
		bool selects_session = flag == "--fork" || flag.starts_with("--fork=") ||
		                       flag == "--session" || flag.starts_with("--session=") ||
		                       flag == "--continue" || flag.starts_with("--continue=");
		if (flag.size() > 1 && flag[0] == '-' && flag[1] != '-')
		{
			// OpenCode's short model option consumes the remaining characters as its value.
			for (std::size_t index = 1; index < flag.size(); ++index)
			{
				if (flag[index] == 's' || flag[index] == 'c') selects_session = true;
				if (flag[index] != 'h' && flag[index] != 'v') break;
			}
		}
		if (selects_session)
		{
			return "OpenCode session selection is managed by UAM. Remove session, continue, and fork flags from the provider command and extra flags.";
		}
	}
	const std::string resume_argument = uam::strings::Trim(profile.resume_argument);
	if (!profile.supports_resume || (resume_argument != "--session" && resume_argument != "-s"))
	{
		return "OpenCode terminal settings must enable session resume using --session or -s.";
	}
	return {};
}

bool OpenCodeCliProviderRuntime::PrepareInteractiveSession(uam::AppState& app, ChatSession& chat, const ProviderProfile& profile, const std::string& resume_id, const ExecutionHost&, std::string* error_out) const
{
	if (error_out != nullptr) error_out->clear();
	if (!uam::execution_hosts::IsPortableId(resume_id))
	{
		if (error_out != nullptr) *error_out = "OpenCode needs a saved session before terminal launch. Retry opening the terminal.";
		return false;
	}
	// A failed save retains the exact ID for retry, but must never launch an untracked CLI.
	chat.native_session_id = resume_id;
	if (!SaveHistory(profile, app.data_root, chat))
	{
		if (error_out != nullptr) *error_out = "Could not save the OpenCode session. Retry when storage is available.";
		return false;
	}
	return true;
}

const ProviderCliPolicy* OpenCodeCliProviderRuntime::CliVersionPolicy() const
{
	static constexpr ProviderCliPolicy policy
	{
		.provider_id = uam::provider_ids::kOpenCodeCli,
		.npm_package = "opencode-ai",
		.fallback_title = "OpenCode",
		.executable_name = "opencode",
		.version_probe_command = "opencode --version",
		.homebrew_package = "opencode",
		.preferred_version = "latest",
		.fallback_version = "1.18.15",
		.minimum_version = "1.15.13",
		.version_policy = ProviderCliVersionPolicy::MinimumSemver,
		.verified_at = "2026-08-27",
	};
	return &policy;
}

const char* OpenCodeCliProviderRuntime::RuntimeId() const
{
	return uam::provider_ids::kOpenCodeCli;
}


std::vector<std::string> OpenCodeCliProviderRuntime::BuildInteractiveArgv(const ProviderProfile& profile, const ChatSession& chat, const AppSettings& settings) const
{
	if (!profile.supports_interactive)
	{
		return {};
	}

	const AppSettings provider_settings = uam::provider_runtime_internal::MergeProviderSettings(profile, settings);
	std::vector<std::string> argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "opencode");

	uam::provider_runtime_internal::AppendResumeArgs(argv, profile, chat.native_session_id);

	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", chat.model_id);

	uam::provider_runtime_internal::AppendArgs(argv, uam::provider_runtime_internal::BuildProviderFlagsArgv(provider_settings));
	return argv;
}

std::vector<std::string> OpenCodeCliProviderRuntime::BuildNativeDiscoveryArgv(const ProviderProfile& profile) const
{
	std::vector<std::string> argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "opencode");
	argv.insert(argv.end(), {"session", "list", "--format", "json", "--pure", "--max-count", "200"});
	return argv;
}

std::vector<std::string> OpenCodeCliProviderRuntime::BuildNativeExportArgv(const ProviderProfile& profile, const ChatSession& chat) const
{
	std::vector<std::string> argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "opencode");
	// Export ignores a session ID after "--" and opens an interactive picker.
	argv.insert(argv.end(), {"export", chat.native_session_id, "--pure"});
	return argv;
}

MessageRole OpenCodeCliProviderRuntime::RoleFromNativeType(const ProviderProfile& profile, std::string_view native_type) const
{
	return uam::provider_runtime_internal::RoleFromNativeType(profile, native_type);
}

void OpenCodeCliProviderRuntime::ApplyNativeToolMetadata(ToolCall& tool, const nlohmann::json& native_item) const
{
	if (tool.name != "task") return;
	tool.is_sub_agent = true;
	const nlohmann::json* state = uam::nlohmann_json::FindObjectField(native_item, "state");
	if (state == nullptr) return;
	const nlohmann::json* metadata = uam::nlohmann_json::FindObjectField(*state, "metadata");
	if (metadata == nullptr) return;
	ApplyOpenCodeChildIdentity(tool.sub_agent_id, tool.sub_agent_title, *metadata,
	    uam::nlohmann_json::StringViewOrEmpty(*state, "title"));
}

void OpenCodeCliProviderRuntime::ApplyAcpToolMetadata(uam::AcpToolCallState& tool, const nlohmann::json& update) const
{
	// OpenCode maps its task tool to ACP's think kind even when the title is descriptive.
	if (tool.kind != "think" && !tool.is_sub_agent) return;
	tool.is_sub_agent = true;
	const nlohmann::json* output = uam::nlohmann_json::FindObjectField(update, "rawOutput");
	if (output == nullptr) return;
	const nlohmann::json* metadata = uam::nlohmann_json::FindObjectField(*output, "metadata");
	if (metadata == nullptr) return;
	ApplyOpenCodeChildIdentity(tool.sub_agent_id, tool.sub_agent_title, *metadata, tool.title);
}

std::vector<ChatSession> OpenCodeCliProviderRuntime::LoadHistory(const ProviderProfile&, const std::filesystem::path& data_root, const std::filesystem::path&, const ProviderRuntimeHistoryLoadOptions&) const
{
	return uam::provider_runtime_internal::LoadLocalChats(data_root);
}

bool OpenCodeCliProviderRuntime::SaveHistory(const ProviderProfile&, const std::filesystem::path& data_root, const ChatSession& chat) const
{
	return uam::provider_runtime_internal::SaveLocalChat(data_root, chat);
}


std::vector<std::string> OpenCodeCliProviderRuntime::BuildWorkerArgv(const ProviderProfile& profile, const AppSettings& settings, std::string_view prompt, std::string_view model_id) const
{
	std::vector<std::string> argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "opencode");
	argv.push_back("run");
	// Workers must not inherit a default subagent or generate a second model-backed title.
	uam::provider_runtime_internal::AppendArgs(argv, {"--agent", "uam-text-worker", "--title", "UAM text worker"});
	const std::vector<std::string> flags = uam::provider_runtime_internal::ProviderWorkerFlags(profile, settings);
	uam::provider_runtime_internal::AppendArgs(argv, flags);

	uam::provider_runtime_internal::AppendTrimmedOptionValue(argv, "--model", model_id);

	argv.push_back("--");
	argv.push_back(std::string(prompt));
	return argv;
}

std::vector<std::string> OpenCodeCliProviderRuntime::BuildStructuredLaunchArgv(const ProviderProfile& profile, const ChatSession&) const
{
	std::vector<std::string> argv = uam::provider_runtime_internal::SplitInteractiveCommandOrDefault(profile, "opencode");
	argv.push_back("acp");
	return argv;
}

std::vector<std::pair<std::string, std::string>> OpenCodeCliProviderRuntime::BuildStructuredLaunchEnvironment(const ProviderProfile&, const ChatSession& chat) const
{
	// OpenCode ACP does not reliably forward child-session permission requests, so
	// disable Task until the provider can mediate those requests without hanging.
	// UAM's own native target grant is the approval boundary for these exact tools.
	if (!uam::computer_use::AvailableForChat(chat))
		return {{"OPENCODE_PERMISSION", R"({"*":"ask","task":"deny","uam-computer_computer_observe":"deny","uam-computer_computer_action":"deny"})"}};
	return {{"OPENCODE_PERMISSION", R"({"*":"ask","task":"deny","uam-computer_computer_observe":"allow","uam-computer_computer_action":"allow"})"}};
}

bool OpenCodeCliProviderRuntime::OnAcpHandleError(uam::AppState& app, uam::AcpSessionState& session,
	ChatSession& chat, const uam::acp_detail::AcpResponseFailureDetails& details) const
{
	if (details.failure.has_code && details.failure.code != -32002) return false;
	return uam::acp_detail::RetrySessionNewAfterInvalidLoad(app, session, chat, details);
}

std::string OpenCodeCliProviderRuntime::OnAcpMapApprovalModeId(const std::string& mode_id) const
{
	return mode_id == "default" ? "build" : mode_id;
}

const IProviderRuntime& GetOpenCodeCliProviderRuntime()
{
	static const OpenCodeCliProviderRuntime runtime;
	return runtime;
}

namespace
{
	std::string CreateOpenCodeNativeSession(const std::filesystem::path& workspace,
	                                const std::vector<std::string>& argv,
	                                const std::vector<std::pair<std::string, std::string>>& environment,
	                                std::stop_token stop_token, std::string* error_out, const ExecutionHost* remote_host)
	{
		if (error_out != nullptr) error_out->clear();
		std::string confirmed_session_id;
		const auto fail = [error_out, &confirmed_session_id](const std::string& error) -> std::string
		{
			// Teardown failures must not discard an already-created session.
			if (!confirmed_session_id.empty()) return confirmed_session_id;
			if (error_out != nullptr) *error_out = error;
			return {};
		};
		if (stop_token.stop_requested()) return fail("OpenCode session creation was canceled.");
		if (argv.empty()) return fail("OpenCode session creation requires a provider command.");
		if (remote_host != nullptr)
		{
			if (remote_host->id == uam::execution_hosts::kLocalHostId || remote_host->transport != "ssh" || remote_host->runner_status != "ready" ||
			    remote_host->runner_protocol_version < 3 || !uam::execution_hosts::IsSafeSshAlias(remote_host->ssh_alias) ||
			    !uam::execution_hosts::IsAbsoluteRemotePath(remote_host->platform, uam::paths::Utf8PathString(workspace)))
				return fail("The remote OpenCode workspace or runner is not ready for session creation.");
		}
		else if (!workspace.is_absolute()) return fail("OpenCode session creation requires an absolute workspace.");
		IPlatformProcessService& service = PlatformServicesFactory::Instance().process_service;
		struct ScopedProcess
		{
			IPlatformProcessService& service;
			uam::platform::StdioProcessPlatformFields process;
			std::unique_ptr<uam::remote::RunnerClient> remote;
			std::string remote_process_id;
			~ScopedProcess()
			{
				if (remote && !remote_process_id.empty())
				{
					(void)remote->StopProcess(remote_process_id);
					(void)remote->RemoveProcess(remote_process_id);
				}
				else service.StopStdioProcess(process, true);
			}
		} child{service, {}, {}, {}};
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		std::string error;
		if (remote_host != nullptr)
		{
			child.remote = std::make_unique<uam::remote::RunnerClient>(service,
			    uam::remote::SshBridgeArgv(remote_host->ssh_alias, remote_host->platform, remote_host->runner_version,
			                               remote_host->runner_directory, remote_host->runner_protocol_version),
			    remote_host->runner_version, remote_host->runner_protocol_version);
			const std::string operation_id = service.GenerateUuid();
			if (operation_id.empty()) return fail("Could not allocate a remote OpenCode creation request.");
			child.remote_process_id = "opencode-create-" + operation_id;
			// Default authority uses the runner's transient lease if the connection disappears.
			if (!child.remote->StartProcess(child.remote_process_id, workspace, argv, environment, &error))
				return fail("Could not start remote OpenCode session creation: " + error);
		}
		else if (!service.StartStdioProcess(child.process, workspace, argv, &error, environment))
			return fail("Could not start OpenCode session creation: " + error);
		const auto send = [&](int id, const char* method, nlohmann::json params)
		{
			if (stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline)
			{
				error = stop_token.stop_requested() ? "OpenCode session creation was canceled." : "OpenCode session creation timed out.";
				return false;
			}
			const std::string line = nlohmann::json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}}.dump() + "\n";
			return child.remote ? child.remote->WriteProcess(child.remote_process_id, line, &error)
			                    : service.WriteToStdioProcess(child.process, line.data(), line.size(), &error);
		};
		if (!send(1, "initialize", {{"protocolVersion", 1}, {"clientCapabilities", nlohmann::json::object()}, {"clientInfo", {{"name", "universal-agent-manager"}, {"version", "1"}}}}))
			return fail("Could not initialize OpenCode: " + error);
		int expected_id = 1;
		std::string pending, diagnostic, session_id;
		std::size_t total_bytes = 0;
		bool stdout_closed = false, stderr_closed = false;
		auto close_deadline = deadline;
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (stop_token.stop_requested()) return fail("OpenCode session creation was canceled.");
			bool received = false;
			uam::remote::ProcessPollResult remote_poll;
			if (child.remote)
			{
				const auto interrupted = [&] { return stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline; };
				if (!child.remote->PollProcess(child.remote_process_id, remote_poll, &error, interrupted))
					return fail(stop_token.stop_requested() ? "OpenCode session creation was canceled." : "Could not read remote OpenCode session response: " + error);
				total_bytes += remote_poll.standard_output.size() + remote_poll.standard_error.size();
				if (total_bytes > uam::platform::kCapturedCommandMaxOutputBytes) return fail("OpenCode session response exceeded the output limit.");
				pending += remote_poll.standard_output;
				diagnostic += remote_poll.standard_error;
				if (diagnostic.size() > 4096) diagnostic.erase(0, diagnostic.size() - 4096);
				received = !remote_poll.standard_output.empty() || !remote_poll.standard_error.empty();
			}
			else for (bool stderr_stream : {false, true})
			{
				bool& closed = stderr_stream ? stderr_closed : stdout_closed;
				if (closed) continue;
				char buffer[8192];
				const std::ptrdiff_t count = stderr_stream
				    ? service.ReadStdioProcessStderr(child.process, buffer, sizeof(buffer), &error)
				    : service.ReadStdioProcessStdout(child.process, buffer, sizeof(buffer), &error);
				if (count == 0) closed = true;
				else if (count == -1) return fail("Could not read OpenCode session response: " + error);
				else if (count > 0)
				{
					received = true;
					total_bytes += static_cast<std::size_t>(count);
					if (total_bytes > uam::platform::kCapturedCommandMaxOutputBytes) return fail("OpenCode session response exceeded the output limit.");
					if (stderr_stream)
					{
						diagnostic.append(buffer, static_cast<std::size_t>(count));
						if (diagnostic.size() > 4096) diagnostic.erase(0, diagnostic.size() - 4096);
					}
					else pending.append(buffer, static_cast<std::size_t>(count));
				}
			}
			std::size_t newline;
			while ((newline = pending.find('\n')) != std::string::npos)
			{
				if (newline > 1024 * 1024) return fail("OpenCode session response line exceeded the limit.");
				const std::string line = pending.substr(0, newline);
				pending.erase(0, newline + 1);
				if (line.empty() || line == "\r") continue;
				const nlohmann::json response = nlohmann::json::parse(line, nullptr, false);
				if (!response.is_object()) return fail("OpenCode returned malformed session JSON.");
				if (!session_id.empty()) continue;
				if (response.contains("method"))
				{
					if (response.contains("id")) return fail("OpenCode session creation requested an unsupported client operation.");
					continue;
				}
				const nlohmann::json id = response.value("id", nlohmann::json());
				if (!id.is_number_integer() || id != expected_id) continue;
				if (response.contains("error"))
				{
					const nlohmann::json& detail = response["error"];
					const std::string message = detail.is_object() && detail.contains("message") && detail["message"].is_string() ? detail["message"].get<std::string>().substr(0, 1024) : "Provider rejected the request.";
					return fail("OpenCode session creation failed: " + message);
				}
				if (!response.contains("result") || !response["result"].is_object()) return fail("OpenCode session response omitted its result.");
				if (expected_id == 1)
				{
					const nlohmann::json version = response["result"].value("protocolVersion", nlohmann::json());
					if (!version.is_number_integer() || version != 1) return fail("OpenCode returned an unsupported ACP protocol version.");
					expected_id = 2;
					if (!send(2, "session/new", {{"cwd", uam::paths::Utf8PathString(workspace)}, {"mcpServers", nlohmann::json::array()}}))
						return fail("Could not request an OpenCode session: " + error);
				}
				else
				{
					const nlohmann::json& result = response["result"];
					if (!result.contains("sessionId") || !result["sessionId"].is_string()) return fail("OpenCode session response omitted its session ID.");
					session_id = result["sessionId"].get<std::string>();
					if (session_id.size() <= 4 || !session_id.starts_with("ses_") || !uam::execution_hosts::IsPortableId(session_id)) return fail("OpenCode returned an invalid native session ID.");
					confirmed_session_id = session_id;
					if (child.remote)
					{
						// The ID is already known: a lost teardown reply must not create a second session on retry.
						(void)child.remote->CloseProcessInput(child.remote_process_id);
						return session_id;
					}
					service.CloseStdioProcessInput(child.process);
					close_deadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(2));
				}
			}
			if (pending.size() > 1024 * 1024) return fail("OpenCode session response line exceeded the limit.");
			if (child.remote)
			{
				if (!child.remote->AcknowledgeProcessOutput(child.remote_process_id, remote_poll, &error))
					return fail("Could not acknowledge remote OpenCode output: " + error);
				if (!remote_poll.running) return fail("OpenCode exited before session creation completed. " + diagnostic);
				if (!received) std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}
			int exit_code = -1;
			if (!received && service.PollStdioProcessExited(child.process, &exit_code))
			{
				// A confirmed session survives a teardown failure; retrying would create another.
				if (!session_id.empty()) return session_id;
				return fail("OpenCode exited before session creation completed (exit " + std::to_string(exit_code) + "). " + diagnostic);
			}
			if (stdout_closed && session_id.empty()) return fail("OpenCode closed output before session creation completed. " + diagnostic);
			if (!session_id.empty() && std::chrono::steady_clock::now() >= close_deadline) return session_id;
			if (!received) std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return fail("OpenCode session creation timed out after 30 seconds.");
	}
}

std::string OpenCodeCliProviderRuntime::LocalCliCompatibilityError(const uam::AppState& app) const
{
	const auto state_it = app.runtime_cli_versions_by_provider_id.find(uam::provider_ids::kOpenCodeCli);
	if (state_it == app.runtime_cli_versions_by_provider_id.end())
	{
		return "";
	}

	const uam::CliProviderVersionState& state = state_it->second;
	if (!state.checked)
	{
		return "Checking OpenCode compatibility. Try again in a moment.";
	}
	if (state.supported)
	{
		return "";
	}
	if (!state.installed_version.empty())
	{
		return "OpenCode 1.15.13 or newer is required for reliable ACP permission mediation (installed " + state.installed_version + "). Update it in Settings.";
	}
	return uam::strings::NonEmptyOrFallback(state.message, "OpenCode is not installed or its version could not be determined.") + " Open Settings to check or update it.";
}

bool OpenCodeCliProviderRuntime::SupportsSavedImageRetry(const uam::AcpSessionState& session) const
{
	// Cold permission capture precedes native discovery; it grants no image reads or dispatch.
	return session.protocol_kind == "opencode-acp";
}

SavedImageRetryPreflight OpenCodeCliProviderRuntime::PrepareSavedImageRetry(uam::AcpSessionState& session, const ChatSession& settings,
    const std::vector<MessageAttachment>& images, std::string& error) const
{
	const auto gate = uam::OpenCodeNativeActivityService::PrepareSavedImageRetry(session, settings, images, error);
	if (gate == uam::OpenCodeNativeGateState::Ready) return SavedImageRetryPreflight::Ready;
	if (gate == uam::OpenCodeNativeGateState::Pending) return SavedImageRetryPreflight::Pending;
	return SavedImageRetryPreflight::Rejected;
}

nlohmann::json OpenCodeCliProviderRuntime::SavedImageRetryMetadata(const uam::AcpSessionState& session, const ChatSession& settings) const
{
	return uam::OpenCodeNativeActivityService::SavedImageRetryMetadata(session, settings);
}

bool OpenCodeCliProviderRuntime::EncodePreparedSavedImageRetry(nlohmann::json& request, const nlohmann::json& prepared,
    std::string& error, const std::vector<MessageAttachment>& images, const nlohmann::json& metadata) const
try
{
	const auto text = [](const nlohmann::json& object, const char* key) -> std::string
	{
		const auto found = object.find(key);
		return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
	};
	const auto fail = [&error]()
	{ error = "OpenCode could not retain complete supported image input. The prompt was not sent."; return false; };
	if (images.empty() || images.size() > 32 || !request.is_object() || text(request, "jsonrpc") != "2.0" ||
	    text(request, "method") != "session/prompt" || !request.contains("id") || !request["id"].is_number_integer() || request["id"].get<std::int64_t>() <= 0 ||
	    text(metadata, "phase") != "ready" || text(metadata, "lifetime").empty() || !metadata.contains("generation") || !metadata["generation"].is_number_unsigned() || metadata["generation"].get<std::uint64_t>() == 0 ||
	    !metadata.contains("facts") || !metadata["facts"].is_object() || metadata["facts"].value("image", false) != true ||
	    !metadata.contains("names") || !metadata["names"].is_array() || metadata["names"].size() != images.size() ||
	    !request.contains("params") || !request["params"].is_object() || !prepared.is_object() || !prepared.contains("message") || !prepared["message"].is_object() ||
	    !prepared["message"].contains("content") || !prepared["message"]["content"].is_array()) return fail();
	auto& params = request["params"];
	const auto& content = prepared["message"]["content"];
	if (text(params, "sessionId").empty() || text(params, "sessionId") != text(metadata, "sessionId") ||
	    !params.contains("prompt") || !params["prompt"].is_array() || params["prompt"].size() != 1 ||
	    !params["prompt"][0].is_object() || text(params["prompt"][0], "type") != "text" || !params["prompt"][0].contains("text") || !params["prompt"][0]["text"].is_string() ||
	    content.size() != images.size() + 1 || !content[0].is_object() || text(content[0], "type") != "text" || !content[0].contains("text") || !content[0]["text"].is_string()) return fail();
	std::string labels;
	std::size_t total_bytes = 0;
	for (std::size_t index = 0; index < images.size(); ++index)
	{
		if (!metadata["names"][index].is_string() || metadata["names"][index] != images[index].name || images[index].kind != "image" || images[index].name.size() > 4096) return fail();
		labels += "\nImage " + std::to_string(index + 1) + " (native image-block order): " + nlohmann::json(images[index].name).dump();
		const auto& part = content[index + 1];
		if (!part.is_object() || part.size() != 2 || text(part, "type") != "image" || !part.contains("source") || !part["source"].is_object()) return fail();
		const auto& source = part["source"];
		const auto data = text(source, "data"), mime = text(source, "media_type");
		std::string bytes;
		if (source.size() != 3 || text(source, "type") != "base64" || data.empty() || data.size() > 4 * 1024 * 1024 ||
		    !uam::base64::Decode(data, bytes) || bytes.empty() || uam::base64::Encode(bytes) != data ||
		    bytes.size() > 4 * 1024 * 1024 - total_bytes || uam::attachment_preview::ImageMime(bytes) != mime ||
		    (mime != "image/png" && mime != "image/jpeg" && mime != "image/gif" && mime != "image/webp")) return fail();
		total_bytes += bytes.size();
	}
	const auto& prepared_text = content[0]["text"].get_ref<const std::string&>();
	if (!prepared_text.ends_with(labels)) return fail();
	const auto body = prepared_text.substr(0, prepared_text.size() - labels.size());
	const auto& original_text = params["prompt"][0]["text"].get_ref<const std::string&>();
	if (body.empty() || !original_text.ends_with(body)) return fail();
	// Keep context added by the normal ACP builder, then the worker's exact captured-name labels.
	auto encoded = request;
	encoded["params"]["prompt"][0]["text"] = original_text + labels;
	uam::structured_attachments::AppendPreparedAcpImages(encoded, prepared);
	if (encoded["params"]["prompt"].size() != images.size() + 1 || encoded.dump().size() + 1 > 4 * 1024 * 1024) return fail();
	request = std::move(encoded);
	return true;
}
catch (...)
{
	error = "OpenCode image input or its native evidence is invalid. The prompt was not sent.";
	return false;
}
