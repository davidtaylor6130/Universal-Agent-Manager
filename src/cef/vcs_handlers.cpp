#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"

#include "app/chat_domain_service.h"
#include "app/chat_reference_context.h"
#include "app/git_worktree_service.h"
#include "app/runtime_orchestration_services.h"
#include "app/vcs_commit_service.h"
#include "app/workspace_vcs_context_service.h"
#include "app/worktree_launch_preparation_service.h"
#include "cef/cef_push.h"
#include "common/chat/chat_repository.h"
#include "common/config/execution_host_config.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_root.h"
#include "common/paths/workspace_file_reader.h"
#include "remote/runner_client.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/runtime/terminal/terminal_identity.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// VCS and worktree handlers
// ---------------------------------------------------------------------------

using namespace uam::query_handler_internal;
using namespace uam::query_handler_async;

namespace
{
	class PendingWorktreeCreationTask final : public CefTask
	{
	  public:
		PendingWorktreeCreationTask(std::weak_ptr<void> lifetime, std::function<bool(bool)> resume)
		    : m_lifetime(std::move(lifetime)), m_resume(std::move(resume)), m_deadline(std::chrono::steady_clock::now() + std::chrono::minutes(3)) {}
		void Execute() override
		{
			if (m_lifetime.expired() || !m_resume) return;
			CEF_REQUIRE_UI_THREAD();
			try
			{
				if (m_resume(std::chrono::steady_clock::now() >= m_deadline)) m_resume = {};
				else if (!CefPostDelayedTask(TID_UI, this, 50)) { (void)m_resume(true); m_resume = {}; }
			}
			catch (...)
			{
				(void)m_resume(true);
				m_resume = {};
			}
		}
	  private:
		std::weak_ptr<void> m_lifetime;
		std::function<bool(bool)> m_resume;
		std::chrono::steady_clock::time_point m_deadline;
		IMPLEMENT_REFCOUNTING(PendingWorktreeCreationTask);
	};

	std::shared_ptr<void> BeginRemoteVcsOperationLease(uam::AppState& app,
	                                                   const std::string& host_id)
	{
		if (host_id.empty() || host_id == uam::execution_hosts::kLocalHostId) return {};
		std::weak_ptr<void>& lease = app.remote_vcs_operation_leases_by_host_id[host_id];
		std::shared_ptr<void> token = lease.lock();
		if (!token)
		{
			token = std::make_shared<int>(0);
			lease = token;
		}
		return token;
	}

	uam::AppState BuildReadOnlyAppSnapshot(std::filesystem::path data_root, AppSettings settings, std::vector<ChatFolder> folders, std::vector<ProviderProfile> provider_profiles, std::unordered_map<std::string, uam::CliProviderVersionState> provider_versions)
	{
		uam::AppState snapshot;
		snapshot.data_root = std::move(data_root);
		snapshot.settings = std::move(settings);
		snapshot.folders = std::move(folders);
		snapshot.provider_profiles = std::move(provider_profiles);
		snapshot.runtime_cli_versions_by_provider_id = std::move(provider_versions);
		return snapshot;
	}

	struct ReadOnlyAppSnapshotInputs
	{
		std::filesystem::path data_root;
		AppSettings settings;
		std::vector<ChatFolder> folders;
		std::vector<ProviderProfile> provider_profiles;
		std::unordered_map<std::string, uam::CliProviderVersionState> provider_versions;
	};

	ReadOnlyAppSnapshotInputs CaptureReadOnlyAppSnapshotInputs(const uam::AppState& app)
	{
		return {app.data_root, app.settings, app.folders, app.provider_profiles, app.runtime_cli_versions_by_provider_id};
	}

	uam::AppState BuildReadOnlyAppSnapshot(ReadOnlyAppSnapshotInputs inputs)
	{
		return BuildReadOnlyAppSnapshot(std::move(inputs.data_root), std::move(inputs.settings), std::move(inputs.folders), std::move(inputs.provider_profiles), std::move(inputs.provider_versions));
	}

	nlohmann::json SerializeGitWorktreeStatus(const uam::GitWorktreeStatus& status)
	{
		nlohmann::json json;
		json["isGitRepository"] = status.is_git_repository;
		json["isSvnWorkspace"] = status.is_svn_workspace;
		json["managedRepository"] = status.managed_repository;
		json["isolated"] = status.isolated;
		json["sourceDirty"] = status.source_dirty;
		json["worktreeDirty"] = status.worktree_dirty;
		json["worktreeMissing"] = status.worktree_missing;
		json["sourceDirectory"] = status.source_directory;
		json["worktreeDirectory"] = status.worktree_directory;
		json["branchName"] = status.branch_name;
		json["baseRef"] = status.base_ref;
		json["warning"] = status.warning;
		json["error"] = status.error;
		return json;
	}

	nlohmann::json SerializeGitWorktreeResult(const uam::GitWorktreeOperationResult& result)
	{
		nlohmann::json json;
		json["status"] = SerializeGitWorktreeStatus(result.status);
		json["message"] = result.message;
		json["patchPath"] = result.patch_path.empty() ? "" : uam::paths::Utf8PathString(result.patch_path);
		if (!result.port_state.empty())
		{
			json["portState"] = result.port_state;
			json["operationReceipt"] = uam::paths::Utf8PathString(result.operation_receipt);
		}
		return json;
	}

	nlohmann::json SerializeTurnCheckpointResult(const uam::GitTurnCheckpointResult& result)
	{
		return {{"message", result.message}, {"diff", result.diff}, {"checkpointSha", result.checkpoint_sha}, {"parentSha", result.parent_sha}};
	}

	std::string FilePreviewWorkspace(const uam::AppState& app, const ChatSession& chat)
	{
		if (chat.imported_read_only) return {};
		if (!uam::paths::IsControllerLocalWorkspace(chat) && uam::paths::HasGitWorktreeDirectory(chat))
			return chat.workspace_worktree_directory;
		return uam::paths::Utf8PathString(uam::paths::ResolveWorkspaceRootPath(app, chat));
	}

	using WorktreeOperation = uam::GitWorktreeOperationResult (uam::GitWorktreeService::*)(uam::AppState&, ChatSession&) const;

