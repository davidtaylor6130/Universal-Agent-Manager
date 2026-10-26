#include "app/worktree_launch_preparation_service.h"
#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"
#include "cef/branch_operation_dispatch.h"
#include "cef/cef_push.h"
#include "app/chat_domain_service.h"
#include "app/git_worktree_service.h"
#include "app/svn_branch_service.h"
#include "common/paths/path_utils.h"
#include "common/config/execution_host_config.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include <chrono>
#include <optional>
#include "common/runtime/terminal/terminal_identity.h"

namespace
{
	/// Uses the existing UI poller to observe owned process exit before resuming the original request.
	class PendingBranchStopTask final : public CefTask
	{
	  public:
		PendingBranchStopTask(std::weak_ptr<void> lifetime, std::function<bool(bool)> resume)
		    : m_lifetime(std::move(lifetime)), m_resume(std::move(resume)), m_deadline(std::chrono::steady_clock::now() + std::chrono::seconds(5)) {}
		void Execute() override
		{
			if (m_lifetime.expired() || !m_resume) return;
			const bool expired = std::chrono::steady_clock::now() >= m_deadline;
			if (m_resume(expired)) m_resume = {};
			else if (!CefPostDelayedTask(TID_UI, this, 50)) { (void)m_resume(true); m_resume = {}; }
		}
	  private:
		std::weak_ptr<void> m_lifetime;
		std::function<bool(bool)> m_resume;
		std::chrono::steady_clock::time_point m_deadline;
		IMPLEMENT_REFCOUNTING(PendingBranchStopTask);
	};

	bool BranchRuntimeSettled(const uam::AppState& app, const std::string& chat_id)
	{
		if (uam::AcpStopInProgress(app, chat_id) || uam::IsWorktreeLaunchPreparationPending(app, chat_id) ||
		    app.git_port_requests.contains(chat_id) || app.pending_chat_save_at_by_chat_id.contains(chat_id) ||
		    app.pending_chat_save_tasks.contains(chat_id)) return false;
		for (const std::unique_ptr<uam::AcpSessionState>& session : app.acp_sessions)
			if (session && session->chat_id == chat_id && (session->running || session->local_stop_pending || session->remote_stop_pending)) return false;
		for (const std::unique_ptr<uam::CliTerminalState>& terminal : app.cli_terminals)
			if (terminal && terminal->running && uam::CliTerminalMatchesChatId(*terminal, chat_id)) return false;
		return true;
	}

	struct BranchRequest
	{
		std::filesystem::path root;
		AppSettings settings;
		std::optional<ExecutionHost> host;
		std::vector<ChatFolder> folders;
		std::vector<ProviderProfile> providers;
		std::vector<ChatSession> references;
		ChatSession chat;
		ChatDomainService::BranchHistoryIdentity identity;
		std::shared_ptr<uam::WorkspaceLeaseGate::Lease> lease;
	};

	std::shared_ptr<BranchRequest> CaptureBranchRequest(uam::AppState& app, const ChatSession& chat)
	{
		std::shared_ptr<BranchRequest> request = std::make_shared<BranchRequest>();
		request->root = app.data_root;
		request->settings = app.settings;
		if (const ExecutionHost* host = uam::execution_hosts::Find(app.settings.execution_hosts, chat.execution_host_id.empty() ? "local" : chat.execution_host_id)) request->host = *host;
		request->folders = app.folders;
		request->providers = app.provider_profiles;
		for (const ChatSession& candidate : app.chats)
		{
			ChatSession reference;
			reference.folder_id = candidate.folder_id;
			reference.id = candidate.id; reference.execution_host_id = candidate.execution_host_id;
			reference.workspace_directory = candidate.workspace_directory;
			reference.workspace_source_directory = candidate.workspace_source_directory;
			reference.workspace_worktree_directory = candidate.workspace_worktree_directory;
			request->references.push_back(std::move(reference));
		}
		request->chat = chat;
		request->identity = ChatDomainService().CaptureBranchHistoryIdentity(chat);
		return request;
	}

	uam::AppState BranchSnapshot(const BranchRequest& request)
	{
		uam::AppState snapshot;
		snapshot.data_root = request.root;
		snapshot.settings = request.settings;
		snapshot.folders = request.folders;
		snapshot.provider_profiles = request.providers;
		snapshot.chats = request.references;
		return snapshot;
	}

	uam::SvnBranchScope SvnScope(const BranchRequest& request)
	{
		return uam::BuildSvnBranchScope(request.settings.svn_enabled, request.chat, request.folders, request.references);
	}

