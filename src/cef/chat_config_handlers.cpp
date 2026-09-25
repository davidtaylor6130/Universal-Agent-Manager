#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"

#include "app/chat_domain_service.h"
#include "app/agent_definition_service.h"
#include "app/agent_run_scheduler.h"
#include "app/chat_lifecycle_service.h"
#include "app/computer_use_service.h"
#include "app/persistence_coordinator.h"
#include "app/provider_resolution_service.h"
#include "app/runtime_orchestration_services.h"
#include "app/uam_control_service.h"
#include "cef/cef_push.h"
#include "common/chat/chat_repository.h"
#include "common/config/approval_modes.h"
#include "common/config/execution_host_config.h"
#include "common/config/provider_chat_defaults.h"
#include "computer_use/computer_use_mcp_config.h"
#include "computer_use/computer_use_platform.h"
#include "common/memory/memory_levels.h"
#include "common/paths/workspace_root.h"
#include "common/paths/path_utils.h"
#include "common/platform/platform_services.h"
#include "common/provider/codex/codex_options.h"
#include "common/provider/provider_ids.h"
#include "common/provider/provider_profile.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/runtime/terminal/terminal_lifecycle.h"
#include "common/security/command_safety.h"
#include "common/utils/range_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Chat configuration handlers (model, provider, approval, memory)
// ---------------------------------------------------------------------------

namespace
{
	uam::query_handler_async::AsyncCefResult RunComputerUseSettingsHelperOnce(
	    std::string action, std::string permission = {})
	{
		const bool user_prompt = action == "request";
		std::vector<std::string> argv = {
		    uam::computer_use::McpExecutablePath(), "--uam-computer-use-mcp", "--settings-action", std::move(action)};
		if (!permission.empty())
		{
			argv.push_back("--permission");
			argv.push_back(std::move(permission));
		}

		uam::platform::StdioProcessPlatformFields process;
		std::string error;
		auto& process_service = PlatformServicesFactory::Instance().process_service;
		if (!process_service.StartStdioProcess(process, {}, argv, &error))
			return uam::query_handler_async::AsyncFailure(500, error.empty() ? "Computer Use helper could not start." : error);
		process_service.CloseStdioProcessInput(process);

		std::string output;
		std::string stderr_output;
		std::array<char, 4096> buffer{};
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(user_prompt ? 120 : 60);
		int exit_code = -1;
		bool exited = false;
		for (;;)
		{
			std::string read_error;
			const std::ptrdiff_t read = process_service.ReadStdioProcessStdout(process, buffer.data(), buffer.size(), &read_error);
			if (read > 0)
			{
				output.append(buffer.data(), static_cast<std::size_t>(read));
				if (output.size() > 1024 * 1024)
				{
					process_service.TerminateStdioProcess(process, true);
					process_service.CloseStdioProcessHandles(process);
					return uam::query_handler_async::AsyncFailure(502, "Computer Use helper returned too much output.");
				}
			}
			const std::ptrdiff_t error_read = process_service.ReadStdioProcessStderr(process, buffer.data(), buffer.size(), nullptr);
			if (error_read > 0)
			{
				stderr_output.append(buffer.data(), static_cast<std::size_t>(error_read));
				if (stderr_output.size() > 1024 * 1024)
				{
					process_service.TerminateStdioProcess(process, true);
					process_service.CloseStdioProcessHandles(process);
					return uam::query_handler_async::AsyncFailure(502, "Computer Use helper returned too much diagnostic output.");
				}
			}
			if (!exited) exited = process_service.PollStdioProcessExited(process, &exit_code);
			if (exited && read <= 0 && error_read <= 0) break;
			if (std::chrono::steady_clock::now() >= deadline)
			{
				process_service.TerminateStdioProcess(process, true);
				process_service.CloseStdioProcessHandles(process);
				return uam::query_handler_async::AsyncFailure(504, "Computer Use helper timed out.");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		process_service.CloseStdioProcessHandles(process);
		const nlohmann::json result = nlohmann::json::parse(output, nullptr, false);
		if (exit_code != 0)
			return uam::query_handler_async::AsyncFailure(409, result.is_object() ? result.value("error", "Computer Use helper failed.") : stderr_output.empty() ? "Computer Use helper failed." : stderr_output);
		if (!result.is_object() && !result.is_array())
			return uam::query_handler_async::AsyncFailure(502, stderr_output.empty() ? "Computer Use helper returned invalid output." : stderr_output);
		return uam::query_handler_async::AsyncSuccess(result);
	}

	uam::query_handler_async::AsyncCefResult RunComputerUseSettingsHelper(
	    const std::string& action, const std::string& permission = {})
	{
		uam::query_handler_async::AsyncCefResult result = RunComputerUseSettingsHelperOnce(action, permission);
		if ((action == "check" || action == "list") && !result.ok && result.status == 502 &&
		    result.error == "Computer Use helper returned invalid output.")
			result = RunComputerUseSettingsHelperOnce(action, permission);
		return result;
	}
}

using namespace uam::query_handler_internal;

void UamQueryHandler::HandleOpenNativeSessionChat(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	using namespace uam::query_handler_async;
	const std::string source_id = payload.value("chatId", "");
	const std::string native_id = uam::strings::Trim(payload.value("nativeSessionId", ""));
	ChatSession* source = ChatDomainService().FindChatById(m_app, source_id);
	const ProviderProfile* provider = source == nullptr ? nullptr : &ProviderResolutionService().ProviderForChatOrDefault(m_app, *source);
	const std::string provider_id = provider == nullptr ? std::string{} : uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider->id);
	const bool needs_export = source != nullptr && provider != nullptr && !native_id.empty() &&
	    (provider_id == uam::provider_ids::kOpenCodeCli || provider_id == uam::provider_ids::kCodexCli) &&
	    (payload.value("refreshHistory", !payload.value("selectChat", true)) || ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(m_app, *source, *provider, native_id, false) == nullptr);
	if (!needs_export)
	{
		const AsyncCefResult result = FinishOpenNativeSessionChat(browser, payload);
		if (result.ok) cb->Success(result.body);
		else cb->Failure(result.status, result.error);
		return;
	}

	std::optional<ExecutionHost> remote_host;
	if (!uam::paths::IsControllerLocalWorkspace(*source))
	{
		const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, source->execution_host_id);
		if (host == nullptr || host->runner_status != "ready")
		{
			cb->Failure(409, "The remote execution host is not ready to load child history.");
			return;
		}
		remote_host = *host;
	}