	struct WorktreeAsyncState
	{
		uam::GitWorktreeOperationResult result;
		std::string isolation_kind;
		std::string source_directory;
		std::string base_ref;
		std::string branch_name;
		std::string worktree_directory;
		std::string updated_at;
	};

	void CaptureWorktreeFields(WorktreeAsyncState& target, const ChatSession& source)
	{
		target.isolation_kind = source.workspace_isolation_kind;
		target.source_directory = source.workspace_source_directory;
		target.base_ref = source.workspace_base_ref;
		target.branch_name = source.workspace_branch_name;
		target.worktree_directory = source.workspace_worktree_directory;
		target.updated_at = source.updated_at;
	}

	void ApplyWorktreeFields(ChatSession& target, const WorktreeAsyncState& source)
	{
		target.workspace_isolation_kind = source.isolation_kind;
		target.workspace_source_directory = source.source_directory;
		target.workspace_base_ref = source.base_ref;
		target.workspace_branch_name = source.branch_name;
		target.workspace_worktree_directory = source.worktree_directory;
		target.updated_at = source.updated_at;
	}

	void RunWorktreeOperationAsync(std::weak_ptr<void> lifetime, uam::AppState& app,
	                               CefRefPtr<CefBrowser> browser,
	                               CefRefPtr<CefMessageRouterBrowserSide::Callback> callback,
	                               const std::string& chat_id,
	                               WorktreeOperation operation,
	                               std::string failure_fallback)
	{
		auto state = std::make_shared<WorktreeAsyncState>();
		ReadOnlyAppSnapshotInputs snapshot_inputs = CaptureReadOnlyAppSnapshotInputs(app);
		uam::AppState* live_app = &app;
		app.worktree_operation_chat_ids.insert(chat_id);

		RunAsyncCefQuery(std::move(lifetime),
		    callback,
		    [snapshot_inputs = std::move(snapshot_inputs), state, chat_id, operation, failure_fallback = std::move(failure_fallback)]() mutable
		    {
			    uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(snapshot_inputs));
			    std::string load_warning;
			    std::optional<ChatSession> chat = ChatRepository::LoadLocalChat(
			        snapshot.data_root, chat_id, true, &load_warning);
			    if (!chat)
			    {
				    return AsyncFailure(
				        500,
				        FailureDetailOrFallback(
				            load_warning, "Failed to load chat before its worktree operation."));
			    }
			    snapshot.chats.push_back(std::move(*chat));
			    const uam::GitWorktreeService service;
			    state->result = (service.*operation)(snapshot, snapshot.chats.front());
			    CaptureWorktreeFields(*state, snapshot.chats.front());
			    if (!state->result.ok)
			    {
				    std::string message = FailureDetailOrFallback(state->result.message, failure_fallback);
				    if (!state->result.patch_path.empty())
				    {
					    message += "\nPatch saved at: " + uam::paths::Utf8PathString(state->result.patch_path);
				    }
				    return AsyncFailure(400, std::move(message));
			    }
			    return AsyncSuccess(SerializeGitWorktreeResult(state->result));
		    },
		    [live_app, browser, state, chat_id](AsyncCefResult& result)
		    {
			    live_app->worktree_operation_chat_ids.erase(chat_id);
			    if (!result.ok)
			    {
				    return;
			    }

			    ChatSession* live_chat = ChatDomainService().FindChatById(*live_app, chat_id);
			    if (live_chat == nullptr)
			    {
				    result = AsyncFailure(404, "Chat was removed while its worktree operation was running.");
				    return;
			    }

			    ApplyWorktreeFields(*live_chat, *state);
			    uam::PushStateUpdateIfChanged(browser, *live_app);
		    });
	}

	nlohmann::json SerializeVcsCommitStatus(const uam::VcsCommitStatus& status)
	{
		nlohmann::json types = nlohmann::json::array();
		for (const uam::VcsType type : status.vcs_types)
		{
			types.push_back(uam::VcsTypeToString(type));
		}

		nlohmann::json files = nlohmann::json::array();
		for (const uam::VcsChangedFile& file : status.changed_files)
		{
			files.push_back({
			    {"path", file.path},
			    {"status", file.status},
			    {"staged", file.staged},
			    {"additions", file.additions},
			    {"deletions", file.deletions},
			    {"binary", file.binary},
			    {"contentFingerprint", file.content_fingerprint},
			});
		}

		nlohmann::json json;
		json["available"] = status.available;
		json["vcsTypes"] = std::move(types);
		json["activeVcsType"] = uam::VcsTypeToString(status.active_vcs_type);
		json["workspaceDirectory"] = status.workspace_directory;
		json["branchOrRevision"] = status.branch_or_revision;
		json["changedFiles"] = std::move(files);
		json["lineStatsReady"] = status.line_stats_ready;
		json["warning"] = status.warning;
		json["error"] = status.error;
		return json;
	}

	nlohmann::json SerializeVcsCommitResult(const uam::VcsCommitResult& result)
	{
		return {
		    {"ok", result.ok},
		    {"status", SerializeVcsCommitStatus(result.status)},
		    {"message", result.message},
		    {"error", result.error},
		};
	}

	bool ChatRuntimeBusy(const uam::AppState& app, const std::string& chat_id)
	{
		for (const auto& session : app.acp_sessions)
		{
			if (session == nullptr || session->chat_id != chat_id || !session->running)
			{
				continue;
			}

			if (uam::AcpSessionHasBlockingRuntimeWork(*session))
			{
				return true;
			}
		}
		for (const auto& terminal : app.cli_terminals)
		{
			if (terminal != nullptr && terminal->running && uam::CliTerminalMatchesChatId(*terminal, chat_id))
			{
				return true;
			}
		}
		return false;
	}
} // namespace

