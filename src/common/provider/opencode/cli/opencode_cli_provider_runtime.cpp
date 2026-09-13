#include "common/provider/opencode/cli/opencode_cli_provider_runtime.h"
#include "common/runtime/acp/acp_session_internal.h"

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
#include <thread>
#include "remote/runner_client.h"
#include "common/provider/runtime/provider_runtime_internal.h"

bool OpenCodeCliProviderRuntime::AcpPermissionRequiresUserDecision(const uam::AcpPendingPermissionState& pending) const
{
	// OpenCode asks whether to continue identical repeated calls; this is not action authorization.
	return uam::strings::ToLowerAscii(uam::strings::Trim(pending.title)) == "doom_loop";
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

std::vector<std::pair<std::string, std::string>> OpenCodeCliProviderRuntime::BuildStructuredLaunchEnvironment(const ProviderProfile&, const ChatSession&) const
{
	// OpenCode ACP does not reliably forward child-session permission requests, so
	// disable Task until the provider can mediate those requests without hanging.
	// UAM's own native target grant is the approval boundary for these exact tools.
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