	ChatSession* target = ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(m_app, *source, *provider, native_id, false);
	std::string hydrate_error;
	if (target != nullptr && !ChatRepository::HydrateChatMessages(m_app.data_root, *target, &hydrate_error))
	{
		cb->Failure(500, uam::strings::NonEmptyOrFallback(hydrate_error, "Failed to load the current child history."));
		return;
	}
	const std::string target_id = target == nullptr ? std::string{} : target->id;
	const std::string target_digest = target == nullptr ? std::string{} : uam::StateSerializer::MessageDigest(*target);
	const std::string request_key = target_id.empty() ? source_id + "/" + native_id : target_id;
	const auto previous_request = m_nativeHistoryRequests.find(request_key);
	if (previous_request != m_nativeHistoryRequests.end()) previous_request->second->request_stop();
	const std::shared_ptr<std::stop_source> request = std::make_shared<std::stop_source>();
	m_nativeHistoryRequests[request_key] = request;

	ChatSession identity;
	identity.id = source_id;
	identity.provider_id = source->provider_id;
	identity.native_session_id = source->native_session_id;
	identity.workspace_directory = source->workspace_directory;
	identity.execution_host_id = source->execution_host_id;
	identity.folder_id = source->folder_id;
	auto snapshot = std::make_shared<ChatSession>();
	snapshot->id = native_id;
	snapshot->native_session_id = native_id;
	snapshot->provider_id = provider_id;
	snapshot->workspace_directory = source->workspace_directory;
	snapshot->folder_id = source->folder_id;
	snapshot->execution_host_id = source->execution_host_id;
	snapshot->title = uam::strings::NonEmptyOrFallback(uam::strings::Trim(payload.value("title", "")), native_id);
	snapshot->created_at = target == nullptr ? uam::time::TimestampNow() : target->created_at;
	snapshot->updated_at = target == nullptr ? snapshot->created_at : target->updated_at;
	if (!CefPostTask(TID_FILE_BACKGROUND, new CefQueryWorkerTask(m_asyncLifetime, cb,
	    [snapshot, remote_host, request, profile_snapshot = *provider]()
	    {
		    if (request->stop_requested()) return AsyncFailure(409, "A newer history refresh was requested.");
		    if (!remote_host && snapshot->provider_id == uam::provider_ids::kCodexCli)
		    {
			    std::string error;
			    std::optional<ChatSession> loaded = ChatHistorySyncService().LoadLocalCodexChildChat(*snapshot, &error);
			    if (!loaded) return AsyncFailure(502, uam::strings::NonEmptyOrFallback(error, "Codex child history is unavailable."));
			    *snapshot = std::move(*loaded);
			    return AsyncSuccess({});
		    }
		    if (remote_host && snapshot->provider_id == uam::provider_ids::kCodexCli)
		    {
			    auto transcript = ChatHistorySyncService().LoadRemoteCodexTranscript(*remote_host, *snapshot, request->get_token());
			    if (!transcript.success) return AsyncFailure(502, transcript.error);
			    snapshot->messages = std::move(transcript.messages);
		    }
		    else
		    {
			    auto transcript = remote_host
			        ? ChatHistorySyncService().LoadRemoteOpenCodeTranscript(*remote_host, *snapshot, profile_snapshot, request->get_token())
			        : ChatHistorySyncService().LoadLocalOpenCodeTranscript(*snapshot, profile_snapshot, request->get_token());
			    if (!transcript.success) return AsyncFailure(502, transcript.error);
			    snapshot->messages = std::move(transcript.messages);
		    }
		    snapshot->messages_loaded = true;
		    snapshot->created_at = uam::strings::NonEmptyOrFallback(snapshot->messages.empty() ? std::string{} : snapshot->messages.front().created_at, snapshot->created_at);
		    snapshot->updated_at = uam::strings::NonEmptyOrFallback(snapshot->messages.empty() ? std::string{} : snapshot->messages.back().created_at, snapshot->updated_at);
		    return AsyncSuccess({});
	    },
	    [this, browser, payload, identity, snapshot, remote_host, target_id, target_digest, request_key, request](AsyncCefResult& result)
	    {
		    const auto pending = m_nativeHistoryRequests.find(request_key);
		    if (pending == m_nativeHistoryRequests.end() || pending->second != request)
		    {
			    result = AsyncFailure(409, "A newer history refresh was requested.");
			    return;
		    }
		    m_nativeHistoryRequests.erase(pending);
		    if (!result.ok) return;
		    const ChatSession* current = ChatDomainService().FindChatById(m_app, identity.id);
		    if (current == nullptr || current->provider_id != identity.provider_id ||
		        current->native_session_id != identity.native_session_id ||
		        current->workspace_directory != identity.workspace_directory ||
		        current->execution_host_id != identity.execution_host_id || current->folder_id != identity.folder_id)
		    {
			    result = AsyncFailure(409, "Source chat changed while its child history was loading.");
			    return;
		    }
		    if (remote_host)
		    {
			    const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, remote_host->id);
			    if (host == nullptr || host->transport != remote_host->transport ||
			        host->ssh_alias != remote_host->ssh_alias || host->platform != remote_host->platform ||
			        host->runner_directory != remote_host->runner_directory ||
			        host->runner_version != remote_host->runner_version ||
			        host->runner_protocol_version != remote_host->runner_protocol_version)
			    {
				    result = AsyncFailure(409, "Remote host changed while its child history was loading.");
				    return;
			    }
		    }
		    const ProviderProfile& current_provider = ProviderResolutionService().ProviderForChatOrDefault(m_app, *current);
		    ChatSession* current_target = ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(m_app, *current, current_provider, snapshot->native_session_id, false);
		    if ((current_target == nullptr ? std::string{} : current_target->id) != target_id)
		    {
			    result = AsyncFailure(409, "Child chat changed while its history was loading. Try again.");
			    return;
		    }
		    if (current_target != nullptr)
		    {
			    std::string error;
			    if (!ChatRepository::HydrateChatMessages(m_app.data_root, *current_target, &error))
			    {
				    result = AsyncFailure(500, uam::strings::NonEmptyOrFallback(error, "Failed to reload the current child history."));
				    return;
			    }
			    if (uam::StateSerializer::MessageDigest(*current_target) != target_digest)
			    {
				    result = AsyncFailure(409, "Child chat changed while its history was loading. Try again.");
				    return;
			    }
		    }
		    result = FinishOpenNativeSessionChat(browser, payload, snapshot.get());
	    })))
	{
		m_nativeHistoryRequests.erase(request_key);
		cb->Failure(503, "The background task queue is unavailable.");
	}
}