void UamQueryHandler::HandleGetChatWorktreeStatus(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}

	const std::string chat_id = chat->id;
	ReadOnlyAppSnapshotInputs snapshot_inputs = CaptureReadOnlyAppSnapshotInputs(m_app);
	RunAsyncCefQuery(m_asyncLifetime,
	    cb,
	    [snapshot_inputs = std::move(snapshot_inputs), chat_id]() mutable
	    {
		    uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(snapshot_inputs));
		    std::string load_warning;
		    std::optional<ChatSession> chat = ChatRepository::LoadLocalChat(
		        snapshot.data_root, chat_id, false, &load_warning);
		    if (!chat)
		    {
			    return AsyncFailure(
			        500,
			        FailureDetailOrFallback(
			            load_warning, "Failed to load chat worktree status."));
		    }
		    return AsyncSuccess(SerializeGitWorktreeStatus(
		        uam::GitWorktreeService().Status(snapshot, *chat)));
	    });
}

void UamQueryHandler::HandleCreateChatWorktree(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}
	if (chat->imported_read_only || !chat->agent_run_id.empty() || !chat->goal_owner_chat_id.empty() || !chat->temporary_parent_chat_id.empty())
	{
		cb->Failure(409, "Workspace isolation can only be changed on an editable primary chat.");
		return;
	}
	if (m_app.pending_chat_save_at_by_chat_id.contains(chat->id))
	{
		cb->Failure(409, "Wait for chat history to finish saving before changing workspace isolation.");
		return;
	}
	if (ChatRuntimeBusy(m_app, chat->id))
	{
		cb->Failure(409, "Stop the chat runtime before changing workspace isolation.");
		return;
	}


	if (!uam::paths::IsControllerLocalWorkspace(*chat))
	{
		uam::AppState* app = &m_app;
		const std::filesystem::path root = m_app.data_root;
		const std::string id = chat->id, created = chat->created_at, provider = chat->provider_id,
		    native = chat->native_session_id, host = chat->execution_host_id;
		const auto incarnation = chat->metadata_incarnation;
		auto resume = [app, browser, cb, root, id, created, provider, native, host, incarnation,
		               expected = std::shared_ptr<uam::WorktreeLaunchPreparationTask>{}](bool expired) mutable
		{
			ChatSession* current = ChatDomainService().FindChatById(*app, id);
			const auto pending = app->worktree_launch_preparations.find(id);
			if (expired || app->data_root != root || !current || current->created_at != created ||
			    current->metadata_incarnation != incarnation || current->provider_id != provider ||
			    current->native_session_id != native || current->execution_host_id != host ||
			    (expected && (pending == app->worktree_launch_preparations.end() || pending->second != expected)))
			{
				if (expected) uam::CancelWorktreeLaunchPreparation(*app, id, expected->cancellation);
				cb->Failure(409, expired ? "Workspace preparation did not finish. Its owned artifacts were preserved; retry to inspect them."
				    : "The chat changed while preparing its workspace. Retry the operation.");
				return true;
			}
			const uam::WorktreeLaunchPreparationResult result = uam::PrepareWorktreeForLaunch(*app, *current, true);
			if (result.state == uam::WorktreeLaunchPreparationState::Pending)
			{
				if (!expected)
					if (const auto found = app->worktree_launch_preparations.find(id); found != app->worktree_launch_preparations.end()) expected = found->second;
				return false;
			}
			if (result.state == uam::WorktreeLaunchPreparationState::Failed)
			{
				cb->Failure(409, FailureDetailOrFallback(result.error, "Failed to prepare the remote worktree."));
				return true;
			}
			uam::GitWorktreeOperationResult operation;
			if (expected && expected->prepared) operation = expected->prepared->operation;
			operation.ok = true;
			operation.status.is_git_repository = true;
			operation.status.isolated = uam::paths::HasGitWorktreeDirectory(*current);
			operation.status.source_directory = current->workspace_source_directory;
			operation.status.worktree_directory = current->workspace_worktree_directory;
			operation.status.branch_name = current->workspace_branch_name;
			operation.status.base_ref = current->workspace_base_ref;
			uam::PushStateUpdateIfChanged(browser, *app);
			cb->Success(SerializeGitWorktreeResult(operation).dump());
			return true;
		};
		if (!resume(false))
		{
			CefRefPtr<PendingWorktreeCreationTask> task = new PendingWorktreeCreationTask(m_asyncLifetime, std::move(resume));
			if (!CefPostDelayedTask(TID_UI, task, 50))
			{
				uam::CancelWorktreeLaunchPreparation(m_app, id);
				cb->Failure(503, "The workspace task queue is unavailable. Retry the operation.");
			}
		}
		return;
	}

	RunWorktreeOperationAsync(m_asyncLifetime, m_app,
	                          browser,
	                          cb,
	                          chat->id,
	                          &uam::GitWorktreeService::CreateForChat,
	                          "Failed to create isolated Git worktree.");
}

void UamQueryHandler::HandleDiscardChatWorktreeChanges(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}
	if (m_app.pending_chat_save_at_by_chat_id.contains(chat->id))
	{
		cb->Failure(409, "Wait for chat history to finish saving before changing workspace isolation.");
		return;
	}
	if (ChatRuntimeBusy(m_app, chat->id))
	{
		cb->Failure(409, "Stop the chat runtime before discarding worktree changes.");
		return;
	}

	RunWorktreeOperationAsync(m_asyncLifetime, m_app,
	                          browser,
	                          cb,
	                          chat->id,
	                          &uam::GitWorktreeService::DiscardChatChanges,
	                          "Failed to discard worktree changes.");
}