	nlohmann::json SvnContextJson(const uam::SvnBranchContext& c)
	{
		return {{"directory", c.directory}, {"url", c.url}, {"repository", c.repository}, {"uuid", c.uuid}, {"revision", c.revision}, {"parent", c.parent}, {"repositoryRevision", c.repository_revision}};
	}

	uam::SvnBranchContext ParseSvnContext(const nlohmann::json& payload)
	{
		const auto& c = payload.at("svnContext");
		return {c.at("directory").get<std::string>(), c.at("url").get<std::string>(), c.at("repository").get<std::string>(), c.at("uuid").get<std::string>(), c.at("revision").get<std::string>(), c.at("parent").get<std::string>(), c.at("repositoryRevision").get<std::string>()};
	}

	bool BranchRequestCurrent(const uam::AppState& app, const BranchRequest& request)
	{
		const ChatSession* chat = ChatDomainService().FindChatById(app, request.chat.id);
		const ChatFolder* folder = ChatDomainService().FindFolderById(app, request.chat.folder_id);
		const ChatFolder* original_folder = nullptr;
		for (const ChatFolder& candidate : request.folders) if (candidate.id == request.chat.folder_id) original_folder = &candidate;
		const ExecutionHost* host = uam::execution_hosts::Find(app.settings.execution_hosts, request.chat.execution_host_id.empty() ? "local" : request.chat.execution_host_id);
		return host && request.host && *host == *request.host && app.data_root == request.root && chat && chat->metadata_incarnation == request.chat.metadata_incarnation &&
		    chat->folder_id == request.chat.folder_id && chat->imported_read_only == request.chat.imported_read_only &&
		    chat->agent_run_id == request.chat.agent_run_id && chat->goal_owner_chat_id == request.chat.goal_owner_chat_id &&
		    chat->temporary_parent_chat_id == request.chat.temporary_parent_chat_id &&
		    ChatDomainService().MatchesBranchHistoryIdentity(*chat, request.identity) && folder && original_folder &&
		    folder->directory == original_folder->directory && folder->execution_host_id == original_folder->execution_host_id &&
		    app.settings.git_enabled == request.settings.git_enabled && app.settings.svn_enabled == request.settings.svn_enabled;
	}
}

void UamQueryHandler::HandleListChatBranches(CefRefPtr<CefBrowser>, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ChatSession* chat = uam::query_handler_internal::FindPayloadChatOrFail(m_app, payload, cb);
	if (!chat) return;
	const std::shared_ptr<BranchRequest> request = CaptureBranchRequest(m_app, *chat);
	const bool svn_requested = payload.value("vcsType", std::string{}) == "svn";
	const std::string parent = payload.value("svnParent", std::string{});
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb, [request, svn_requested, parent]()
	{
		if (svn_requested)
		{
			const auto result = uam::DiscoverSvnBranches(SvnScope(*request), parent, uam::CreateSvnBranchRunner());
			if (!result.ok) return uam::query_handler_async::AsyncFailure(400, result.error);
			nlohmann::json branches = nlohmann::json::array();
			for (const auto& branch : result.branches) branches.push_back({{"name", branch.name}, {"path", branch.path}, {"current", branch.current}});
			return uam::query_handler_async::AsyncSuccess({{"vcsType", "svn"}, {"branches", branches}, {"svnContext", SvnContextJson(result.context)}});
		}
		uam::AppState snapshot = BranchSnapshot(*request);
		std::vector<uam::GitChatBranch> branches;
		bool svn = false;
		std::string error;
		if (!uam::GitWorktreeService().ListChatBranches(snapshot, request->chat, branches, svn, error))
			return uam::query_handler_async::AsyncFailure(400, error);
		nlohmann::json values = nlohmann::json::array();
		for (const uam::GitChatBranch& branch : branches) values.push_back({{"name", branch.name}, {"current", branch.current}});
		return uam::query_handler_async::AsyncSuccess({{"branches", values}, {"vcsType", svn ? "svn" : "git"}});
	}, [this, request](uam::query_handler_async::AsyncCefResult& result)
	{
		if (!BranchRequestCurrent(m_app, *request)) result = uam::query_handler_async::AsyncFailure(409, "The chat workspace changed. Open the branch picker again.");
	});
}


