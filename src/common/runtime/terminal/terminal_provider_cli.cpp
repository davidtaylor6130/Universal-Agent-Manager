#include "common/config/host_scoped_settings.h"
#include "common/config/provider_availability.h"
#include "app/uam_control_service.h"
#include "app/provider_configuration_service.h"
#include "common/config/central_provider_configuration.h"
#include "common/runtime/terminal/terminal_provider_cli.h"

#include "app/chat_domain_service.h"
#include "app/agent_run_scheduler.h"
#include "common/chat/chat_repository.h"
#include "common/chat/conversation_handoff.h"
#include "app/provider_resolution_service.h"
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


#include <functional>
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

/// <summary>False with an empty error means a runtime stop is pending; retry after confirmation.</summary>
bool PrepareAcpSessionForCliTerminalLaunch(AppState& app, ChatSession& chat, std::string* error_out)
{
	if (error_out != nullptr)
	{
		error_out->clear();
	}

	if (AgentRunScheduler::HasUnconfirmedInitialRootPublication(app, chat))
	{
		if (error_out)
			*error_out = "Wait for the saved request to be confirmed before opening CLI View.";
		return false;
	}
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	const std::string agent_id = uam::strings::NonEmptyOrFallback(uam::strings::Trim(chat.uam_agent_id), "build");
	if (agent_id != "build" && agent_id != "plan")
	{
		SelectedPrimaryAgentSnapshot snapshot;
		std::string error;
		if (!AgentDefinitionService::CaptureSelectedPrimary(app.data_root,
		        uam::paths::ResolveControllerWorkspaceRootPath(app, chat), agent_id, snapshot, error))
		{
			if (error_out) *error_out = error;
			return false;
		}
		if (!AgentDefinitionService::ValidateCapturedRuntimePolicy(agent_id, snapshot.definition.markdown_snapshot,
		        provider.id, "terminal", error_out)) return false;
	}
	if (!chat.agent_run_id.empty())
	{
		const auto run = std::ranges::find(app.agent_runs, chat.agent_run_id, &AgentRun::id);
		if (run == app.agent_runs.end())
		{
			if (error_out) *error_out = "Managed agent definition snapshot is unavailable.";
			return false;
		}
		if (!AgentDefinitionService::ValidateCapturedRuntimePolicy(run->agent_id, run->definition_snapshot,
		        provider.id, "terminal", error_out)) return false;
	}
	if (!provider_availability::IsEnabled(app.settings, provider.id, chat.execution_host_id))
	{
		if (error_out != nullptr) *error_out = provider_availability::DisabledReason();
		return false;
	}
	if (const std::string update_error = ProviderCliLaunchBlockReason(app, provider.id, chat.execution_host_id); !update_error.empty())
	{
		if (error_out != nullptr) *error_out = update_error;
		return false;
	}
	if (!chat.provider_handoff_context.empty() &&
	    !(chat.provider_id == uam::provider_ids::kCodexCli && !chat.native_session_id.empty() && chat.provider_handoff_session_id == chat.native_session_id) &&
	    !ValidateProviderHandoffContext(chat, chat.messages.size(), error_out)) return false;
	if (!PrepareCodexThreadForRuntimeLaunch(app, chat, error_out)) return false;
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
		if (AcpStopInProgress(app, chat.id) && (session == nullptr || !session->remote_stop_unconfirmed))
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
	if (AcpStopInProgress(app, chat.id)) return false;
	return true;
}

std::vector<std::string> BuildProviderInteractiveArgv(const AppState& app, const ChatSession& chat)
{
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(app, chat);
	ChatSession effective_chat = chat;
	effective_chat.native_session_id = ResolveProviderInteractiveResumeId(app, chat, provider);
	if (app.settings.central_provider_configuration.enabled || chat.uam_control_enabled)
	{
		const AgentDefinitionCatalog catalog = AgentDefinitionService::Load(app.data_root, uam::paths::ResolveControllerWorkspaceRootPath(app, chat));
		const auto selected = std::ranges::find_if(catalog.definitions, [&](const AgentDefinition& agent) { return agent.id == chat.uam_agent_id; });
		if (selected != catalog.definitions.end() && selected->workspace_access == "read") effective_chat.approval_mode = "plan";
	}

	return ProviderRuntime::BuildInteractiveArgv(provider, effective_chat, app.settings);
}