void UamQueryHandler::HandlePortChatWorktreeChanges(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}
	if (m_app.pending_chat_save_at_by_chat_id.contains(chat->id))
	{
		cb->Failure(409, "Wait for chat history to finish saving before changing workspace isolation.");
		return;
	}
	if (ChatRuntimeBusy(m_app, chat->id))
	{
		cb->Failure(409, "Stop the chat runtime before porting worktree changes.");
		return;
	}

	if (m_app.settings.git_port_method != GitPortMethod::Patch)
	{
		if (m_app.worktree_operation_chat_ids.contains(chat->id) || m_app.git_port_requests.contains(chat->id))
		{ cb->Failure(409, "Wait for the current workspace operation to finish."); return; }
		const std::shared_ptr<uam::WorkspaceLeaseGate::Lease> lease = m_app.worktree_retention_gate.TryAcquireMutation();
		if (!lease) { cb->Failure(409, "Workspace cleanup is finishing. Retry porting changes."); return; }
		ChatSession captured = uam::CaptureVcsChatContext(*chat);
		captured.created_at = chat->created_at;
		captured.metadata_incarnation = chat->metadata_incarnation;
		captured.workspace_base_ref = chat->workspace_base_ref;
		const std::string chat_id = chat->id;
		const std::filesystem::path root = m_app.data_root;
		const AppSettings settings = m_app.settings;
		const std::shared_ptr<const ChatSession> expected = std::make_shared<const ChatSession>(std::move(captured));
		const std::shared_ptr<uam::AppState::GitPortRequest> request = std::make_shared<uam::AppState::GitPortRequest>();
		request->lease = lease;
		request->handler_lifetime = m_asyncLifetime; request->lifetime_bound = true;
		request->root = root; request->incarnation = expected->metadata_incarnation;
		request->workspace = expected->workspace_directory; request->source = expected->workspace_source_directory;
		request->worktree = expected->workspace_worktree_directory; request->branch = expected->workspace_branch_name; request->base = expected->workspace_base_ref;
		m_app.git_port_requests[chat_id] = request;
		m_app.worktree_operation_chat_ids.insert(chat_id);
		if (!RunAsyncCefQuery(m_asyncLifetime, cb,
		    [expected, root, settings, request]() mutable
		    {
			    if (request->handler_lifetime.expired()) request->cancellation->request_stop();
			    uam::AppState::GitPortRequest::WorkerState queued = uam::AppState::GitPortRequest::WorkerState::Queued;
			    if (!request->worker_state.compare_exchange_strong(queued, uam::AppState::GitPortRequest::WorkerState::Running))
			        return AsyncFailure(409, "Port request was canceled before staging.");
			    struct FinishPortWorker
			    {
				    std::shared_ptr<uam::AppState::GitPortRequest> request;
				    ~FinishPortWorker() { request->worker_state.store(uam::AppState::GitPortRequest::WorkerState::Finished); }
			    } finished{request};
			    if (request->cancellation->stop_requested()) return AsyncFailure(409, "Port request was canceled before staging.");
			    uam::AppState snapshot;
			    snapshot.data_root = root;
			    snapshot.settings = settings;
			    ChatSession context = *expected;
			    const uam::GitWorktreeOperationResult port = uam::GitWorktreeService().PortCommittedChanges(snapshot, context, request->cancellation->get_token());
			    // Conflicts are a durable Git result and must retain their recovery receipt in the response.
			    if (!port.ok && port.port_state.empty()) return AsyncFailure(400, port.message);
			    return AsyncSuccess(SerializeGitWorktreeResult(port));
		    },
		    [this, expected, root, chat_id, request](AsyncCefResult& result)
		    {
			    const std::unordered_map<std::string, std::shared_ptr<uam::AppState::GitPortRequest>>::iterator owner = m_app.git_port_requests.find(chat_id);
			    if ((owner == m_app.git_port_requests.end() && !request->gui_settled) || (owner != m_app.git_port_requests.end() && owner->second != request))
			    { result = AsyncFailure(409, "Port request ownership changed. Inspect the operation receipt."); return; }
			    if (owner != m_app.git_port_requests.end())
			    {
				    request->gui_settled = true;
				    request->lease.reset();
				    m_app.git_port_requests.erase(owner);
				    m_app.worktree_operation_chat_ids.erase(chat_id);
			    }
			    const ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id);
			    if (request->cancellation->stop_requested() || !current || m_app.data_root != root || current->metadata_incarnation != expected->metadata_incarnation ||
			        current->workspace_directory != expected->workspace_directory || current->workspace_source_directory != expected->workspace_source_directory ||
			        current->workspace_worktree_directory != expected->workspace_worktree_directory || current->workspace_branch_name != expected->workspace_branch_name ||
			        current->workspace_base_ref != expected->workspace_base_ref || current->execution_host_id != expected->execution_host_id)
			        result = AsyncFailure(409, "The chat workspace changed during porting. Inspect the source Git workspace and port receipt.");
		    }))
		{
			m_app.git_port_requests.erase(chat_id);
			m_app.worktree_operation_chat_ids.erase(chat_id);
		}
		return;
	}

	RunWorktreeOperationAsync(m_asyncLifetime, m_app,
	                          browser,
	                          cb,
	                          chat->id,
	                          &uam::GitWorktreeService::PortChatChanges,
	                          "Failed to port worktree changes.");
}

void UamQueryHandler::HandlePreviewChatTurnRollback(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	const std::optional<int> message_index = uam::nlohmann_json::IntFieldStrict(payload, "messageIndex");
	if (chat == nullptr) return;
	if (!message_index || *message_index < 0)
	{
		cb->Failure(400, "messageIndex must be a non-negative integer.");
		return;
	}
	const std::string chat_id = chat->id;
	ReadOnlyAppSnapshotInputs snapshot_inputs = CaptureReadOnlyAppSnapshotInputs(m_app);
	RunAsyncCefQuery(m_asyncLifetime, cb, [snapshot_inputs = std::move(snapshot_inputs), chat_id, message_index = *message_index]() mutable {
		uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(snapshot_inputs));
		std::string warning;
		std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(snapshot.data_root, chat_id, true, &warning);
		if (!loaded) return AsyncFailure(500, FailureDetailOrFallback(warning, "Failed to load the checkpointed turn."));
		const uam::GitTurnCheckpointResult result = uam::GitWorktreeService().PreviewTurnRollback(snapshot, *loaded, message_index);
		return result.ok ? AsyncSuccess(SerializeTurnCheckpointResult(result)) : AsyncFailure(409, result.message);
	});
}

