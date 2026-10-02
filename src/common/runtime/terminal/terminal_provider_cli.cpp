#include "common/runtime/terminal/terminal_provider_cli.h"

#include "app/chat_domain_service.h"
#include "app/provider_resolution_service.h"
#include "common/chat/chat_repository.h"
#include "common/platform/platform_services.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/provider_cli_compatibility_service.h"
#include "common/runtime/terminal/terminal_chat_sync.h"
#include "common/runtime/terminal/terminal_debug_diagnostics.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/utils/string_utils.h"

#include "common/utils/io_utils.h"
#include "common/utils/env_utils.h"
#include "common/utils/hash_utils.h"
#include "common/paths/workspace_root.h"
#include "common/paths/path_utils.h"
#include "common/config/execution_host_config.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_native_context.h"
#include "remote/runner_client.h"
#include "remote/runner_proxy.h"
#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace uam
{

std::string CliTerminalIdForChat(std::string_view chat_id)
{
	std::string terminal_id;
	terminal_id.reserve(kCliTerminalIdPrefix.size() + chat_id.size());
	terminal_id.append(kCliTerminalIdPrefix);
	terminal_id.append(chat_id);
	return terminal_id;
}

bool ProviderSupportsInteractiveTerminal(const ProviderProfile& provider)
{
	return ProviderRuntime::IsRuntimeEnabled(provider) &&
	       ProviderRuntime::UsesCliOutput(provider) &&
	       provider.supports_interactive;
}

std::string ProviderInteractiveTerminalUnavailableReason(const ProviderProfile& provider)
{
	if (!ProviderRuntime::IsRuntimeEnabled(provider))
	{
		return uam::strings::NonEmptyOrFallback(ProviderRuntime::DisabledReason(provider), "Selected provider runtime is disabled in this build.");
	}

	if (!ProviderRuntime::UsesCliOutput(provider))
	{
		return "CLI output is unavailable for the selected provider.";
	}

	if (!provider.supports_interactive)
	{
		return "Provider does not expose an interactive CLI runtime.";
	}

	return "";
}

std::string ResolveProviderInteractiveResumeId(const AppState& app, const ChatSession& chat, const ProviderProfile& provider)
{
	if (const AcpSessionState* acp_session = FindAcpSessionForChat(app, chat.id); acp_session != nullptr && acp_session->running && !uam::strings::Trim(acp_session->session_id).empty())
	{
		return uam::strings::Trim(acp_session->session_id);
	}

	return ProviderRuntimeRegistry::Resolve(provider).ResolveInteractiveResumeId(app, chat);
}

/// <summary>False with an empty error means a remote stop is pending; retry after confirmation.</summary>
bool PrepareAcpSessionForCliTerminalLaunch(AppState& app, ChatSession& chat, std::string* error_out)
{
	if (error_out != nullptr)
	{
		error_out->clear();
	}

	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	if (const std::string update_error = ProviderCliLaunchBlockReason(app, provider.id, chat.execution_host_id); !update_error.empty())
	{
		if (error_out != nullptr) *error_out = update_error;
		return false;
	}
	AcpSessionState* session = FindAcpSessionForChat(app, chat.id);
	if (session != nullptr && AcpSessionHasBlockingRuntimeWork(*session))
	{
		if (error_out != nullptr)
		{
			*error_out = "Cannot start terminal fallback while the structured runtime is busy.";
		}
		return false;
	}

	if (!StopAcpSession(app, chat.id))
	{
		if ((session != nullptr && session->remote_stop_pending) ||
		    (session == nullptr && AcpStopInProgress(app, chat.id)))
		{
			return false;
		}
		if (error_out != nullptr)
		{
			*error_out = session != nullptr && session->remote_stop_unconfirmed
			    ? "The remote stop could not be confirmed. Reconnect structured chat before retrying the terminal."
			    : "Failed to stop the idle structured runtime before starting terminal fallback.";
		}
		return false;
	}
	return true;
}

std::vector<std::string> BuildProviderInteractiveArgv(const AppState& app, const ChatSession& chat)
{
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	ChatSession effective_chat = chat;
	effective_chat.native_session_id = ResolveProviderInteractiveResumeId(app, chat, provider);

	return ProviderRuntime::BuildInteractiveArgv(provider, effective_chat, app.settings);
}

std::string BuildProviderHandoffContext(const ChatSession& chat)
{
	std::string context;
	for (const Message& message : chat.messages)
	{
		for (const MessageBlock& block : message.blocks)
		{
			if (block.type == "context_compaction" && !uam::strings::IsBlank(block.text))
				context += "Conversation summary: " + block.text + "\n\n";
		}
		if ((message.role == MessageRole::User || message.role == MessageRole::Assistant) && !uam::strings::IsBlank(message.content))
			context += (message.role == MessageRole::User ? "User: " : "Assistant: ") + message.content + "\n\n";
	}
	return context;
}

/// <summary>Recovers legitimate saved history when a Codex chat has no native session to resume.</summary>
static bool PrepareUnboundCodexHistory(AppState& app, ChatSession& chat, const std::vector<std::string>& argv, std::string& error)
{
	if (chat.provider_id != uam::provider_ids::kCodexCli || !chat.provider_handoff_context.empty() || !chat.native_session_id.empty() ||
	    std::find(argv.begin(), argv.end(), "resume") != argv.end()) return true;
	if (!ChatRepository::HydrateChatMessages(app.data_root, chat, &error))
	{
		error = "Could not load the complete Codex conversation before opening its terminal. " + error;
		return false;
	}
	chat.provider_handoff_context = BuildProviderHandoffContext(chat);
	return true;
}

static std::string ContextConnectionIdentity(const ExecutionHost& host)
{
	return uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(nlohmann::json::array({host.id, host.transport, host.ssh_alias, host.platform, host.architecture, host.runner_directory}).dump()));
}