uam::query_handler_async::AsyncCefResult UamQueryHandler::FinishOpenNativeSessionChat(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, const ChatSession* native_snapshot)
{
	using namespace uam::query_handler_async;
	const std::string source_chat_id = payload.value("chatId", "");
	const std::string native_session_id = uam::strings::Trim(payload.value("nativeSessionId", ""));
	const bool select_chat = payload.value("selectChat", true);
	if (native_session_id.empty())
	{
		return AsyncFailure(400, "A native session id is required.");
	}

	ChatSession* source_chat = ChatDomainService().FindChatById(m_app, source_chat_id);
	if (source_chat == nullptr)
	{
		return AsyncFailure(404, "Source chat not found: " + source_chat_id);
	}
	const ProviderProfile& provider = ProviderResolutionService().ProviderForChatOrDefault(m_app, *source_chat);
	if (!ProviderRuntime::UsesNativeOverlayHistory(provider) && !ProviderRuntime::UsesLocalHistory(provider))
	{
		return AsyncFailure(409, "This provider does not expose a native or local session history path.");
	}

	const std::string source_provider_id = uam::provider_ids::NormalizeCliProviderAliasOrSelf(provider.id);
	if (!uam::paths::IsControllerLocalWorkspace(*source_chat) &&
	    source_provider_id != uam::provider_ids::kCodexCli && source_provider_id != uam::provider_ids::kOpenCodeCli)
		return AsyncFailure(409, "Remote child history is unavailable for this provider.");


	const std::string previous_selected_chat_id = ChatDomainService().SelectedChatId(m_app);
	const auto previous_resolved_native_session = m_app.resolved_native_sessions_by_chat_id.find(source_chat_id);
	const bool had_previous_resolved_native_session = previous_resolved_native_session != m_app.resolved_native_sessions_by_chat_id.end();
	const std::string previous_resolved_native_session_id = had_previous_resolved_native_session ? previous_resolved_native_session->second : std::string{};
	ChatSession* target_chat = ChatHistorySyncService().FindInMemoryNativeSessionChatForOpen(m_app, *source_chat, provider, native_session_id, false);
	std::optional<ChatSession> previous_target_chat;
	if (target_chat != nullptr) previous_target_chat = *target_chat;

	bool inserted_chat = false;
	std::string target_chat_id;
	bool had_previous_target_resolved_native_session = false;
	std::string previous_target_resolved_native_session_id;
	if (target_chat == nullptr)
	{
		target_chat = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(m_app, *source_chat, provider, native_session_id, false, native_snapshot);
		if (target_chat == nullptr)
		{
			return AsyncFailure(404, "Sub-agent chat not found in native history.");
		}
		inserted_chat = true;
		target_chat_id = target_chat->id;
	}
	else
	{
		target_chat_id = target_chat->id;
		const auto previous_target_resolved_native_session = m_app.resolved_native_sessions_by_chat_id.find(target_chat_id);
		had_previous_target_resolved_native_session = previous_target_resolved_native_session != m_app.resolved_native_sessions_by_chat_id.end();
		previous_target_resolved_native_session_id = had_previous_target_resolved_native_session ? previous_target_resolved_native_session->second : std::string{};
	}

	if (!inserted_chat && (payload.value("refreshHistory", !select_chat) || native_snapshot != nullptr))
	{
		target_chat = ChatHistorySyncService().FindOrImportNativeSessionChatForOpen(m_app, *source_chat, provider, native_session_id, false, native_snapshot);
		if (target_chat == nullptr)
		{
			return AsyncFailure(404, "Sub-agent chat history is unavailable.");
		}
		if (target_chat->id != target_chat_id)
		{
			// A conflicting resolved link can yield a new import instead of the hinted chat.
			inserted_chat = true;
			target_chat_id = target_chat->id;
			previous_target_chat.reset();
		}
	}
	m_app.resolved_native_sessions_by_chat_id[target_chat->id] = native_session_id;
	if (target_chat->provider_id.empty())
	{
		target_chat->provider_id = source_provider_id;
	}
	if (select_chat)
	{
		ChatDomainService().SelectChatById(m_app, target_chat_id);
	}

	ChatSession* selected_chat = select_chat ? ChatDomainService().SelectedChat(m_app) : target_chat;
	if (selected_chat == nullptr)
	{
		if (inserted_chat)
		{
			ChatHistorySyncService().RollbackOpenNativeSessionChatImport(m_app, target_chat_id, previous_selected_chat_id);
		}
		if (!inserted_chat)
		{
			*target_chat = std::move(*previous_target_chat);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, target_chat_id, had_previous_target_resolved_native_session, previous_target_resolved_native_session_id);
		}
		ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, source_chat_id, had_previous_resolved_native_session, previous_resolved_native_session_id);
		return AsyncFailure(404, "Selected chat no longer exists.");
	}

	if (select_chat)
	{
		selected_chat->last_opened_at = uam::time::TimestampNow();
	}
	if (!PersistenceCoordinator().SaveSettings(m_app))
	{
		if (select_chat)
		{
			ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		}
		if (!inserted_chat)
		{
			*selected_chat = std::move(*previous_target_chat);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, target_chat_id, had_previous_target_resolved_native_session, previous_target_resolved_native_session_id);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, source_chat_id, had_previous_resolved_native_session, previous_resolved_native_session_id);
		}
		if (inserted_chat)
		{
			ChatHistorySyncService().RollbackOpenNativeSessionChatImport(m_app, target_chat_id, previous_selected_chat_id);
		}
		return AsyncFailure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist selected chat."));
	}

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *selected_chat, "", ""))
	{
		if (select_chat)
		{
			ChatDomainService().SelectChatById(m_app, previous_selected_chat_id);
		}
		if (!inserted_chat)
		{
			*selected_chat = std::move(*previous_target_chat);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, target_chat_id, had_previous_target_resolved_native_session, previous_target_resolved_native_session_id);
			ChatHistorySyncService().RestoreOpenNativeSessionResolvedMapping(m_app, source_chat_id, had_previous_resolved_native_session, previous_resolved_native_session_id);
		}
		if (inserted_chat)
		{
			ChatHistorySyncService().RollbackOpenNativeSessionChatImport(m_app, target_chat_id, previous_selected_chat_id);
		}
		(void)PersistenceCoordinator().SaveSettings(m_app);
		return AsyncFailure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist selected chat."));
	}

	ChatDomainService().SortChatsByRecent(m_app.chats);
	ChatDomainService().SelectChatById(m_app, select_chat ? target_chat_id : previous_selected_chat_id);
	uam::PushStateUpdateIfChanged(browser, m_app);
	return AsyncSuccess(nlohmann::json{{"chatId", target_chat_id}});
}