void UamQueryHandler::HandleRollbackChatTurn(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	const std::optional<int> message_index = uam::nlohmann_json::IntFieldStrict(payload, "messageIndex");
	if (chat == nullptr) return;
	if (!message_index || *message_index < 0)
	{
		cb->Failure(400, "messageIndex must be a non-negative integer.");
		return;
	}
	if (ChatRuntimeBusy(m_app, chat->id) || m_app.worktree_operation_chat_ids.contains(chat->id))
	{
		cb->Failure(409, "Stop the chat runtime and wait for the current worktree operation before rollback.");
		return;
	}
	if (m_app.pending_chat_save_at_by_chat_id.contains(chat->id))
	{
		cb->Failure(409, "Wait for chat history to finish saving before rollback.");
		return;
	}
	const std::string chat_id = chat->id;
	const int index = *message_index;
	const uam::GitTurnRollbackSnapshot rollback_snapshot =
	    uam::CaptureGitTurnRollbackSnapshot(*chat, index);
	if (chat->messages_loaded && rollback_snapshot.checkpoint_sha.empty())
	{
		cb->Failure(409, "This turn has no rollback checkpoint.");
		return;
	}
	ReadOnlyAppSnapshotInputs snapshot_inputs = CaptureReadOnlyAppSnapshotInputs(m_app);
	const std::shared_ptr<uam::GitTurnCheckpointResult> result_state = std::make_shared<uam::GitTurnCheckpointResult>();
	const std::shared_ptr<uam::GitTurnRollbackSnapshot> loaded_checkpoint_snapshot =
	    std::make_shared<uam::GitTurnRollbackSnapshot>(rollback_snapshot);
	const std::shared_ptr<std::optional<ChatSession>> persisted_after = std::make_shared<std::optional<ChatSession>>();
	m_app.worktree_operation_chat_ids.insert(chat_id);
	uam::AppState* live_app = &m_app;
	RunAsyncCefQuery(m_asyncLifetime, cb,
		[snapshot_inputs = std::move(snapshot_inputs), chat_id, index, rollback_snapshot, loaded_checkpoint_snapshot, result_state, persisted_after]() mutable {
			uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(snapshot_inputs));
			std::string warning;
			std::optional<ChatSession> loaded = ChatRepository::LoadLocalChat(snapshot.data_root, chat_id, true, &warning);
			if (!loaded) return AsyncFailure(500, FailureDetailOrFallback(warning, "Failed to load the checkpointed turn."));
			if (!uam::MatchesGitTurnRollbackSnapshot(*loaded, rollback_snapshot))
				return AsyncFailure(409, "Chat history or workspace changed before rollback. Refresh and try again.");
			*loaded_checkpoint_snapshot = uam::CaptureGitTurnRollbackSnapshot(*loaded, index);
			*result_state = uam::GitWorktreeService().RollbackTurn(snapshot, *loaded, index);
			if (!result_state->ok) return AsyncFailure(409, result_state->message);
			*persisted_after = ChatRepository::LoadLocalChat(snapshot.data_root, chat_id, false, &warning);
			if (!persisted_after->has_value())
				return AsyncFailure(500, FailureDetailOrFallback(warning, "Rollback completed, but saved chat metadata could not be reloaded."));
			return AsyncSuccess(SerializeTurnCheckpointResult(*result_state));
		},
		[live_app, browser, chat_id, loaded_checkpoint_snapshot, result_state, persisted_after](AsyncCefResult& result) {
			live_app->worktree_operation_chat_ids.erase(chat_id);
			if (!result.ok) return;
			ChatSession* live_chat = ChatDomainService().FindChatById(*live_app, chat_id);
			if (live_chat == nullptr || !persisted_after->has_value() ||
			    !uam::ApplyGitTurnRollbackSnapshot(*live_chat, *loaded_checkpoint_snapshot,
			                                      **persisted_after, result_state->checkpoint_sha))
			{
				result = AsyncFailure(409, "Chat changed while its rollback was running. Refresh its history.");
				return;
			}
			uam::PushStateUpdateIfChanged(browser, *live_app);
		});
}

void UamQueryHandler::HandleCaptureChatRepositoryBaseline(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr) return;
	if (ChatRuntimeBusy(m_app, chat->id) || m_app.worktree_operation_chat_ids.contains(chat->id))
	{
		cb->Failure(409, "Stop the chat runtime before saving its repository baseline.");
		return;
	}
	const ChatSession chat_snapshot = *chat;
	ReadOnlyAppSnapshotInputs inputs = CaptureReadOnlyAppSnapshotInputs(m_app);
	const std::string chat_id = chat->id;
	const std::filesystem::path workspace = uam::paths::ResolveWorkspaceRootPath(m_app, *chat);
	m_app.worktree_operation_chat_ids.insert(chat_id);
	uam::AppState* live_app = &m_app;
	if (!RunAsyncCefQuery(m_asyncLifetime, cb,
	    [inputs = std::move(inputs), chat = chat_snapshot]() mutable
	    {
		    uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(inputs));
		    std::string error;
		    if (!uam::VcsCommitService().CaptureChatBaseline(snapshot, chat, &error)) return AsyncFailure(409, FailureDetailOrFallback(error, "Failed to save the chat repository baseline."));
		    return AsyncSuccess({{"saved", true}});
	    },
	    [live_app, chat_id, workspace](AsyncCefResult& result)
	    {
		    live_app->worktree_operation_chat_ids.erase(chat_id);
		    const ChatSession* current = ChatDomainService().FindChatById(*live_app, chat_id);
		    if (current == nullptr || uam::paths::ResolveWorkspaceRootPath(*live_app, *current) != workspace) result = AsyncFailure(409, "The chat workspace changed while its repository baseline was being saved.");
	    }))
	{
		live_app->worktree_operation_chat_ids.erase(chat_id);
	}
}