std::string BuildProviderHandoffContext(const ChatSession& chat, std::stop_token stop_token, std::string* error)
{
	return BuildProviderHandoffContext(std::span<const Message>(chat.messages), stop_token, error);
}

std::string BuildProviderHandoffContext(std::span<const Message> messages, std::stop_token stop_token, std::string* error, std::size_t budget)
{
	if (error != nullptr) error->clear();
	for (const Message& message : messages)
	{
		if (stop_token.stop_requested()) return {};
		if (!message.acp_prompt_discarded && uam::chat::HandoffHasQuarantinedSummary(message) && !uam::chat::HandoffUsesBlockOrder(message))
		{
			if (error != nullptr) *error = uam::chat::kAmbiguousCompactionHistory;
			return {};
		}
	}
	constexpr std::string_view kOmissionNotice = "[Earlier conversation omitted to fit the handoff budget. Full history remains in the saved chat.]\n\n";
	constexpr std::string_view kTruncatedEntryNotice = "[Earlier text in this entry omitted]\n";
	std::deque<std::string> entries;
	if (budget <= kOmissionNotice.size()) return {};
	std::size_t remaining = budget - kOmissionNotice.size();
	bool omitted = false;
	const auto append = [&](std::string_view label, const std::string& content)
	{
		if (uam::strings::IsBlank(content)) return;
		if (label.size() + content.size() + 2 <= remaining)
		{
			entries.push_front(std::string(label) + content + "\n\n");
			remaining -= entries.front().size();
			return;
		}
		omitted = true;
		// Keep a UTF-8 aligned suffix of the newest oversized entry, never an unlabeled fragment.
		if (entries.empty() && remaining > label.size() + kTruncatedEntryNotice.size() + 2)
		{
			std::size_t start = content.size() - (remaining - label.size() - kTruncatedEntryNotice.size() - 2);
			while (start < content.size() && (static_cast<unsigned char>(content[start]) & 0xc0) == 0x80) ++start;
			entries.push_front(std::string(label) + std::string(kTruncatedEntryNotice) + content.substr(start) + "\n\n");
		}
		remaining = 0;
	};
	for (auto message = messages.rbegin(); message != messages.rend(); ++message)
	{
		if (stop_token.stop_requested()) return {};
		if (message->acp_prompt_discarded) continue;
		if (uam::chat::HandoffUsesBlockOrder(*message))
		{
			for (std::vector<MessageBlock>::const_reverse_iterator block = message->blocks.crbegin(); block != message->blocks.crend(); ++block)
			{
				if (block->type == "context_compaction" || block->type == "context_compaction_interrupted")
					append(block->type == "context_compaction_interrupted" ? "Interrupted conversation summary: " : "Conversation summary: ", block->text);
				else if (block->type == "assistant_text") append("Assistant: ", block->text);
			}
		}
		else
		{
			if (message->role == MessageRole::User || message->role == MessageRole::Assistant)
				append(message->role == MessageRole::User ? "User: " : "Assistant: ", message->content);
			for (std::vector<MessageBlock>::const_reverse_iterator block = message->blocks.crbegin(); block != message->blocks.crend(); ++block)
				if (block->type == "context_compaction" || block->type == "context_compaction_interrupted")
					append(block->type == "context_compaction_interrupted" ? "Interrupted conversation summary: " : "Conversation summary: ", block->text);
		}
	}
	std::string context;
	if (omitted) context = kOmissionNotice;
	for (const std::string& entry : entries) context += entry;
	return context;
}