void UamQueryHandler::HandleSetChatModel(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = uam::nlohmann_json::TrimmedStringValueOr(payload, "chatId", "");
	const std::string model_id = uam::nlohmann_json::TrimmedStringValueOr(payload, "modelId", "");
	const std::string model_role = uam::nlohmann_json::TrimmedStringValueOr(payload, "modelRole", "worker");

	if (!IsAllowedModelId(model_id) || (model_role != "worker" && model_role != "reviewer"))
	{
		cb->Failure(400, "Unsupported ACP model selection.");
		return;
	}

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (!ChatProviderAvailableOrFail(m_app, *chat, cb))
	{
		return;
	}

	if (model_role == "reviewer")
	{
		if (chat->reviewer_model_id == model_id)
		{
			cb->Success(nlohmann::json{{"reviewerModelId", model_id}}.dump());
			return;
		}
		const std::string previous_model_id = chat->reviewer_model_id;
		const std::string previous_updated_at = chat->updated_at;
		chat->reviewer_model_id = model_id;
		chat->updated_at = uam::time::TimestampNow();
		if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat reviewer model updated.", "Chat reviewer model changed in UI, but failed to save."))
		{
			chat->reviewer_model_id = previous_model_id;
			chat->updated_at = previous_updated_at;
			cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat reviewer model."));
			return;
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"reviewerModelId", model_id}}.dump());
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	const bool defer_live_update = session != nullptr && AcpSessionBlocksModelChange(*session);
	const bool is_copilot = uam::provider_ids::IsCliProviderAliasOf(chat->provider_id, uam::provider_ids::kCopilotCli);
	const uam::AcpModelState* selected_model = nullptr;
	if (session != nullptr)
	{
		const auto found = std::ranges::find_if(session->available_models, [&model_id](const uam::AcpModelState& model) { return model.id == model_id; });
		if (found != session->available_models.end())
		{
			selected_model = &*found;
		}
	}
	std::string reasoning_effort = chat->reasoning_effort;
	std::string service_tier = chat->service_tier;
	bool service_tier_explicit = chat->service_tier_explicit;
	if (selected_model != nullptr && !reasoning_effort.empty() && !selected_model->supported_reasoning_efforts.empty() &&
	    !uam::ranges::Contains(selected_model->supported_reasoning_efforts, reasoning_effort))
	{
		reasoning_effort = uam::ranges::Contains(selected_model->supported_reasoning_efforts, selected_model->default_reasoning_effort)
		                       ? selected_model->default_reasoning_effort
		                       : selected_model->supported_reasoning_efforts.front();
	}
	if (selected_model != nullptr && !service_tier.empty() && !uam::ranges::Contains(selected_model->additional_speed_tiers, service_tier))
	{
		service_tier.clear();
		service_tier_explicit = true;
	}
	const bool copilot_effort_changed = is_copilot && chat->reasoning_effort != reasoning_effort;
	if (defer_live_update && copilot_effort_changed)
	{
		cb->Failure(409, "Wait for the active Copilot request to finish before changing to a model with a different effort.");
		return;
	}

	if (chat->model_id == model_id && chat->reasoning_effort == reasoning_effort && chat->service_tier == service_tier && chat->service_tier_explicit == service_tier_explicit)
	{
		if (!defer_live_update && session != nullptr && session->running && !model_id.empty() && session->current_model_id != model_id)
		{
			std::string acp_error;
			if (!uam::SetAcpSessionModel(m_app, chat->id, model_id, &acp_error))
			{
				cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP model."));
				return;
			}
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"modelId", model_id}, {"reasoningEffort", reasoning_effort}, {"serviceTier", service_tier}, {"serviceTierExplicit", service_tier_explicit}}.dump());
		return;
	}
	if (!uam::EnsureAcpStopProgress(m_app, chat->id))
	{
		cb->Failure(409, "The runtime is stopping; retry shortly.");
		return;
	}
	if (!defer_live_update && session != nullptr && session->running &&
	    (model_id.empty() || copilot_effort_changed) &&
	    !uam::StopAcpSession(m_app, chat->id))
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Failure(409, FailureDetailOrFallback(m_app.status_line, "The runtime is stopping; retry shortly."));
		return;
	}

	const std::string previous_model_id = chat->model_id;
	const std::string previous_reasoning_effort = chat->reasoning_effort;
	const std::string previous_service_tier = chat->service_tier;
	const bool previous_service_tier_explicit = chat->service_tier_explicit;
	const std::string previous_updated_at = chat->updated_at;
	chat->model_id = model_id;
	chat->reasoning_effort = reasoning_effort;
	chat->service_tier = service_tier;
	chat->service_tier_explicit = service_tier_explicit;
	chat->updated_at = uam::time::TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat model updated.", "Chat model changed in UI, but failed to save."))
	{
		chat->model_id = previous_model_id;
		chat->reasoning_effort = previous_reasoning_effort;
		chat->service_tier = previous_service_tier;
		chat->service_tier_explicit = previous_service_tier_explicit;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat model."));
		return;
	}

	if (!defer_live_update && session != nullptr && session->running)
	{
		std::string acp_error;
		const bool live_updated = model_id.empty() || copilot_effort_changed ? uam::StopAcpSession(m_app, chat->id) : uam::SetAcpSessionModel(m_app, chat->id, model_id, &acp_error, previous_model_id);
		if (!live_updated)
		{
			chat->model_id = previous_model_id;
			chat->reasoning_effort = previous_reasoning_effort;
			chat->service_tier = previous_service_tier;
			chat->service_tier_explicit = previous_service_tier_explicit;
			chat->updated_at = previous_updated_at;
			(void)ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat model reverted.", "Chat model changed in UI, but failed to revert.");
			cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP model."));
			return;
		}
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"modelId", model_id}, {"reasoningEffort", reasoning_effort}, {"serviceTier", service_tier}, {"serviceTierExplicit", service_tier_explicit}}.dump());
}