void UamQueryHandler::HandleGetVcsCommitStatus(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}

	const uam::VcsType requested_type = uam::VcsTypeFromString(payload.value("vcsType", "git"));
	const bool context_only = payload.value("contextOnly", false);
	const bool include_line_stats = payload.value("includeLineStats", true);
	const std::string request_id = payload.value("requestId", "");
	const std::string comparison_ref(uam::nlohmann_json::TrimmedStringViewOrEmpty(payload, "comparisonRef"));
	ChatSession chat_snapshot = uam::CaptureVcsChatContext(*chat);
	const std::filesystem::path expected_workspace = uam::paths::ResolveWorkspaceRootPath(m_app, *chat);
	const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, chat->execution_host_id);
	const std::optional<ExecutionHost> expected_host = host == nullptr ? std::nullopt : std::optional<ExecutionHost>(*host);
	uam::AppState* live_app = &m_app;
	ReadOnlyAppSnapshotInputs snapshot_inputs = CaptureReadOnlyAppSnapshotInputs(m_app);
	const std::shared_ptr<void> remote_vcs_lease = BeginRemoteVcsOperationLease(m_app, chat->execution_host_id);

	RunAsyncCefQuery(m_asyncLifetime, cb,
	                 [snapshot_inputs = std::move(snapshot_inputs), chat = std::move(chat_snapshot), requested_type, include_line_stats, request_id, comparison_ref, context_only, remote_vcs_lease]() mutable
	                 {
		                 uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(snapshot_inputs));
		                 const uam::VcsCommitStatus status = uam::VcsCommitService().Status(snapshot, chat, requested_type, include_line_stats, comparison_ref, context_only);
		                 return AsyncSuccess(WithOptionalRequestId(SerializeVcsCommitStatus(status), request_id));
	                 },
	                 [live_app, chat_id = chat->id, expected_workspace, expected_host](AsyncCefResult& result)
	                 {
		                 const ChatSession* current = ChatDomainService().FindChatById(*live_app, chat_id);
		                 const ExecutionHost* current_host = current == nullptr ? nullptr
		                     : uam::execution_hosts::Find(live_app->settings.execution_hosts, current->execution_host_id);
		                 if (current == nullptr || current_host == nullptr || !expected_host ||
		                     !uam::execution_hosts::SameConnection(*current_host, *expected_host) ||
		                     uam::paths::ResolveWorkspaceRootPath(*live_app, *current) != expected_workspace)
		                     result = AsyncFailure(409, "The chat workspace changed while its repository context was loading.");
	                 });
}

void UamQueryHandler::HandleCaptureChatReferenceContext(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr) return;
	const std::string kind = payload.value("kind", "");
	if ((kind != "branch" && kind != "worktree") || chat->imported_read_only || chat->created_at.empty())
	{ cb->Failure(400, "Choose an editable chat and a branch or worktree reference."); return; }
	if (m_workspaceDeletionPending || m_app.worktree_operation_chat_ids.contains(chat->id))
	{ cb->Failure(409, "The workspace is changing. Try again shortly."); return; }
	if (kind == "worktree" && !uam::paths::HasGitWorktreeDirectory(*chat))
	{ cb->Failure(400, "This chat has no configured Git worktree to reference."); return; }
	if (!uam::BoundedChatReferenceValue(chat->id, 128) || !uam::BoundedChatReferenceValue(chat->created_at, 64) ||
	    !uam::BoundedChatReferenceValue(chat->execution_host_id, 256) ||
	    !uam::BoundedChatReferenceValue(chat->workspace_source_directory, 4096) ||
	    !uam::BoundedChatReferenceValue(chat->workspace_base_ref, 1024))
	{ cb->Failure(400, "This chat context exceeds the reference limits."); return; }
	std::shared_ptr<void> slot = uam::AcquireWorkspaceVcsContextSlot(m_pendingWorkspaceVcsContexts);
	if (!slot) { cb->Failure(429, "Repository context is busy. Try again shortly."); return; }
	ChatSession captured = uam::CaptureVcsChatContext(*chat);
	captured.metadata_incarnation = chat->metadata_incarnation;
	captured.created_at = chat->created_at;
	captured.workspace_base_ref = chat->workspace_base_ref;
	const std::filesystem::path workspace = uam::paths::ResolveWorkspaceRootPath(m_app, *chat);
	const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, chat->execution_host_id);
	if (workspace.empty() || host == nullptr)
	{ cb->Failure(409, "The chat workspace or execution host is unavailable."); return; }
	const ExecutionHost expected_host = *host;
	const std::filesystem::path data_root = m_app.data_root;
	ReadOnlyAppSnapshotInputs inputs = CaptureReadOnlyAppSnapshotInputs(m_app);
	const std::shared_ptr<void> remote_lease = BeginRemoteVcsOperationLease(m_app, chat->execution_host_id);
	RunAsyncCefQuery(m_asyncLifetime, cb,
	    [inputs = std::move(inputs), captured, kind, slot = std::move(slot), remote_lease]() mutable
	    {
		    const std::shared_ptr<void> active_slot = std::move(slot);
		    uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(inputs));
		    const uam::VcsCommitStatus status = uam::VcsCommitService().Status(snapshot, captured, uam::VcsType::Git, false, {}, true);
		    if (!status.available || status.active_vcs_type != uam::VcsType::Git || !status.error.empty() || status.workspace_directory.empty())
		    { return AsyncFailure(409, "Git context is unavailable for this chat workspace."); }
		    if (kind == "branch" && status.branch_or_revision.empty())
		    { return AsyncFailure(409, "This workspace has no named current branch (it may have a detached HEAD)."); }
		    if (!uam::BoundedChatReferenceValue(status.branch_or_revision, 1024) ||
		        !uam::BoundedChatReferenceValue(status.workspace_directory, 4096))
		    { return AsyncFailure(409, "This repository context exceeds the reference limits."); }
		    // This is a historical observation of the configured association, never a checkout/control token.
		    return AsyncSuccess({{"kind", kind}, {"chatId", captured.id}, {"chatCreatedAt", captured.created_at},
		        {"executionHostId", uam::strings::NonEmptyOrFallback(captured.execution_host_id, "local")},
		        {"workspaceDirectory", status.workspace_directory}, {"branchName", status.branch_or_revision},
		        {"worktreeDirectory", kind == "worktree" ? status.workspace_directory : std::string{}},
		        {"sourceDirectory", captured.workspace_source_directory}, {"baseRef", captured.workspace_base_ref},
		        {"capturedAt", uam::time::IsoUtcTimestampNow()}});
	    },
	    [this, captured, workspace, expected_host, data_root](AsyncCefResult& result)
	    {
		    if (!result.ok) return;
		    const ChatSession* current = ChatDomainService().FindChatById(m_app, captured.id);
		    const ExecutionHost* current_host = current == nullptr ? nullptr : uam::execution_hosts::Find(m_app.settings.execution_hosts, current->execution_host_id);
		    if (current == nullptr || current_host == nullptr || m_app.data_root != data_root ||
		        m_workspaceDeletionPending || m_app.worktree_operation_chat_ids.contains(captured.id) ||
		        !uam::SameChatReferenceContext(*current, captured) ||
		        !uam::execution_hosts::SameConnection(*current_host, expected_host) ||
		        uam::paths::ResolveWorkspaceRootPath(m_app, *current) != workspace)
		        result = AsyncFailure(409, "The chat, workspace or execution host changed while capturing context. Try again.");
	    }, TID_FILE_USER_VISIBLE);
}