bool ValidateProviderHandoffContext(const ChatSession& chat, std::size_t history_count, std::string* error, std::stop_token stop_token)
{
	if (error) error->clear();
	if (chat.provider_handoff_context.empty()) return true;
	if (!chat.messages_loaded)
	{
		if (error) *error = "Load the complete saved history before replaying provider context. Input and context are retained.";
		return false;
	}
	const auto messages = std::span<const Message>(chat.messages);
	if (std::ranges::none_of(messages, [](const Message& message)
	    { return !message.acp_prompt_discarded && uam::chat::HandoffHasQuarantinedSummary(message); })) return true;
	std::string projection_error;
	const std::size_t prefix_count = std::min(history_count, messages.size());
	const std::string projection = BuildProviderHandoffContext(messages.first(prefix_count), stop_token, &projection_error);
	if (!stop_token.stop_requested() && projection_error.empty() && projection == chat.provider_handoff_context) return true;
	// A provider switch may have captured the selected unsent row as reference context.
	// Accept only an exact safe projection, never a substring or unproved legacy cache.
	if (prefix_count < messages.size())
	{
		const std::string full_projection = BuildProviderHandoffContext(messages, stop_token, &projection_error);
		if (!stop_token.stop_requested() && projection_error.empty() && full_projection == chat.provider_handoff_context) return true;
	}
	if (error) *error = projection_error.empty()
	    ? "Saved provider context cannot be verified against interrupted compaction history. Input and context are retained."
	    : projection_error;
	return false;
}

static bool NeedsUnboundCodexHistory(const ChatSession& chat, const std::vector<std::string>& argv)
{
	return chat.provider_id == uam::provider_ids::kCodexCli && chat.provider_handoff_context.empty() && chat.native_session_id.empty() &&
	    std::find(argv.begin(), argv.end(), "resume") == argv.end() && (!chat.messages_loaded || !chat.messages.empty());
}