static std::filesystem::path CliProviderContextDirectory(const AppState& app, const ChatSession& chat, const ExecutionHost& host)
{
	const std::string token = uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(uam::paths::Utf8PathString(std::filesystem::absolute(app.data_root)) + ":" + chat.id));
	if (host.id == uam::execution_hosts::kLocalHostId) return std::filesystem::absolute(app.data_root / "runtime-context" / token);
	return uam::paths::PathFromUtf8(uam::execution_hosts::JoinRemotePath(host.platform,
	    uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, chat)), ".UAM/context/" + token));
}

bool RemoveCliProviderHandoff(AppState& app, const ChatSession& chat, std::string& error, std::stop_token stop_token)
{
	if (chat.provider_handoff_cli_contexts.empty()) return true;
	if (AcpStopInProgress(app, chat.id)) return false;
	if (const CliTerminalState* terminal = FindCliTerminalForChat(app, chat.id); terminal != nullptr && terminal->running) return false;
	const std::string token = uam::hashing::Hex64Padded(uam::hashing::Fnv1a64(uam::paths::Utf8PathString(std::filesystem::absolute(app.data_root)) + ":" + chat.id));
	for (const ProviderHandoffCliContext& location : chat.provider_handoff_cli_contexts)
	{
		if (stop_token.stop_requested()) return false;
		const ExecutionHost* host = uam::execution_hosts::Find(app.settings.execution_hosts, location.execution_host_id);
		if (host == nullptr || location.connection_identity != ContextConnectionIdentity(*host))
		{
			error = "Provider context cleanup is waiting for its original execution host configuration.";
			return false;
		}
		const std::filesystem::path directory = uam::paths::PathFromUtf8(location.directory);
		if (uam::paths::Utf8PathString(directory.filename()) != token) return false;
		if (host->id == uam::execution_hosts::kLocalHostId)
		{
			if (directory != std::filesystem::absolute(app.data_root / "runtime-context" / token)) return false;
			if (!uam::provider_native_context::RemoveOwnedContext(directory, false, error)) return false;
			continue;
		}
		uam::remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
		    uam::remote::SshBridgeArgv(host->ssh_alias, host->platform, host->runner_version, host->runner_directory, host->runner_protocol_version),
		    host->runner_version, host->runner_protocol_version);
		if (!client.Connect(&error, stop_token) || !client.SupportsProviderNativeContext()) return false;
		if (!client.RemoveProviderContext(directory, &error, stop_token)) return false;
	}
	return true;
}