void UamQueryHandler::HandleGetWorkspaceFilePreview(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if (!payload.contains("chatId") || !payload["chatId"].is_string() || !payload.contains("path") || !payload["path"].is_string())
	{ cb->Failure(400, "Choose a chat and file path."); return; }
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr) return;
	if (m_pendingFilePreviews >= 8)
	{ cb->Failure(429, "File previews are busy. Try again shortly."); return; }
	if (m_workspaceDeletionPending || m_app.worktree_operation_chat_ids.contains(chat->id))
	{ cb->Failure(409, "The workspace is changing. Try again shortly."); return; }
	const std::string path = payload["path"].get<std::string>();
	const std::string workspace = FilePreviewWorkspace(m_app, *chat);
	if (workspace.empty() || path.empty() || path.size() > 16384 || path.find('\0') != std::string::npos)
	{ cb->Failure(400, "Choose a file in this chat's workspace."); return; }
	const std::string host_id = uam::strings::NonEmptyOrFallback(chat->execution_host_id, "local");
	const std::string chat_created_at = chat->created_at;
	const auto chat_incarnation = chat->metadata_incarnation;
	const ExecutionHost* host = uam::execution_hosts::Find(m_app.settings.execution_hosts, host_id);
	const std::optional<ExecutionHost> expected_host = host == nullptr ? std::nullopt : std::optional<ExecutionHost>(*host);
	if (host_id != "local" && (!expected_host || expected_host->transport != "ssh" || expected_host->runner_status != "ready"))
	{ cb->Failure(503, "The chat's remote runner is unavailable. Recheck it in Settings."); return; }
	const std::filesystem::path data_root = m_app.data_root;
	++m_pendingFilePreviews;
	if (!RunAsyncCefQuery(m_asyncLifetime, cb,
	    [workspace, path, host_id, expected_host, chat_id = chat->id, chat_created_at]()
	    {
		    nlohmann::json preview;
		    if (host_id == "local")
		    {
			    const uam::workspace_files::TextFile file = uam::workspace_files::Read(uam::paths::PathFromUtf8(workspace), path);
			    if (!file.ok) return AsyncFailure(400, file.error);
			    preview = {{"text", file.text}, {"path", file.path}};
		    }
		    else
		    {
			    uam::remote::RunnerClient client(PlatformServicesFactory::Instance().process_service,
			        *expected_host);
			    std::string error;
			    if (!client.ReadWorkspaceFile(workspace, path, preview, &error)) return AsyncFailure(503, error);
		    }
		    preview["workspace"] = workspace;
		    preview["executionHostId"] = host_id;
		    preview["chatId"] = chat_id;
		    preview["chatCreatedAt"] = chat_created_at;
		    return AsyncSuccess(std::move(preview));
	    },
	    [this, chat_id = chat->id, chat_created_at, chat_incarnation, workspace, host_id, expected_host, data_root](AsyncCefResult& result)
	    {
		    --m_pendingFilePreviews;
		    if (!result.ok) return;
		    const ChatSession* current = ChatDomainService().FindChatById(m_app, chat_id);
		    const ExecutionHost* current_host = uam::execution_hosts::Find(m_app.settings.execution_hosts, host_id);
		    if (current == nullptr || current->created_at != chat_created_at || current->metadata_incarnation != chat_incarnation ||
		        m_app.data_root != data_root || m_workspaceDeletionPending ||
		        m_app.worktree_operation_chat_ids.contains(chat_id) ||
		        uam::strings::NonEmptyOrFallback(current->execution_host_id, "local") != host_id || FilePreviewWorkspace(m_app, *current) != workspace ||
		        (host_id != "local" && (!current_host || !expected_host || !uam::execution_hosts::SameConnection(*current_host, *expected_host))))
		        result = AsyncFailure(409, "The chat workspace or host changed while loading. Try again.");
	    }, TID_FILE_USER_BLOCKING)) --m_pendingFilePreviews;
}

void UamQueryHandler::HandleGetVcsFileDiff(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}

	const std::string path = payload.value("path", "");
	const uam::VcsType type = uam::VcsTypeFromString(payload.value("vcsType", "git"));
	const std::string request_id = payload.value("requestId", "");
	const std::string comparison_ref(uam::nlohmann_json::TrimmedStringViewOrEmpty(payload, "comparisonRef"));
	ChatSession chat_snapshot = uam::CaptureVcsChatContext(*chat);
	ReadOnlyAppSnapshotInputs snapshot_inputs = CaptureReadOnlyAppSnapshotInputs(m_app);
	const std::shared_ptr<void> remote_vcs_lease = BeginRemoteVcsOperationLease(m_app, chat->execution_host_id);

	RunAsyncCefQuery(m_asyncLifetime, cb,
	                 [snapshot_inputs = std::move(snapshot_inputs), chat = std::move(chat_snapshot), path, type, request_id, comparison_ref, remote_vcs_lease]() mutable
	                 {
		                 uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(snapshot_inputs));
		                 std::string error;
		                 const std::string diff = uam::VcsCommitService().Diff(snapshot, chat, path, type, &error, comparison_ref);
		                 if (!error.empty())
		                 {
			                 return AsyncFailure(400, error);
		                 }
		                 return AsyncSuccess(WithOptionalRequestId(nlohmann::json{{"diff", diff}}, request_id));
	                 });
}