void UamQueryHandler::HandleSetChatCodexOptions(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string requested_reasoning_effort = payload.value("reasoningEffort", "");
	std::string service_tier = uam::codex::NormalizeServiceTier(payload.value("serviceTier", ""));

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (!ChatProviderAvailableOrFail(m_app, *chat, cb))
	{
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	const bool is_codex = uam::provider_ids::IsCliProviderAliasOf(chat->provider_id, uam::provider_ids::kCodexCli);
	const bool is_copilot = uam::provider_ids::IsCliProviderAliasOf(chat->provider_id, uam::provider_ids::kCopilotCli);
	bool service_tier_explicit = is_codex && (payload.contains("serviceTierExplicit") ? payload.value("serviceTierExplicit", false) : payload.contains("serviceTier"));
	std::string reasoning_effort = uam::provider_chat_defaults::NormalizeReasoningEffort(chat->provider_id, requested_reasoning_effort);
	if (session != nullptr && uam::AcpSessionHasBlockingRuntimeWork(*session))
	{
		cb->Failure(409, "Wait for the active provider request to finish before changing model options.");
		return;
	}
	const uam::AcpModelState* selected_model = nullptr;
	if (session != nullptr)
	{
		const std::string selected_model_id = chat->model_id.empty() ? session->current_model_id : chat->model_id;
		for (const uam::AcpModelState& model : session->available_models)
		{
			if (model.id == selected_model_id)
			{
				selected_model = &model;
				break;
			}
		}
	}
	if (!is_codex && !is_copilot && (selected_model == nullptr || selected_model->supported_reasoning_efforts.empty()))
	{
		cb->Failure(409, "This provider model does not expose reasoning-effort options.");
		return;
	}
	if (selected_model != nullptr && !reasoning_effort.empty() && !selected_model->supported_reasoning_efforts.empty())
	{
		const auto requested = std::ranges::find(selected_model->supported_reasoning_efforts, reasoning_effort);
		if (requested == selected_model->supported_reasoning_efforts.end())
		{
			reasoning_effort = uam::ranges::Contains(selected_model->supported_reasoning_efforts, selected_model->default_reasoning_effort)
			                       ? selected_model->default_reasoning_effort
			                       : selected_model->supported_reasoning_efforts.front();
		}
	}
	if (!is_codex)
	{
		service_tier.clear();
		service_tier_explicit = false;
	}
	if (is_codex && selected_model != nullptr && !service_tier.empty() && !uam::ranges::Contains(selected_model->additional_speed_tiers, service_tier))
	{
		cb->Failure(409, "The selected model does not support that speed tier.");
		return;
	}
	if (chat->reasoning_effort == reasoning_effort && chat->service_tier == service_tier && chat->service_tier_explicit == service_tier_explicit)
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"reasoningEffort", reasoning_effort}, {"serviceTier", service_tier}, {"serviceTierExplicit", service_tier_explicit}}.dump());
		return;
	}
	if (!uam::EnsureAcpStopProgress(m_app, chat->id))
	{
		cb->Failure(409, "The runtime is stopping; retry shortly.");
		return;
	}
	if (is_copilot && session != nullptr && session->running &&
	    !uam::StopAcpSession(m_app, chat->id))
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Failure(409, FailureDetailOrFallback(m_app.status_line, "The runtime is stopping; retry shortly."));
		return;
	}

	const std::string previous_reasoning_effort = chat->reasoning_effort;
	const std::string previous_service_tier = chat->service_tier;
	const bool previous_service_tier_explicit = chat->service_tier_explicit;
	const std::string previous_updated_at = chat->updated_at;
	chat->reasoning_effort = reasoning_effort;
	chat->service_tier = service_tier;
	chat->service_tier_explicit = service_tier_explicit;
	chat->updated_at = uam::time::TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat model options updated.", "Chat model options changed in UI, but failed to save."))
	{
		chat->reasoning_effort = previous_reasoning_effort;
		chat->service_tier = previous_service_tier;
		chat->service_tier_explicit = previous_service_tier_explicit;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist Codex chat options."));
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"reasoningEffort", reasoning_effort}, {"serviceTier", service_tier}, {"serviceTierExplicit", service_tier_explicit}}.dump());
}

void UamQueryHandler::HandleSetChatProvider(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = ProviderSwitchChatIdFromPayload(payload);
	const std::string provider_id = ProviderSwitchProviderIdFromPayload(payload);

	switch (uam::SwitchChatProvider(m_app, chat_id, provider_id))
	{
	case uam::ChatProviderSwitchResult::UnsupportedProvider:
		cb->Failure(400, "Unsupported provider: " + provider_id);
		return;
	case uam::ChatProviderSwitchResult::ChatNotFound:
		cb->Failure(404, "Chat not found: " + chat_id);
		return;
	case uam::ChatProviderSwitchResult::ActiveRuntime:
		cb->Failure(409, "Cannot change provider while a runtime turn or input request is active.");
		return;
	case uam::ChatProviderSwitchResult::RuntimeStopping:
		cb->Failure(409, FailureDetailOrFallback(m_app.status_line, "The previous runtime is stopping; retry shortly."));
		return;
	case uam::ChatProviderSwitchResult::SaveFailed:
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat provider."));
		return;
	case uam::ChatProviderSwitchResult::Changed:
	case uam::ChatProviderSwitchResult::Unchanged:
		break;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatApprovalMode(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string mode_id = uam::approval_modes::NormalizeIncomingApprovalModeId(payload.value("modeId", ""));

	if (!uam::approval_modes::IsAgentMode(mode_id))
	{
		cb->Failure(400, "Unsupported ACP mode: " + mode_id);
		return;
	}

	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (!ChatProviderAvailableOrFail(m_app, *chat, cb))
	{
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	const bool defer_live_update = session != nullptr && AcpSessionBlocksModelChange(*session);
	const std::string effective_mode_id = uam::approval_modes::EffectiveProviderMode(mode_id, chat->command_safety_tier);

	if (chat->approval_mode == mode_id)
	{
		if (!defer_live_update && session != nullptr && session->running && session->current_mode_id != effective_mode_id)
		{
			std::string acp_error;
			if (!uam::SetAcpSessionMode(m_app, chat->id, effective_mode_id, &acp_error))
			{
				cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP mode."));
				return;
			}
		}
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"approvalMode", chat->approval_mode}, {"currentModeId", session == nullptr ? effective_mode_id : session->current_mode_id}}.dump());
		return;
	}

	const std::string previous_mode_id = chat->approval_mode;
	const std::string previous_updated_at = chat->updated_at;
	chat->approval_mode = mode_id;
	chat->updated_at = uam::time::TimestampNow();

	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat mode updated.", "Chat mode changed in UI, but failed to save."))
	{
		chat->approval_mode = previous_mode_id;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat mode."));
		return;
	}

	if (!defer_live_update && session != nullptr && session->running)
	{
		std::string acp_error;
		if (!uam::SetAcpSessionMode(m_app, chat->id, effective_mode_id, &acp_error, previous_mode_id))
		{
			chat->approval_mode = previous_mode_id;
			chat->updated_at = previous_updated_at;
			(void)ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat mode reverted.", "Chat mode changed in UI, but failed to revert.");
			cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP mode."));
			return;
		}
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"approvalMode", chat->approval_mode}, {"currentModeId", session == nullptr ? effective_mode_id : session->current_mode_id}}.dump());
}

void UamQueryHandler::HandleSetChatUamAgent(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string agent_id = uam::strings::NonEmptyOrFallback(uam::strings::Trim(payload.value("agentId", "")), "build");
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	if (!chat->agent_run_id.empty())
	{
		cb->Failure(403, "Managed child chats cannot change their assigned UAM agent.");
		return;
	}
	const uam::AgentDefinitionCatalog agents = uam::AgentDefinitionService::Load(
	    m_app.data_root, uam::paths::ResolveControllerWorkspaceRootPath(m_app, *chat));
	const auto selected = std::ranges::find(agents.definitions, agent_id, &uam::AgentDefinition::id);
	if (selected == agents.definitions.end() || (selected->mode != "primary" && selected->mode != "both"))
	{
		cb->Failure(400, "UAM agent is unavailable for primary chat use: " + agent_id);
		return;
	}
	if (chat->uam_agent_id == agent_id)
	{
		cb->Success(nlohmann::json{{"uamAgentId", agent_id}}.dump());
		return;
	}
	const std::string previous_agent_id = chat->uam_agent_id;
	const std::string previous_updated_at = chat->updated_at;
	chat->uam_agent_id = agent_id;
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "UAM agent updated.", "UAM agent changed in UI, but failed to save."))
	{
		chat->uam_agent_id = previous_agent_id;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist the UAM agent."));
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"uamAgentId", chat->uam_agent_id}}.dump());
}