bool StageCliProviderContextCleanup(AppState& app, const ChatSession& chat)
{
	if (chat.provider_handoff_cli_contexts.empty()) return true;
	if (AcpStopInProgress(app, chat.id)) return false;
	if (const CliTerminalState* terminal = FindCliTerminalForChat(app, chat.id); terminal != nullptr && terminal->running) return false;
	ChatSession intent;
	intent.id = chat.id;
	intent.provider_id = chat.provider_id;
	intent.provider_handoff_cli_contexts = chat.provider_handoff_cli_contexts;
	return ChatRepository::SaveChat(app.data_root / "runtime-context-cleanup", intent);
}

void RetryCliProviderContextCleanup(AppState& app, double now_s)
{
	if (app.provider_context_cleanup_worker != nullptr)
	{
		if (app.provider_context_cleanup_finished == nullptr || !app.provider_context_cleanup_finished->load()) return;
		app.provider_context_cleanup_worker.reset();
		app.provider_context_cleanup_finished.reset();
	}
	if (now_s < app.provider_context_cleanup_not_before_s) return;
	app.provider_context_cleanup_not_before_s = now_s + 30.0;
	std::error_code existence_error;
	if (!std::filesystem::exists(app.data_root / "runtime-context-cleanup" / "chats", existence_error) || existence_error) return;
	app.provider_context_cleanup_finished = std::make_shared<std::atomic<bool>>(false);
	const std::shared_ptr<std::atomic<bool>> finished = app.provider_context_cleanup_finished;
	app.provider_context_cleanup_worker = std::make_unique<std::jthread>(
	    [data_root = app.data_root, settings = app.settings, finished](std::stop_token stop)
	    {
		    AppState cleanup;
		    cleanup.data_root = data_root;
		    cleanup.settings = settings;
		    const std::filesystem::path pending_root = data_root / "runtime-context-cleanup";
		    try
		    {
		    for (const ChatSession& intent : ChatRepository::LoadLocalChats(pending_root))
		    {
			    if (stop.stop_requested()) break;
			    std::string error;
			    if (RemoveCliProviderHandoff(cleanup, intent, error, stop))
			    {
				    if (ChatRepository::DeleteChatStorageFiles(pending_root, intent.id).Failed())
					    LogCliDiagnosticEvent(cleanup, "provider_context_cleanup", "intent_delete_failed", nullptr, "chat_id=" + intent.id);
			    }
			    else LogCliDiagnosticEvent(cleanup, "provider_context_cleanup", "pending", nullptr, "chat_id=" + intent.id + ", " + error);
		    }
		    }
		    catch (...)
		    {
			    LogCliDiagnosticEvent(cleanup, "provider_context_cleanup", "pending", nullptr, "Context cleanup failed; its durable intent is retained.");
		    }
		    finished->store(true);
	    });
}