void UamQueryHandler::HandleCommitVcsChanges(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}
	if (ChatRuntimeBusy(m_app, chat->id))
	{
		cb->Failure(409, "Stop the active chat runtime before committing workspace changes.");
		return;
	}

	const std::vector<std::string> files = uam::nlohmann_json::TrimmedStringArrayField(payload, "files");
	const std::string message = payload.value("message", "");
	const uam::VcsType type = uam::VcsTypeFromString(payload.value("vcsType", "git"));
	ChatSession chat_snapshot = uam::CaptureVcsChatContext(*chat);
	ReadOnlyAppSnapshotInputs snapshot_inputs = CaptureReadOnlyAppSnapshotInputs(m_app);
	const std::shared_ptr<void> remote_vcs_lease = BeginRemoteVcsOperationLease(m_app, chat->execution_host_id);
	uam::AppState* live_app = &m_app;
	RunAsyncCefQuery(m_asyncLifetime, cb,
	                 [snapshot_inputs = std::move(snapshot_inputs), chat = std::move(chat_snapshot), type, message, files, remote_vcs_lease]() mutable
	                 {
		                 uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(snapshot_inputs));
		                 const uam::VcsCommitResult result = uam::VcsCommitService().Commit(snapshot, chat, type, message, files);
		                 if (!result.ok) return AsyncFailure(400, FailureDetailOrFallback(result.error, "Failed to commit changes."));
		                 return AsyncSuccess(SerializeVcsCommitResult(result));
	                 },
	                 [live_app, browser](AsyncCefResult& result)
	                 {
		                 if (result.ok) uam::PushStateUpdateIfChanged(browser, *live_app);
	                 });
}

void UamQueryHandler::HandleGenerateVcsCommitMessage(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const ChatSession* chat = FindPayloadChatOrFail(m_app, payload, cb);
	if (chat == nullptr)
	{
		return;
	}

	std::vector<std::string> files = uam::nlohmann_json::TrimmedStringArrayField(payload, "files");
	const uam::VcsType type = uam::VcsTypeFromString(payload.value("vcsType", "git"));
	const std::string request_id = payload.value("requestId", "");
	ChatSession chat_snapshot = uam::CaptureVcsChatContext(*chat);
	ReadOnlyAppSnapshotInputs snapshot_inputs = CaptureReadOnlyAppSnapshotInputs(m_app);
	const std::shared_ptr<void> remote_vcs_lease = BeginRemoteVcsOperationLease(m_app, chat->execution_host_id);

	RunAsyncCefQuery(m_asyncLifetime, cb,
	                 [snapshot_inputs = std::move(snapshot_inputs), chat = std::move(chat_snapshot), type, files = std::move(files), request_id, remote_vcs_lease]() mutable
	                 {
		                 uam::AppState snapshot = BuildReadOnlyAppSnapshot(std::move(snapshot_inputs));
		                 const uam::VcsCommitMessageSuggestion suggestion = uam::VcsCommitService().GenerateMessage(snapshot, chat, type, files);
		                 if (!suggestion.ok)
		                 {
			                 return AsyncFailure(400, FailureDetailOrFallback(suggestion.error, "Failed to generate commit message."));
		                 }

		                 return AsyncSuccess(WithOptionalRequestId(
		                     nlohmann::json{
		                         {"title", suggestion.title},
		                         {"description", suggestion.description},
		                     },
		                     request_id));
	                 });
}

void UamQueryHandler::HandleGetWorkspaceVcsContext(CefRefPtr<CefBrowser> /*browser*/, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string folder_id(uam::nlohmann_json::TrimmedStringViewOrEmpty(payload, "folderId"));
	const ChatFolder* folder = ChatDomainService().FindFolderById(m_app, folder_id);
	if (!folder || folder->directory.empty())
	{ cb->Failure(400, "Choose an existing workspace folder."); return; }
	if (!folder->execution_host_id.empty() && folder->execution_host_id != "local")
	{ cb->Failure(400, "Local repository context requires a local workspace."); return; }
	if (m_workspaceDeletionPending)
	{ cb->Failure(409, "The workspace is changing. Try again shortly."); return; }
	std::shared_ptr<void> slot = uam::AcquireWorkspaceVcsContextSlot(m_pendingWorkspaceVcsContexts);
	if (!slot) { cb->Failure(429, "Repository context is busy. Try again shortly."); return; }
	const std::string directory = folder->directory;
	const std::string host = folder->execution_host_id;
	const std::filesystem::path root = m_app.data_root;
	const bool git_enabled = m_app.settings.git_enabled;
	const bool svn_enabled = m_app.settings.svn_enabled;
	RunAsyncCefQuery(m_asyncLifetime, cb,
	    [directory, git_enabled, svn_enabled, slot = std::move(slot)]() mutable
	    {
		    const std::shared_ptr<void> active_slot = std::move(slot);
		    const uam::WorkspaceVcsContext context = uam::ReadWorkspaceVcsContext(uam::paths::PathFromUtf8(directory), git_enabled, svn_enabled);
		    if (!context.error.empty()) return AsyncFailure(409, context.error);
		    nlohmann::json branches = nlohmann::json::array();
		    for (const uam::WorkspaceGitBranch& branch : context.local_branches)
			    branches.push_back({{"name", branch.name}, {"commit", branch.commit}});
		    return AsyncSuccess({{"vcs", context.vcs}, {"currentBranch", context.current_branch},
		        {"currentCommit", context.current_commit}, {"detached", context.detached},
		        {"localBranches", std::move(branches)}, {"checkoutUrl", context.checkout_url}, {"revision", context.revision}});
	    },
	    [this, folder_id, directory, host, root, git_enabled, svn_enabled](AsyncCefResult& result)
	    {
		    const ChatFolder* current = ChatDomainService().FindFolderById(m_app, folder_id);
		    if (!current || m_app.data_root != root || m_workspaceDeletionPending ||
		        current->directory != directory || current->execution_host_id != host ||
		        m_app.settings.git_enabled != git_enabled || m_app.settings.svn_enabled != svn_enabled)
			    result = AsyncFailure(409, "The workspace changed while its repository context was loading.");
	    });
}