void UamQueryHandler::HandleSetChatUamControlEnabled(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::optional<bool> enabled = uam::nlohmann_json::BoolFieldStrict(payload, "enabled");
	if (!enabled.has_value())
	{
		cb->Failure(400, "Agent goal control requires an explicit boolean enabled value.");
		return;
	}
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	if (!chat->agent_run_id.empty())
	{
		cb->Failure(403, "Managed child chats cannot change UAM Control authority.");
		return;
	}
	const ProviderProfile* provider = ProviderResolutionService().ProviderForChat(m_app, *chat);
	if (*enabled && (provider == nullptr ||
	                 !uam::UamControlService::SupportsStructuredProtocol(provider->structured_protocol)))
	{
		cb->Failure(409, "This provider's structured protocol cannot attach UAM Control. Supported providers are Gemini CLI, OpenCode, and GitHub Copilot CLI.");
		return;
	}
	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	if (*enabled && session != nullptr && session->running && session->uam_control_capability_id.empty())
	{
		cb->Failure(409, "Stop the current provider session before enabling agent goal control.");
		return;
	}
	if (chat->uam_control_enabled == *enabled)
	{
		cb->Success(nlohmann::json{{"enabled", *enabled}}.dump());
		return;
	}

	const bool previous = chat->uam_control_enabled;
	const std::string previous_updated_at = chat->updated_at;
	chat->uam_control_enabled = *enabled;
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Agent goal control updated.",
	                                                "Agent goal control changed in UI, but failed to save."))
	{
		chat->uam_control_enabled = previous;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist agent goal control."));
		return;
	}
	if (!*enabled)
	{
		if (session != nullptr)
		{
			uam::UamControlService::RevokeForSession(m_app, *session);
		}
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"enabled", chat->uam_control_enabled}}.dump());
}

void UamQueryHandler::HandleListUamAgents(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;

	const uam::AgentDefinitionCatalog catalog = uam::AgentDefinitionService::Load(
	    m_app.data_root, uam::paths::ResolveControllerWorkspaceRootPath(m_app, *chat));
	nlohmann::json agents = nlohmann::json::array();
	for (const uam::AgentDefinition& agent : catalog.definitions)
	{
		if (agent.mode != "primary" && agent.mode != "both") continue;
		agents.push_back({
		    {"id", agent.id},
		    {"description", agent.description},
		    {"builtIn", agent.built_in},
		});
	}
	cb->Success(nlohmann::json{{"agents", std::move(agents)}, {"errors", catalog.errors}}.dump());
}

namespace
{
	nlohmann::json SerializeProviderAgentImportPreview(const uam::ProviderAgentImportPreview& preview)
	{
		return {
		    {"providerId", preview.provider_id},
		    {"sourcePath", uam::paths::Utf8PathString(preview.source_path)},
		    {"suggestedId", preview.suggested_id},
		    {"description", preview.description},
		    {"mode", preview.mode},
		    {"securityFields", preview.security_fields},
		    {"ignoredFields", preview.ignored_fields},
		    {"error", preview.error},
		    {"supported", preview.supported},
		};
	}
}

void UamQueryHandler::HandleBrowseProviderAgentImport(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::filesystem::path initial_path = PlatformServicesFactory::Instance().path_service.ExpandLeadingTildePath(payload.value("currentValue", ""));
	std::string selected_path;
	std::string error;
	if (!PlatformServicesFactory::Instance().file_dialog_service.BrowsePath(PlatformPathBrowseTarget::File, initial_path, &selected_path, &error))
	{
		if (!error.empty()) cb->Failure(500, error);
		else cb->Success(nlohmann::json{{"selectedPath", ""}}.dump());
		return;
	}
	cb->Success(nlohmann::json{{"selectedPath", selected_path}}.dump());
}

void UamQueryHandler::HandlePreviewProviderAgentImport(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const auto preview = uam::AgentDefinitionService::PreviewProviderAgentImport(
	    payload.value("providerId", ""), uam::paths::PathFromUtf8(payload.value("sourcePath", "")));
	cb->Success(SerializeProviderAgentImportPreview(preview).dump());
}

void UamQueryHandler::HandleImportProviderAgent(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Open a workspace chat before importing an agent.");
	if (chat == nullptr) return;

	uam::ProviderAgentImportRequest request;
	request.provider_id = payload.value("providerId", "");
	request.source_path = uam::paths::PathFromUtf8(payload.value("sourcePath", ""));
	request.canonical_id = payload.value("canonicalId", "");
	request.workspace_access = payload.value("workspaceAccess", "");
	request.workspace_scope = payload.value("workspaceScope", false);
	request.acknowledge_ignored_fields = payload.value("acknowledgeIgnoredFields", false);

	uam::AgentDefinition imported;
	std::string error;
	if (!uam::AgentDefinitionService::ImportProviderAgent(
	        m_app.data_root, uam::paths::ResolveControllerWorkspaceRootPath(m_app, *chat),
	        request, &imported, &error))
	{
		cb->Failure(400, FailureDetailOrFallback(error, "Provider agent import failed."));
		return;
	}
	cb->Success(nlohmann::json{{"id", imported.id}, {"description", imported.description}, {"mode", imported.mode}}.dump());
}

void UamQueryHandler::HandleGetManagedAgentTranscript(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string root_chat_id = payload.value("chatId", "");
	const std::string transcript_chat_id = payload.value("transcriptChatId", "");
	const ChatSession* root_chat = ChatDomainService().FindChatById(m_app, root_chat_id);
	if (root_chat == nullptr || !root_chat->agent_run_id.empty())
	{
		cb->Failure(404, "Managed agent transcript parent chat is unavailable.");
		return;
	}
	const auto run = std::ranges::find_if(m_app.agent_runs, [&](const AgentRun& candidate) {
		return candidate.root_chat_id == root_chat_id && candidate.transcript_chat_id == transcript_chat_id;
	});
	if (run == m_app.agent_runs.end())
	{
		cb->Failure(404, "Managed agent transcript is outside this chat or no longer exists.");
		return;
	}
	std::optional<ChatSession> loaded_transcript;
	ChatSession* transcript = ChatDomainService().FindChatById(m_app, transcript_chat_id);
	if (transcript == nullptr)
	{
		std::string warning;
		loaded_transcript = ChatRepository::LoadLocalChat(m_app.data_root, transcript_chat_id, true, &warning);
		if (!loaded_transcript.has_value())
		{
			cb->Failure(404, FailureDetailOrFallback(warning, "Managed agent transcript is unavailable."));
			return;
		}
		transcript = &*loaded_transcript;
	}
	if (transcript->agent_run_id != run->id)
	{
		cb->Failure(404, "Managed agent transcript is unavailable.");
		return;
	}
	std::string hydrate_warning;
	if (!transcript->messages_loaded &&
	    !ChatRepository::HydrateChatMessages(m_app.data_root, *transcript, &hydrate_warning))
	{
		cb->Failure(500, FailureDetailOrFallback(hydrate_warning, "Failed to load the managed agent transcript."));
		return;
	}
	const nlohmann::json serialized = uam::StateSerializer::SerializeSession(*transcript);
	cb->Success(nlohmann::json{
	    {"runId", run->id}, {"agentId", run->agent_id}, {"status", run->status},
	    {"providerId", run->provider_id}, {"executionCapability", run->execution_capability},
	    {"resumedFromRunId", run->resumed_from_run_id},
	    {"title", transcript->title}, {"messages", serialized.value("messages", nlohmann::json::array())},
	}.dump());
}