bool PrepareCliProviderHandoff(AppState& app, ChatSession& chat, const ExecutionHost& host,
    std::vector<std::string>& argv, std::vector<std::pair<std::string, std::string>>& environment,
    std::string& launch_channel, std::string& error, std::stop_token stop_token)
{
	launch_channel.clear();
	if (!PrepareUnboundCodexHistory(app, chat, argv, error)) return false;
	if (stop_token.stop_requested()) { error = "Provider context preparation was cancelled."; return false; }
	if (chat.provider_handoff_context.empty() ||
	    (chat.provider_id == uam::provider_ids::kCodexCli && !chat.native_session_id.empty() && chat.provider_handoff_session_id == chat.native_session_id)) return true;
	const bool remote = host.id != uam::execution_hosts::kLocalHostId;
	const std::filesystem::path workspace = uam::paths::ResolveWorkspaceRootPath(app, chat);
	const std::string directory = uam::paths::Utf8PathString(CliProviderContextDirectory(app, chat, host));
	const std::string context_path = uam::execution_hosts::JoinRemotePath(host.platform, directory, "conversation.md");
	const std::string snapshot = "Use the saved conversation below as context. Do not repeat its tool actions.\n\n" + chat.provider_handoff_context;
	std::unique_ptr<uam::remote::RunnerClient> client;
	if (remote)
	{
		client = std::make_unique<uam::remote::RunnerClient>(PlatformServicesFactory::Instance().process_service,
		    uam::remote::SshBridgeArgv(host.ssh_alias, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version),
		    host.runner_version, host.runner_protocol_version);
		if (!client->Connect(&error, stop_token)) return false;
		if (!client->SupportsProviderNativeContext())
		{
			error = "Update the remote runner before carrying provider context into CLI View.";
			return false;
		}
	}
	const auto stage = [&](const std::string& path, const std::string& content)
	{
		if (stop_token.stop_requested()) { error = "Provider context preparation was cancelled."; return false; }
		if (client != nullptr) return client->UploadFile("context-" + PlatformServicesFactory::Instance().process_service.GenerateUuid(), uam::paths::PathFromUtf8(path), content, &error, stop_token);
		if (uam::io::WriteTextFile(uam::paths::PathFromUtf8(path), content)) return true;
		error = "Could not save provider conversation context. Retry when storage is available.";
		return false;
	};
	const ProviderHandoffCliContext location{host.id, directory, ContextConnectionIdentity(host)};
	if (!uam::ranges::Contains(chat.provider_handoff_cli_contexts, location))
	{
		chat.provider_handoff_cli_contexts.push_back(location);
		if (!ChatRepository::SaveChat(app.data_root, chat))
		{
			chat.provider_handoff_cli_contexts.pop_back();
			error = "Could not save provider context cleanup information.";
			return false;
		}
	}
	if (client != nullptr)
	{
		if (!client->PrepareProviderContext(uam::paths::PathFromUtf8(directory), &error, stop_token)) return false;
	}
	else if (!uam::provider_native_context::PrepareOwnedContext(uam::paths::PathFromUtf8(directory), error)) return false;
	if (!stage(context_path, snapshot)) return false;
	if (chat.provider_id == uam::provider_ids::kCodexCli)
	{
		// The executing host imports the snapshot into owned native thread history.
	}
	else if (chat.provider_id == uam::provider_ids::kClaudeCli)
	{
		argv.insert(argv.end(), {"--append-system-prompt-file", context_path});
	}
	else if (chat.provider_id == uam::provider_ids::kCopilotCli)
	{
		const std::string instructions_path = uam::execution_hosts::JoinRemotePath(host.platform, directory, ".github/instructions/handoff.instructions.md");
		if (!stage(instructions_path, "---\napplyTo: \"**\"\n---\n" + snapshot)) return false;
	}
	else if (chat.provider_id == uam::provider_ids::kGeminiCli)
	{
		if (!stage(uam::execution_hosts::JoinRemotePath(host.platform, directory, "GEMINI.md"), snapshot)) return false;
		argv.insert(argv.end(), {"--include-directories", directory});
	}
	else if (chat.provider_id != uam::provider_ids::kOpenCodeCli) { error = "The selected provider does not support conversation context files."; return false; }
	if (!remote) return uam::provider_native_context::ConfigureEnvironment(chat.provider_id, uam::paths::PathFromUtf8(directory), environment, error, workspace, false, &PlatformServicesFactory::Instance().process_service, &argv, stop_token);
	// Preserve the terminal proxy's environment trust gate: only its private leased handoff carries overrides.
	launch_channel = "terminal-context-" + PlatformServicesFactory::Instance().process_service.GenerateUuid();
	if (!client->OpenChannel(launch_channel, &error, false, 60000, stop_token)) return false;
	if (!client->WriteChannel(launch_channel, "desktopToRemote", uam::remote::BuildProcessProxySpec("terminal", workspace, argv, {}, false, {}, 0, 0, chat.provider_id, directory), &error, stop_token))
	{
		if (!stop_token.stop_requested()) (void)client->CloseChannel(launch_channel, nullptr, stop_token);
		launch_channel.clear();
		return false;
	}
	return true;
}