/// <summary>Reads a detached, identity-validated transcript before constructing native context.</summary>
static bool PrepareUnboundCodexHistory(AppState& app, ChatSession& chat, const std::vector<std::string>& argv, std::string& error, std::stop_token stop_token = {})
{
	const bool build_history = NeedsUnboundCodexHistory(chat, argv);
	const bool replay_context = !chat.provider_handoff_context.empty() &&
	    !(chat.provider_id == uam::provider_ids::kCodexCli && !chat.native_session_id.empty() && chat.provider_handoff_session_id == chat.native_session_id);
	if (!build_history && !replay_context) return true;
	if (stop_token.stop_requested()) { error = "Provider context preparation was cancelled."; return false; }
	if (!chat.messages_loaded)
	{
		const ChatDomainService domain;
		const ChatDomainService::BranchHistoryIdentity identity = domain.CaptureBranchHistoryIdentity(chat);
		ChatSession loaded = chat;
		if (!ChatRepository::HydrateChatMessages(app.data_root, loaded, &error) || !domain.MatchesBranchHistoryIdentity(loaded, identity))
		{
			error = "Could not load the unchanged complete conversation before opening its terminal.";
			return false;
		}
		chat.messages = std::move(loaded.messages);
		chat.messages_loaded = true;
	}
	if (!ValidateProviderHandoffContext(chat, chat.messages.size(), &error, stop_token)) return false;
	if (!build_history) return true;
	const std::string context = BuildProviderHandoffContext(chat, stop_token, &error);
	if (!error.empty()) return false;
	if (stop_token.stop_requested()) { error = "Provider context preparation was cancelled."; return false; }
	chat.provider_handoff_context = context;
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

bool PrepareCliProviderConversation(AppState& app, ChatSession& chat, const ExecutionHost& host,
    std::vector<std::string>& argv, std::vector<std::pair<std::string, std::string>>& environment,
    std::string& launch_channel, std::string& error, std::stop_token stop_token, const std::string& configuration_directory, const nlohmann::json& native_servers)
{
	launch_channel.clear();
	if (!PrepareUnboundCodexHistory(app, chat, argv, error, stop_token)) return false;
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
	if (!chat.provider_handoff_context.empty() && !uam::ranges::Contains(chat.provider_handoff_cli_contexts, location))
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
	if (!client->WriteChannel(launch_channel, "desktopToRemote", uam::remote::BuildProcessProxySpec("terminal", workspace, argv, environment, false, {}, 0, 0, chat.provider_id, directory, configuration_directory.empty() ? "" : chat.provider_id, configuration_directory, native_servers), &error, stop_token))
	{
		if (!stop_token.stop_requested()) (void)client->CloseChannel(launch_channel, nullptr, stop_token);
		launch_channel.clear();
		return false;
	}
	return true;
}

/// <summary>Terminal handoff remains unavailable; managed configuration uses private additional directories.</summary>
static bool ValidateAntigravityTerminalConfiguration(const ChatSession& chat, std::string& error)
{
	if (!uam::provider_ids::IsCliProviderAliasOf(chat.provider_id, uam::provider_ids::kAntigravityCli)) return true;
	if (!chat.provider_handoff_context.empty())
	{
		error = "Antigravity terminal cannot import another provider's conversation. Start a new Antigravity chat or resume an existing conversation.";
		return false;
	}
	return true;
}

bool PrepareConfiguredCliProviderHandoff(AppState& app, ChatSession& chat, const ExecutionHost& host,
    std::vector<std::string>& argv, std::vector<std::pair<std::string, std::string>>& environment,
    std::string& launch_channel, std::string& error, std::stop_token stop_token, const nlohmann::json& native_servers, std::filesystem::path* installed_directory = nullptr)
{
	if (!ValidateAntigravityTerminalConfiguration(chat, error)) return false;
	if (!PrepareUnboundCodexHistory(app, chat, argv, error, stop_token)) return false;
	const std::filesystem::path workspace = uam::paths::ResolveWorkspaceRootPath(app, chat);
	ProviderConfigurationBundle bundle;
	AppSettings settings = app.settings;
	if (!native_servers.empty() && !settings.central_provider_configuration.enabled)
	{
		settings.central_provider_configuration = {};
		settings.central_provider_configuration.enabled = true;
	}
	if (!PrepareProviderConfiguration(app.data_root, settings, chat, host, workspace, bundle, error, nullptr,
	                                 CheckedProviderConfigurationVersion(app, chat.provider_id, host))) return false;
	if (installed_directory) *installed_directory = bundle.local_directory;
	std::filesystem::path target;
	std::unique_ptr<uam::remote::RunnerClient> client;
	if (!bundle.local_directory.empty() && host.id != uam::execution_hosts::kLocalHostId)
	{
		target = workspace / ".UAM" / "provider-setup" / bundle.local_directory.filename();
		client = std::make_unique<uam::remote::RunnerClient>(PlatformServicesFactory::Instance().process_service,
		    uam::remote::SshBridgeArgv(host.ssh_alias, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version), host.runner_version, host.runner_protocol_version);
		if (!client->InstallProviderConfiguration(bundle.local_directory, target, &error, stop_token)) return false;
	}
	if (!PrepareCliProviderConversation(app, chat, host, argv, environment, launch_channel, error, stop_token, uam::paths::Utf8PathString(target), native_servers)) return false;
	if (bundle.local_directory.empty() && (host.id == uam::execution_hosts::kLocalHostId || environment.empty() || !launch_channel.empty()))
		return true;
	if (host.id == uam::execution_hosts::kLocalHostId)
		return uam::provider_setup::Apply(chat.provider_id, bundle.local_directory, workspace, argv, environment, error, native_servers);
	if (launch_channel.empty())
	{
		if (!client)
			client = std::make_unique<uam::remote::RunnerClient>(PlatformServicesFactory::Instance().process_service, uam::remote::SshBridgeArgv(host.ssh_alias, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version), host.runner_version, host.runner_protocol_version);
		launch_channel = "terminal-configuration-" + PlatformServicesFactory::Instance().process_service.GenerateUuid();
		if (!client->OpenChannel(launch_channel, &error, false, 60000, stop_token)) return false;
		if (!client->WriteChannel(launch_channel, "desktopToRemote", uam::remote::BuildProcessProxySpec("terminal", workspace, argv, environment, false, {}, 0, 0, {}, {}, target.empty() ? "" : chat.provider_id, uam::paths::Utf8PathString(target), native_servers), &error, stop_token))
		{
			if (!stop_token.stop_requested()) (void)client->CloseChannel(launch_channel, nullptr, stop_token);
			launch_channel.clear();
			return false;
		}
	}
	return true;
}

bool PrepareCliProviderHandoff(AppState& app, ChatSession& chat, const ExecutionHost& host,
    std::vector<std::string>& argv, std::vector<std::pair<std::string, std::string>>& environment,
    std::string& launch_channel, std::string& error, std::stop_token stop_token)
{
	return PrepareConfiguredCliProviderHandoff(app, chat, host, argv, environment, launch_channel, error, stop_token, nlohmann::json::array());
}

// The worker uses a private snapshot. Only the UI owner registers durable cleanup metadata.
static bool PrepareCliProviderHandoffAsyncImpl(AppState& app, CliTerminalState& terminal, ChatSession& chat, const ExecutionHost& host,
    std::vector<std::string>& argv, std::vector<std::pair<std::string, std::string>>& environment,
    std::string& launch_channel, std::string& error, bool history_prepared = false)
{
	if (!ValidateAntigravityTerminalConfiguration(chat, error)) return false;
	if (terminal.context_preparation == nullptr)
	{
		for (const CliContextPreparationTask& task : app.cli_context_preparation_tasks)
		{
			if (task.state->chat_id == chat.id && !task.state->finished.load())
			{
				error = "Previous provider context preparation is stopping. Retry opening its terminal.";
				return false;
			}
		}
	}
	const bool launch_option_override = (ProviderRuntimeRegistry::ResolveById(chat.provider_id).ModelOptionsRequireRestart() && !chat.reasoning_effort.empty()) || (uam::provider_ids::IsCliProviderAliasOf(chat.provider_id, uam::provider_ids::kClaudeCli) && (chat.service_tier == "fast" || chat.service_tier_explicit));
	const bool prepare_history = NeedsUnboundCodexHistory(chat, argv) || (!chat.messages_loaded && !chat.provider_handoff_context.empty());
	if (chat.messages_loaded && !chat.provider_handoff_context.empty() &&
	    !(chat.provider_id == uam::provider_ids::kCodexCli && !chat.native_session_id.empty() && chat.provider_handoff_session_id == chat.native_session_id) &&
	    !ValidateProviderHandoffContext(chat, chat.messages.size(), &error)) return false;
	const ProviderHandoffCliContext location{host.id,
	    uam::paths::Utf8PathString(CliProviderContextDirectory(app, chat, host)), ContextConnectionIdentity(host)};
	const bool prepare_cleanup = !chat.messages_loaded && !chat.provider_handoff_context.empty() &&
	    !uam::ranges::Contains(chat.provider_handoff_cli_contexts, location);
	if (!launch_option_override && terminal.context_preparation == nullptr && (host.id == uam::execution_hosts::kLocalHostId || environment.empty()) && (!prepare_history || history_prepared) && !uam::central_configuration::McpEnabled(app.settings.central_provider_configuration, chat.provider_id, host.id, "shared-setup", app.settings.central_provider_configuration.enabled) && uam::mcp_server_config::SelectForWorkspace(uam::host_scoped_settings::McpServers(app.settings, host.id), uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, chat)), host.id).empty() && (chat.workspace_source_directory.empty() || uam::mcp_server_config::SelectForWorkspace(uam::host_scoped_settings::McpServers(app.settings, host.id), chat.workspace_source_directory, host.id).empty()) && !chat.uam_control_enabled && (chat.provider_handoff_context.empty() || (chat.provider_id == uam::provider_ids::kCodexCli && !chat.native_session_id.empty() && chat.provider_handoff_session_id == chat.native_session_id)))
		return true;
	const std::function<std::string()> capture_baseline = [&]() { return nlohmann::json::array({chat.id, chat.provider_id, uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, chat)), ContextConnectionIdentity(host), host.instruction_file, host.instruction_text ? nlohmann::json(*host.instruction_text) : nlohmann::json(nullptr), chat.uam_control_enabled, chat.uam_agent_id, chat.provider_handoff_context, chat.native_session_id, chat.provider_handoff_session_id, ChatRepository::ChatMetadataFingerprint(app.data_root, chat), chat.model_id, chat.reasoning_effort, chat.service_tier, chat.service_tier_explicit, CheckedProviderConfigurationVersion(app, chat.provider_id, host), argv, environment, uam::central_configuration::Serialize(app.settings.central_provider_configuration), uam::mcp_server_config::Serialize(uam::host_scoped_settings::McpServers(app.settings, host.id))}).dump(); };
	const std::string baseline = capture_baseline();
	if (terminal.context_preparation != nullptr)
	{
		const std::shared_ptr<CliContextPreparation> state = terminal.context_preparation;
		const bool finished = state->finished.load();
		if (state->baseline != baseline || state->cancellation.stop_requested() ||
		    (finished && ((chat.messages_loaded && chat.messages != state->baseline_messages) ||
		                  ChatRepository::ChatStorageRevision(app.data_root, chat.id) != state->storage_revision)))
		{
			state->cancellation.request_stop();
			terminal.context_preparation.reset();
			error = "The chat changed while provider context was preparing. Retry opening its terminal.";
			return false;
		}
		if (!finished) return false;
		terminal.context_preparation.reset();
		error = state->error;
		if (!state->succeeded)
		{
			if (terminal.uam_control_session != nullptr) UamControlService::RevokeForSession(app, *terminal.uam_control_session);
			terminal.uam_control_session.reset();
			terminal.uam_control_relay.reset();
			if (error.empty()) error = "Provider context preparation failed. Retry opening its terminal.";
			return false;
		}
		// Admit prepared cleanup metadata before any native files or remote channels can be staged.
		if (state->history_preparation)
		{
			if (state->prepared_chat_save && !ChatRepository::PublishPreparedChatSave(state->prepared_chat_save, true, &chat))
			{
				error = "Could not save provider context cleanup information.";
				return false;
			}
			chat.provider_handoff_context = state->prepared_context;
			if (state->prepared_chat_save)
			{
				const ProviderHandoffCliContext location{host.id,
				    uam::paths::Utf8PathString(CliProviderContextDirectory(app, chat, host)), ContextConnectionIdentity(host)};
				if (!uam::ranges::Contains(chat.provider_handoff_cli_contexts, location)) chat.provider_handoff_cli_contexts.push_back(location);
			}
			return PrepareCliProviderHandoffAsyncImpl(app, terminal, chat, host, argv, environment, launch_channel, error, true);
		}
		if (terminal.uam_control_session != nullptr)
		{
			terminal.uam_control_session->provider_configuration_directory = state->provider_configuration_directory;
			for (UamControlCapability& capability : app.uam_control_capabilities)
				if (capability.id == terminal.uam_control_session->uam_control_capability_id) capability.provider_configuration_directory = state->provider_configuration_directory;
		}
		argv = state->argv;
		if (chat.provider_id == uam::provider_ids::kCodexCli && host.id == uam::execution_hosts::kLocalHostId && !chat.provider_handoff_context.empty() && (chat.native_session_id.empty() || chat.provider_handoff_session_id != chat.native_session_id))
		{
			const std::vector<std::string>::const_iterator resume = std::find(argv.cbegin() + 1, argv.cend(), "resume");
			if (resume == argv.cend() || resume + 1 == argv.cend() || !uam::uuid::IsCanonicalUuid(*(resume + 1))) return false;
			const std::string previous_identity = chat.native_session_id;
			const std::string previous_handoff = chat.provider_handoff_session_id;
			chat.native_session_id = *(resume + 1);
			chat.provider_handoff_session_id = chat.native_session_id;
			if (!state->prepared_chat_save || !ChatRepository::PublishPreparedChatSave(state->prepared_chat_save, true, &chat))
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
	if (!prepare_history && !prepare_cleanup && !chat.provider_handoff_context.empty() && !uam::ranges::Contains(chat.provider_handoff_cli_contexts, location))
	{
		chat.provider_handoff_cli_contexts.push_back(location);
		if (!ChatRepository::SaveChat(app.data_root, chat))
		{
			chat.provider_handoff_cli_contexts.pop_back();
			error = "Could not save provider context cleanup information.";
			return false;
		}
	}
	const std::optional<std::string> storage_revision = ChatRepository::ChatStorageRevision(app.data_root, chat.id);
	if (!storage_revision)
	{
		error = "Could not validate saved provider conversation history.";
		return false;
	}
	const std::shared_ptr<CliContextPreparation> state = std::make_shared<CliContextPreparation>();
	state->storage_revision = *storage_revision;
	state->baseline = capture_baseline();
	state->history_preparation = (prepare_history || prepare_cleanup) && !history_prepared;
	if (chat.messages_loaded) state->baseline_messages = chat.messages;
	state->chat_id = chat.id;
	state->argv = argv;
	state->environment = environment;
	terminal.context_preparation = state;
	nlohmann::json native_servers = nlohmann::json::array();
	if (!state->history_preparation && chat.uam_control_enabled)
	{
		nlohmann::json server;
		if (!UamControlService::PrepareTerminalMcpServer(app, terminal, chat, server, error))
		{ terminal.context_preparation.reset(); return false; }
		if (host.id != uam::execution_hosts::kLocalHostId)
		{
			const std::string channel = "terminal-control-" + PlatformServicesFactory::Instance().process_service.GenerateUuid();
			std::vector<std::string> control_argv{server["command"].get<std::string>()};
			for (const nlohmann::json& argument : server["args"]) control_argv.push_back(argument.get<std::string>());
			uam::provider_setup::Environment control_environment;
			for (const nlohmann::json& entry : server["env"]) control_environment.emplace_back(entry["name"].get<std::string>(), entry["value"].get<std::string>());
			const std::string spec = uam::remote::BuildProcessProxySpec(channel, uam::paths::PathFromUtf8(control_argv.front()).parent_path(), control_argv, control_environment);
			terminal.uam_control_relay = std::make_unique<std::jthread>([host, spec](std::stop_token stop) {
				uam::remote::RunLocalMcpRelayFromSpec(stop, host.ssh_alias, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version, spec);
			});
			server = uam::remote::BuildRemoteMcpServer(channel, host.platform, host.runner_version, host.runner_directory, host.runner_protocol_version);
		}
		native_servers.push_back(std::move(server));
	}
	CliContextPreparationTask task;
	task.state = state;
	task.worker = std::make_unique<std::jthread>(
		[state, data_root = app.data_root, settings = app.settings, folders = app.folders, snapshot = chat, host, native_servers, checked_versions = app.runtime_cli_versions_by_provider_id](std::stop_token stop) mutable
		{
			std::stop_callback cancel(stop, [state]() { state->cancellation.request_stop(); });
			AppState isolated;
			isolated.data_root = data_root;
			isolated.settings = settings;
			isolated.folders = folders;
			isolated.runtime_cli_versions_by_provider_id = std::move(checked_versions);
			try
			{
				if (state->history_preparation)
				{
					state->succeeded = PrepareUnboundCodexHistory(isolated, snapshot, state->argv, state->error, state->cancellation.get_token());
					if (state->succeeded)
					{
						state->prepared_context = snapshot.provider_handoff_context;
						if (!snapshot.provider_handoff_context.empty())
						{
							const ProviderHandoffCliContext location{host.id, uam::paths::Utf8PathString(CliProviderContextDirectory(isolated, snapshot, host)), ContextConnectionIdentity(host)};
							if (!uam::ranges::Contains(snapshot.provider_handoff_cli_contexts, location))
								snapshot.provider_handoff_cli_contexts.push_back(location);
							state->prepared_chat_save = ChatRepository::PrepareChatSave(data_root, snapshot, state->storage_revision);
							state->succeeded = state->prepared_chat_save != nullptr;
						}
					}
				}
				else
					state->succeeded = PrepareConfiguredCliProviderHandoff(isolated, snapshot, host, state->argv, state->environment, state->channel, state->error, state->cancellation.get_token(), native_servers, &state->provider_configuration_directory);
				if (state->succeeded && !state->history_preparation && snapshot.provider_id == uam::provider_ids::kCodexCli && host.id == uam::execution_hosts::kLocalHostId && !snapshot.provider_handoff_context.empty() && (snapshot.native_session_id.empty() || snapshot.provider_handoff_session_id != snapshot.native_session_id))
				{
					const std::vector<std::string>::const_iterator resume = std::find(state->argv.cbegin() + 1, state->argv.cend(), "resume");
					if (resume == state->argv.cend() || resume + 1 == state->argv.cend() || !uam::uuid::IsCanonicalUuid(*(resume + 1)))
						state->succeeded = false;
					else
					{
						snapshot.native_session_id = *(resume + 1);
						snapshot.provider_handoff_session_id = snapshot.native_session_id;
						state->prepared_chat_save = ChatRepository::PrepareChatSave(data_root, snapshot, state->storage_revision);
						state->succeeded = state->prepared_chat_save != nullptr;
					}
				}
				if (state->cancellation.stop_requested())
					state->succeeded = false;
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

bool PrepareCliProviderHandoffAsync(AppState& app, CliTerminalState& terminal, ChatSession& chat, const ExecutionHost& host,
    std::vector<std::string>& argv, std::vector<std::pair<std::string, std::string>>& environment,
    std::string& launch_channel, std::string& error)
{
	return PrepareCliProviderHandoffAsyncImpl(app, terminal, chat, host, argv, environment, launch_channel, error);
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