void UamQueryHandler::HandleResumeAgentRun(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	std::string new_run_id;
	std::string error;
	if (!uam::AgentRunScheduler::ResumeInterrupted(
	        m_app, payload.value("runId", ""), &new_run_id, &error))
	{
		cb->Failure(409, FailureDetailOrFallback(error, "Managed run could not be resumed."));
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"runId", new_run_id}, {"status", "queued"}}.dump());
}

void UamQueryHandler::HandleSetChatCommandSafetyTier(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string raw_requested = uam::strings::ToLowerAscii(uam::strings::Trim(payload.value("commandSafetyTier", "")));
	if (raw_requested != "off" && raw_requested != "acceptedits" && raw_requested != "aireview" && raw_requested != "yolo")
	{
		cb->Failure(400, "Permission mode must be default, accept edits, AI Review, or YOLO.");
		return;
	}
	const std::string requested = raw_requested == "acceptedits" ? uam::approval_modes::kAcceptEditsApprovalMode : raw_requested == "aireview" ? "aiReview" : raw_requested;
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	const auto auto_approve_pending = [&]()
	{
		uam::AcpSessionState* pending_session = uam::FindAcpSessionForChat(m_app, chat->id);
		if (pending_session == nullptr || pending_session->pending_permission.request_id_json.empty()) return;
		std::string acp_error;
		if (uam::TryAutoApprovePendingAcpPermission(m_app, chat->id, &acp_error))
		{
			pending_session->last_error.clear();
		}
		else if (!acp_error.empty())
		{
			pending_session->last_error = acp_error;
		}
	};
	if (chat->command_safety_tier == requested)
	{
		auto_approve_pending();
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"commandSafetyTier", chat->command_safety_tier}}.dump());
		return;
	}

	const std::string previous = chat->command_safety_tier;
	const ChatSession previous_chat = *chat;
	const std::string previous_updated_at = chat->updated_at;
	chat->command_safety_tier = requested;
	const std::string requested_effective_mode = uam::approval_modes::EffectiveProviderMode(chat->approval_mode, requested);
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Command safety tier updated.", "Command safety tier changed in UI, but failed to save."))
	{
		chat->command_safety_tier = previous;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist command safety tier."));
		return;
	}
	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id);
	if (session != nullptr && session->running && !AcpSessionBlocksModelChange(*session) &&
	    CommandSafetyTierNeedsLiveUpdate(previous_chat, *chat))
	{
		std::string acp_error;
		if (!uam::SetAcpSessionMode(m_app, chat->id, requested_effective_mode, &acp_error, std::nullopt, previous))
		{
			chat->command_safety_tier = previous;
			chat->updated_at = previous_updated_at;
			(void)ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat permissions reverted.", "Chat permissions changed in UI, but failed to revert.");
			cb->Failure(409, FailureDetailOrFallback(acp_error, "Failed to update live ACP permissions."));
			return;
		}
	}
	auto_approve_pending();

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"commandSafetyTier", chat->command_safety_tier}}.dump());
}

void UamQueryHandler::HandleSetChatComputerUseEnabled(CefRefPtr<CefBrowser> browser,
    const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const bool enabled = payload.value("enabled", false);
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	if (enabled && !uam::computer_use::AvailableForChat(*chat))
	{
		cb->Failure(409, "Computer Use is disabled for remote execution hosts.");
		return;
	}
	const bool uses_uam_backend = uam::computer_use::UsesUamBackend(*chat);

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat_id);
	if (!uam::EnsureAcpStopProgress(m_app, chat_id))
	{
		cb->Failure(409, "The runtime is stopping; retry shortly.");
		return;
	}
	if (enabled && !uses_uam_backend && session != nullptr && uam::AcpSessionHasActiveTurn(*session))
	{
		cb->Failure(409, "Activate provider computer use after the current structured turn finishes.");
		return;
	}
	if (session != nullptr && session->running && (!enabled || !uses_uam_backend) && !uam::StopAcpSession(m_app, chat_id))
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Failure(409, FailureDetailOrFallback(m_app.status_line, "The runtime is stopping; retry shortly."));
		return;
	}

	if (!enabled)
	{
		// Turning computer use off is a safety boundary: terminate the provider first,
		// even if the cooperative control file cannot be updated.
		chat->computer_use_enabled = false;
		if (uses_uam_backend)
			(void)uam::ComputerUseService::SetControlState(m_app, chat_id, "stopped");
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	chat->computer_use_enabled = true;
	if (uses_uam_backend)
	{
		std::string error;
		if (!uam::ComputerUseService::SetControlState(m_app, chat_id, "armed", &error))
		{
			chat->computer_use_enabled = false;
			cb->Failure(500, FailureDetailOrFallback(error, "Failed to arm Computer Use."));
			return;
		}
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatComputerUseBackend(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string requested = uam::strings::ToLowerAscii(uam::strings::Trim(payload.value("backend", "")));
	if (requested != uam::computer_use::kBackendAuto && requested != uam::computer_use::kBackendProvider && requested != uam::computer_use::kBackendUam)
	{
		cb->Failure(400, "Computer-use backend must be auto, provider, or uam.");
		return;
	}
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	if (requested == uam::computer_use::kBackendProvider && !uam::computer_use::ProviderBackendAvailable(chat->provider_id))
	{
		cb->Failure(409, "Provider computer use is unavailable in this structured session.");
		return;
	}
	if (chat->computer_use_enabled)
	{
		cb->Failure(409, "Turn off computer use before changing its control method.");
		return;
	}
	if (uam::computer_use::BackendPreference(chat->computer_use_backend) == requested)
	{
		cb->Success("{}");
		return;
	}

	uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat_id);
	if (!uam::EnsureAcpStopProgress(m_app, chat_id))
	{
		cb->Failure(409, "The runtime is stopping; retry shortly.");
		return;
	}
	if (session != nullptr && uam::AcpSessionHasActiveTurn(*session))
	{
		cb->Failure(409, "Change the computer-use backend after the current turn finishes.");
		return;
	}
	if (session != nullptr && session->running && !uam::StopAcpSession(m_app, chat_id))
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Failure(409, FailureDetailOrFallback(m_app.status_line, "The runtime is stopping; retry shortly."));
		return;
	}

	const ChatSession previous = *chat;
	const std::string previous_updated_at = chat->updated_at;
	chat->computer_use_backend = requested;
	chat->computer_use_target_kind = "window";
	chat->computer_use_target_id.clear();
	chat->computer_use_target_process_id.clear();
	chat->computer_use_target_title.clear();
	chat->computer_use_target_input_mode.clear();
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Computer-use control method updated.", "Computer-use control method changed in UI, but failed to save."))
	{
		*chat = previous;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist the computer-use control method."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetComputerUseControl(CefRefPtr<CefBrowser> browser,
    const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::string requested = uam::strings::ToLowerAscii(uam::strings::Trim(payload.value("state", "")));
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr) return;
	if (!uam::computer_use::UsesUamBackend(*chat))
	{
		cb->Failure(409, "Use the provider's controls for provider computer use.");
		return;
	}
	if (requested == "running" && !chat->computer_use_enabled)
	{
		cb->Failure(409, "Enable computer use before resuming it.");
		return;
	}

	std::string error;
	if (!uam::ComputerUseService::SetControlState(m_app, chat_id, requested, &error))
	{
		cb->Failure(400, error);
		return;
	}
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleGetComputerUseSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	nlohmann::json allowed = nlohmann::json::array();
	for (const auto& rule : m_app.settings.computer_use_allowed_applications)
		allowed.push_back({{"identityKind", rule.identity_kind}, {"identity", rule.identity}});
	cb->Success(nlohmann::json{{"allowlistEnabled", m_app.settings.computer_use_allowlist_enabled}, {"allowedApplications", allowed}}.dump());
}