// The worker uses a private snapshot. Only the UI owner registers durable cleanup metadata.
bool PrepareCliProviderHandoffAsync(AppState& app, CliTerminalState& terminal, ChatSession& chat, const ExecutionHost& host,
    std::vector<std::string>& argv, std::vector<std::pair<std::string, std::string>>& environment,
    std::string& launch_channel, std::string& error)
{
	if (terminal.context_preparation == nullptr && !PrepareUnboundCodexHistory(app, chat, argv, error)) return false;
	if (terminal.context_preparation == nullptr && (chat.provider_handoff_context.empty() ||
	    (chat.provider_id == uam::provider_ids::kCodexCli && !chat.native_session_id.empty() && chat.provider_handoff_session_id == chat.native_session_id))) return true;
	const std::string baseline = nlohmann::json::array({chat.id, chat.provider_id,
	    uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, chat)),
	    ContextConnectionIdentity(host), chat.provider_handoff_context, argv, environment}).dump();
	if (terminal.context_preparation != nullptr)
	{
		const std::shared_ptr<CliContextPreparation> state = terminal.context_preparation;
		if (state->baseline != baseline)
		{
			state->cancellation.request_stop();
			terminal.context_preparation.reset();
			error = "The chat changed while provider context was preparing. Retry opening its terminal.";
			return false;
		}
		if (!state->finished.load()) return false;
		terminal.context_preparation.reset();
		error = state->error;
		if (!state->succeeded)
		{
			if (error.empty()) error = "Provider context preparation failed. Retry opening its terminal.";
			return false;
		}
		argv = state->argv;
		if (chat.provider_id == uam::provider_ids::kCodexCli && host.id == uam::execution_hosts::kLocalHostId)
		{
			const std::vector<std::string>::const_iterator resume = std::find(argv.cbegin() + 1, argv.cend(), "resume");
			if (resume == argv.cend() || resume + 1 == argv.cend() || !uam::uuid::IsCanonicalUuid(*(resume + 1))) return false;
			const std::string previous_identity = chat.native_session_id;
			const std::string previous_handoff = chat.provider_handoff_session_id;
			chat.native_session_id = *(resume + 1);
			chat.provider_handoff_session_id = chat.native_session_id;
			if (!ChatRepository::SaveChat(app.data_root, chat))
			{
				chat.native_session_id = previous_identity;
				chat.provider_handoff_session_id = previous_handoff;
				error = "Could not save the imported Codex session. Retry opening its terminal.";
				return false;
			}
			terminal.attached_session_id = chat.native_session_id;
			app.resolved_native_sessions_by_chat_id[chat.id] = chat.native_session_id;
		}
		environment = state->environment;
		launch_channel = state->channel;
		return true;
	}
	for (const CliContextPreparationTask& task : app.cli_context_preparation_tasks)
	{
		if (task.state->chat_id == chat.id && !task.state->finished.load())
		{
			error = "Previous provider context preparation is stopping. Retry opening its terminal.";
			return false;
		}
	}
	const ProviderHandoffCliContext location{host.id,
	    uam::paths::Utf8PathString(CliProviderContextDirectory(app, chat, host)), ContextConnectionIdentity(host)};
	if (!uam::ranges::Contains(chat.provider_handoff_cli_contexts, location))
	{
		chat.provider_handoff_cli_contexts.push_back(location);
		if (!ChatRepository::SaveChat(app.data_root, chat))
		{
			chat.provider_handoff_cli_contexts.pop_back();
			error = "Could not save provider context cleanup information.";
			return false;
		}
	}
	const std::shared_ptr<CliContextPreparation> state = std::make_shared<CliContextPreparation>();
	state->baseline = baseline;
	state->chat_id = chat.id;
	state->argv = argv;
	state->environment = environment;
	terminal.context_preparation = state;
	CliContextPreparationTask task;
	task.state = state;
	task.worker = std::make_unique<std::jthread>([state, data_root = app.data_root, settings = app.settings, folders = app.folders, snapshot = chat, host](std::stop_token stop) mutable
	{
		std::stop_callback cancel(stop, [state]() { state->cancellation.request_stop(); });
		AppState isolated;
		isolated.data_root = data_root;
		isolated.settings = settings;
		isolated.folders = folders;
		try
		{
			state->succeeded = PrepareCliProviderHandoff(isolated, snapshot, host, state->argv, state->environment,
			    state->channel, state->error, state->cancellation.get_token());
		}
		catch (...)
		{
			state->error = "Provider context preparation failed. Retry opening its terminal.";
		}
		state->finished.store(true);
	});
	app.cli_context_preparation_tasks.push_back(std::move(task));
	return false;
}