void UamQueryHandler::HandleSwitchChatBranch(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ChatSession* chat = uam::query_handler_internal::FindPayloadChatOrFail(m_app, payload, cb);
	if (!chat) return;
	if (m_workspaceDeletionPending || m_app.worktree_operation_chat_ids.contains(chat->id))
	{ cb->Failure(409, "Wait for the workspace operation to finish before switching branches."); return; }
	if (!payload.contains("branch") || !payload["branch"].is_string() ||
	    (payload.contains("create") && !payload["create"].is_boolean()))
	{ cb->Failure(400, "Choose a valid local Git branch."); return; }
	const std::string branch = payload["branch"].get<std::string>();
	const bool create = payload.value("create", false);
	const bool svn = payload.value("vcsType", std::string{}) == "svn";
	const auto svn_context = svn ? ParseSvnContext(payload) : uam::SvnBranchContext{};
	if (svn && create) { cb->Failure(400, "SVN selection does not create repository folders."); return; }
	const std::shared_ptr<BranchRequest> request = CaptureBranchRequest(m_app, *chat);
	uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb, [request, branch, create, svn, svn_context]()
	{
		if (svn)
		{
			const auto result = uam::DiscoverSvnBranches(SvnScope(*request), svn_context.parent, uam::CreateSvnBranchRunner());
			if (!result.ok || result.context != svn_context) return uam::query_handler_async::AsyncFailure(409, result.ok ? "The SVN context changed. Discover the folders again." : result.error);
			return uam::query_handler_async::AsyncSuccess(nlohmann::json::object());
		}
		uam::AppState snapshot = BranchSnapshot(*request);
		std::string error;
		if (!uam::GitWorktreeService().ValidateChatBranchSelection(snapshot, request->chat, branch, create, error))
			return uam::query_handler_async::AsyncFailure(400, error);
		return uam::query_handler_async::AsyncSuccess(nlohmann::json::object());
	}, [this, browser, payload, cb, request](uam::query_handler_async::AsyncCefResult& result)
	{
		if (!result.ok) return;
		if (!BranchRequestCurrent(m_app, *request))
		{ result = uam::query_handler_async::AsyncFailure(409, "The chat changed before branch selection."); return; }
		ContinueSwitchChatBranch(browser, payload, cb);
		result.callback_deferred = true;
	});
}
void UamQueryHandler::ContinueSwitchChatBranch(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	ChatSession* chat = uam::query_handler_internal::FindPayloadChatOrFail(m_app, payload, cb);
	if (!chat) return;
	if (m_workspaceDeletionPending || m_app.worktree_operation_chat_ids.contains(chat->id))
	{ cb->Failure(409, "Wait for the workspace operation to finish before switching branches."); return; }
	if (!payload.contains("branch") || !payload["branch"].is_string() ||
	    (payload.contains("create") && !payload["create"].is_boolean()))
	{ cb->Failure(400, "Choose a valid local Git branch."); return; }
	if (chat->acp_queue_held || !chat->acp_queued_prompts.empty())
	{ cb->Failure(409, "Finish or resolve queued input before switching branches."); return; }
	bool stop_requested = false;
	for (const std::unique_ptr<uam::AcpSessionState>& session : m_app.acp_sessions)
	{
		if (!session || session->chat_id != chat->id || !session->running) continue;
		if (!uam::AcpSessionCanIdleShutdown(*session, *chat) || session->queue_held || !session->queued_user_prompts.empty() ||
		    session->pending_agent_input || session->pending_prompt_delivery || session->pending_queued_history ||
		    !session->pending_steer_requests.empty() || !session->pending_local_steer_abandon.empty())
		{ cb->Failure(409, "Finish the active turn, queued input and child tasks before switching branches."); return; }
		stop_requested = true;
	}
	for (const std::unique_ptr<uam::CliTerminalState>& terminal : m_app.cli_terminals)
		if (terminal && terminal->running && uam::CliTerminalMatchesChatId(*terminal, chat->id))
		{ cb->Failure(409, "Stop the chat terminal before switching branches."); return; }
	if (stop_requested)
	{
		const std::shared_ptr<BranchRequest> waiting = CaptureBranchRequest(m_app, *chat);
		if (uam::AcpSessionState* session = uam::FindAcpSessionForChat(m_app, chat->id); session && session->running)
			(void)uam::StopAcpSession(m_app, chat->id, uam::AcpStopPurpose::WorkspaceChange, true);
		CefRefPtr<PendingBranchStopTask> task = new PendingBranchStopTask(m_asyncLifetime,
		    [this, browser, payload, cb, waiting](bool expired)
		    {
			if (expired || !BranchRequestCurrent(m_app, *waiting))
			{ cb->Failure(409, expired ? "The idle runtime is still stopping. Retry after it exits." : "The chat changed while its idle runtime was stopping. Open the branch picker again."); return true; }
			if (!BranchRuntimeSettled(m_app, waiting->chat.id)) return false;
			ContinueSwitchChatBranch(browser, payload, cb);
			return true;
		    });
		if (!CefPostDelayedTask(TID_UI, task, 50)) cb->Failure(409, "The runtime stop queue is unavailable. Retry after it exits.");
		return;
	}
	if (!BranchRuntimeSettled(m_app, chat->id) || m_workspaceDeletionPending)
	{ cb->Failure(409, "Stop the chat runtime and wait for history to save before switching branches."); return; }
	const std::string branch = payload["branch"].get<std::string>();
	const bool create = payload.value("create", false);
	const bool svn = payload.value("vcsType", std::string{}) == "svn";
	const auto svn_context = svn ? ParseSvnContext(payload) : uam::SvnBranchContext{};
	if (svn && create) { cb->Failure(400, "SVN selection does not create repository folders."); return; }
	const std::shared_ptr<BranchRequest> request = CaptureBranchRequest(m_app, *chat);
	// Unlike the shared mutation lease, exclusivity prevents adding a worktree user during checkout.
	request->lease = m_app.worktree_retention_gate.TryAcquireCleanup();
	if (!request->lease) { cb->Failure(409, "A workspace operation is finishing. Try again shortly."); return; }
	m_app.worktree_operation_chat_ids.insert(chat->id);
	uam::query_handler_async::DispatchBranchOperation(m_app.worktree_operation_chat_ids, request->chat.id, [&]()
	{
		return uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb, [request, svn, svn_context]()
		{
			if (svn)
			{
				const auto result = uam::DiscoverSvnBranches(SvnScope(*request), svn_context.parent, uam::CreateSvnBranchRunner());
				if (!result.ok || result.context != svn_context) return uam::query_handler_async::AsyncFailure(409, result.ok ? "The SVN context changed. Discover the folders again." : result.error);
				return uam::query_handler_async::AsyncSuccess(nlohmann::json::object());
			}
			uam::AppState snapshot = BranchSnapshot(*request);
			std::vector<uam::GitChatBranch> branches;
			bool detected_svn = false;
			std::string error;
			if (!uam::GitWorktreeService().ListChatBranches(snapshot, request->chat, branches, detected_svn, error))
				return uam::query_handler_async::AsyncFailure(400, error);
			if (detected_svn) return uam::query_handler_async::AsyncFailure(400, "Open the SVN folder selector for this working copy.");
			return uam::query_handler_async::AsyncSuccess(nlohmann::json::object());
		}, [this, browser, cb, request, branch, create, svn, svn_context](uam::query_handler_async::AsyncCefResult& result)
		{
			if (!result.ok || !BranchRequestCurrent(m_app, *request) || !BranchRuntimeSettled(m_app, request->chat.id))
			{
				m_app.worktree_operation_chat_ids.erase(request->chat.id);
				if (result.ok) result = uam::query_handler_async::AsyncFailure(409, "The chat changed before branch switching.");
				return;
			}
			uam::query_handler_async::DispatchBranchOperation(m_app.worktree_operation_chat_ids, request->chat.id, [&]()
			{
				return uam::query_handler_async::RunAsyncCefQuery(m_asyncLifetime, cb, [request, branch, create, svn, svn_context]()
				{
					if (svn)
					{
						const auto result = uam::SwitchSvnBranch(SvnScope(*request), svn_context, branch, uam::CreateSvnBranchRunner());
						if (!result.ok) return uam::query_handler_async::AsyncFailure(409, result.error);
						return uam::query_handler_async::AsyncSuccess({{"vcsType", "svn"}, {"branch", result.context.revision}, {"svnContext", SvnContextJson(result.context)}});
					}
					uam::AppState snapshot = BranchSnapshot(*request);
					std::string error;
					if (!uam::GitWorktreeService().SwitchChatBranch(snapshot, request->chat, branch, create, error))
						return uam::query_handler_async::AsyncFailure(409, error.empty() ? "Branch switching could not be saved." : error);
					return uam::query_handler_async::AsyncSuccess({{"branch", request->chat.workspace_branch_name}});
				}, [this, browser, request, svn](uam::query_handler_async::AsyncCefResult& switched)
				{
					m_app.worktree_operation_chat_ids.erase(request->chat.id);
					if (!switched.ok) return;
					ChatSession* current = ChatDomainService().FindChatById(m_app, request->chat.id);
					if (!current || m_app.data_root != request->root || current->metadata_incarnation != request->chat.metadata_incarnation ||
					    !ChatDomainService().MatchesBranchHistoryIdentity(*current, request->identity))
					{ switched = uam::query_handler_async::AsyncFailure(409, "The saved branch changed while the chat workspace was being replaced. Reload this chat."); return; }
					if (!svn)
					{
						current->workspace_branch_name = request->chat.workspace_branch_name;
						current->updated_at = request->chat.updated_at;
					}
					uam::PushStateUpdateIfChanged(browser, m_app);
				});
			});
			result.callback_deferred = true;
		});
	});
}