void UamQueryHandler::HandleSetComputerUseSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if (payload.contains("allowlistEnabled") && !payload["allowlistEnabled"].is_boolean()) { cb->Failure(400, "allowlistEnabled must be a boolean."); return; }
	AppSettings previous = m_app.settings;
	const bool enabled = payload.value("allowlistEnabled", m_app.settings.computer_use_allowlist_enabled);
	std::vector<ComputerUseApplicationRule> allowed = m_app.settings.computer_use_allowed_applications;
	if (payload.contains("allowedApplications"))
	{
		if (!payload["allowedApplications"].is_array()) { cb->Failure(400, "allowedApplications must be an array."); return; }
		allowed.clear();
		for (const auto& item : payload["allowedApplications"])
		{
			if (!item.is_object() || !item.contains("identityKind") || !item["identityKind"].is_string() || !item.contains("identity") || !item["identity"].is_string()) { m_app.settings = previous; cb->Failure(400, "Each allowed application needs identityKind and identity."); return; }
			allowed.push_back({item.value("identityKind", ""), item.value("identity", "")});
		}
	}
	m_app.settings.computer_use_allowlist_enabled = enabled;
	m_app.settings.computer_use_allowed_applications = std::move(allowed);
	if (!PersistenceCoordinator().SaveSettings(m_app)) { m_app.settings = previous; cb->Failure(500, "Failed to persist Computer Use Settings."); return; }
	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success(nlohmann::json{{"allowlistEnabled", m_app.settings.computer_use_allowlist_enabled}}.dump());
}

void UamQueryHandler::HandleListComputerUseApplications(CefRefPtr<CefBrowser>, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb, [] {
		const auto result = RunComputerUseSettingsHelper("list");
		if (!result.ok) return result;
		const nlohmann::json applications = nlohmann::json::parse(result.body, nullptr, false);
		return uam::query_handler_async::AsyncSuccess(nlohmann::json{{"applications", applications}, {"error", ""}});
	});
}

void UamQueryHandler::HandleOpenComputerUseSystemSettings(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string permission = uam::strings::ToLowerAscii(uam::strings::Trim(payload.value("permission", "")));
	if (permission != "screenrecording" && permission != "accessibility") { cb->Failure(400, "permission must be screenRecording or accessibility."); return; }
	const std::string helper_permission = permission == "screenrecording" ? "screenRecording" : "accessibility";
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb, [helper_permission] {
		return RunComputerUseSettingsHelper("open", helper_permission);
	});
}

void UamQueryHandler::HandleCheckComputerUsePermissions(CefRefPtr<CefBrowser>, const nlohmann::json&, CefRefPtr<Callback> cb)
{
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb, [] {
		return RunComputerUseSettingsHelper("check");
	});
}

void UamQueryHandler::HandleRequestComputerUsePermission(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string permission = uam::strings::ToLowerAscii(uam::strings::Trim(payload.value("permission", "")));
	if (permission != "screenrecording" && permission != "accessibility") { cb->Failure(400, "permission must be screenRecording or accessibility."); return; }
	const std::string helper_permission = permission == "screenrecording" ? "screenRecording" : "accessibility";
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb, [helper_permission] {
		return RunComputerUseSettingsHelper("request", helper_permission);
	});
}

void UamQueryHandler::HandleSetChatMemoryEnabled(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const bool enabled = payload.value("enabled", true);
	const std::string requested_level = uam::memory_levels::Normalize(uam::nlohmann_json::TrimmedStringValueOr(payload, "memoryLevel", ""), enabled);
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}

	if (chat->memory_level == requested_level && chat->memory_enabled == uam::memory_levels::IsEnabled(requested_level))
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const bool previous = chat->memory_enabled;
	const std::string previous_level = chat->memory_level;
	const std::string previous_updated_at = chat->updated_at;
	chat->memory_level = requested_level;
	chat->memory_enabled = uam::memory_levels::IsEnabled(requested_level);
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Chat memory setting updated.", "Chat memory setting changed in UI, but failed to save."))
	{
		chat->memory_enabled = previous;
		chat->memory_level = previous_level;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist chat memory setting."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}

void UamQueryHandler::HandleSetChatSmallModelMode(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	const std::optional<bool> enabled = uam::nlohmann_json::BoolFieldStrict(payload, "enabled");
	if (!enabled)
	{
		cb->Failure(400, "Small-model mode must be a boolean.");
		return;
	}
	ChatSession* chat = FindChatOrFail(m_app, chat_id, cb, "Chat not found: " + chat_id);
	if (chat == nullptr)
	{
		return;
	}
	if (chat->small_model_mode == *enabled)
	{
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success("{}");
		return;
	}

	const bool previous = chat->small_model_mode;
	const std::string previous_updated_at = chat->updated_at;
	chat->small_model_mode = *enabled;
	chat->updated_at = uam::time::TimestampNow();
	if (!ChatHistorySyncService().SaveChatWithStatus(m_app, *chat, "Small-model workflow updated.", "Small-model workflow changed in UI, but failed to save."))
	{
		chat->small_model_mode = previous;
		chat->updated_at = previous_updated_at;
		cb->Failure(500, FailureDetailOrFallback(m_app.status_line, "Failed to persist small-model workflow."));
		return;
	}

	uam::PushStateUpdateIfChanged(browser, m_app);
	cb->Success("{}");
}