void RepairCliTerminalIdentityForChat(AppState& app, CliTerminalState& terminal, const ChatSession& chat, const ProviderProfile& provider)
{
	const std::string resume_id = ResolveProviderInteractiveResumeId(app, chat, provider);

	if (terminal.frontend_chat_id != chat.id)
	{
		terminal.frontend_chat_id = chat.id;
	}

	if (CliTerminalAttachedChatId(terminal) != chat.id)
	{
		terminal.attached_chat_id = chat.id;
	}

	const std::string expected_terminal_id = CliTerminalIdForChat(chat.id);
	if (terminal.terminal_id != expected_terminal_id)
	{
		terminal.terminal_id = expected_terminal_id;
	}

	if (CliTerminalAttachedSessionId(terminal) != resume_id)
	{
		terminal.attached_session_id = resume_id;
	}
}

CliTerminalState& EnsureCliTerminalForChat(AppState& app, const ChatSession& chat)
{
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	const std::string resume_id = ResolveProviderInteractiveResumeId(app, chat, provider);
	const bool can_launch_terminal = ProviderSupportsInteractiveTerminal(provider);

	if (CliTerminalState* existing = FindCliTerminalForChat(app, chat.id))
	{
		RepairCliTerminalIdentityForChat(app, *existing, chat, provider);

		if (!can_launch_terminal)
		{
			existing->should_launch = false;
			if (!existing->running)
			{
				MarkCliTerminalDisabled(*existing);
			}
		}

		return *existing;
	}

	auto terminal = std::make_unique<CliTerminalState>();
	terminal->terminal_id = CliTerminalIdForChat(chat.id);
	terminal->frontend_chat_id = chat.id;
	terminal->attached_chat_id = chat.id;
	terminal->attached_session_id = resume_id;
	terminal->should_launch = can_launch_terminal;
	if (!can_launch_terminal)
	{
		MarkCliTerminalDisabled(*terminal);
	}
	app.cli_terminals.push_back(std::move(terminal));
	return *app.cli_terminals.back();
}

void MarkSelectedCliTerminalForLaunch(AppState& app)
{
	ChatSession* selected = ChatDomainService().SelectedChat(app);

	if (selected == nullptr)
	{
		return;
	}

	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, *selected);
	const std::string unavailable_reason = ProviderInteractiveTerminalUnavailableReason(provider);
	if (!unavailable_reason.empty())
	{
		app.status_line = unavailable_reason;
		return;
	}

	CliTerminalState& terminal = EnsureCliTerminalForChat(app, *selected);

	if (!terminal.last_error.empty())
	{
		return;
	}

	terminal.should_launch = true;
}

} // namespace uam
