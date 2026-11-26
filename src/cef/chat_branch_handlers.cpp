#include "app/worktree_launch_preparation_service.h"
#include "cef/uam_query_handler.h"
#include "cef/uam_query_handler_internal.h"
#include "cef/uam_query_handler_async.h"
#include "cef/branch_operation_dispatch.h"
#include "cef/cef_push.h"
#include "app/chat_domain_service.h"
#include "app/git_worktree_service.h"
#include "app/vcs_provider.h"
#include "common/paths/path_utils.h"
#include "common/config/execution_host_config.h"
#include "common/runtime/acp/acp_session_runtime.h"
#include "common/runtime/acp/acp_session_internal.h"
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

	/// Resumes the launching request after the pending branch exists, or fails it with the branch error.
	class PendingBranchCallback final : public CefMessageRouterBrowserSide::Callback
	{
	  public:
		PendingBranchCallback(CefRefPtr<Callback> callback, std::string branch, std::function<void()> resume)
		    : m_callback(std::move(callback)), m_branch(std::move(branch)), m_resume(std::move(resume)) {}
		void Success(const CefString&) override { Resume(); }
		void Success(const void*, size_t) override { Resume(); }
		void Failure(int code, const CefString& message) override
		{ m_callback->Failure(code, "Branch " + m_branch + " could not be created before the chat started. " + message.ToString()); }

	  private:
		void Resume()
		{
			try { m_resume(); }
			catch (const std::exception&) { m_callback->Failure(400, "The request could not be read after creating the branch."); }
		}
		CefRefPtr<Callback> m_callback;
		std::string m_branch;
		std::function<void()> m_resume;
		IMPLEMENT_REFCOUNTING(PendingBranchCallback);
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

	/// A pending branch is applied on the next launch, so nothing may already be working in the chat.
	bool ChatRuntimeIdleForPendingBranch(const uam::AppState& app, const ChatSession& chat)
	{
		if (chat.acp_queue_held || !chat.acp_queued_prompts.empty() || uam::IsWorktreeLaunchPreparationPending(app, chat.id)) return false;
		for (const std::unique_ptr<uam::AcpSessionState>& session : app.acp_sessions)
			if (session && session->chat_id == chat.id && (session->processing || !session->queued_user_prompts.empty() || session->pending_agent_input))
				return false;
		for (const std::unique_ptr<uam::CliTerminalState>& terminal : app.cli_terminals)
			if (terminal && terminal->running && uam::CliTerminalMatchesChatId(*terminal, chat.id)) return false;
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
		/// Set by the switch worker when the system recorded the branch on `chat`.
		bool chat_updated = false;
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

	/// The picker names the system in `vcsType`; anything else is the default (Git).
	const uam::VcsProvider& BranchProvider(const nlohmann::json& payload)
	{
		return uam::VcsProviderForId(payload.value("vcsType", std::string{}));
	}

	// A located listing's context travels as `svnContext`, the picker's existing wire name.
	nlohmann::json BranchContextJson(const uam::VcsBranchContext& c)
	{
		return {{"directory", c.directory}, {"url", c.url}, {"repository", c.repository}, {"uuid", c.uuid}, {"revision", c.revision}, {"parent", c.parent}, {"repositoryRevision", c.repository_revision}};
	}

	uam::VcsBranchContext ParseBranchContext(const nlohmann::json& payload)
	{
		const auto& c = payload.at("svnContext");
		return {c.at("directory").get<std::string>(), c.at("url").get<std::string>(), c.at("repository").get<std::string>(), c.at("uuid").get<std::string>(), c.at("revision").get<std::string>(), c.at("parent").get<std::string>(), c.at("repositoryRevision").get<std::string>()};
	}

	/// Reads the submitted choice; a located listing's context is required.
	uam::VcsBranchSelection ParseBranchSelection(const uam::VcsProvider& provider, const nlohmann::json& payload)
	{
		uam::VcsBranchSelection selection;
		selection.branch = payload["branch"].get<std::string>();
		selection.create = payload.value("create", false);
		if (provider.BranchesAreLocated()) selection.context = ParseBranchContext(payload);
		return selection;
	}

	/// Creation is offered only by a system that declares it; the reason names the system.
	bool RefuseUndeclaredBranchCreation(const uam::VcsProvider& provider, const uam::VcsBranchSelection& selection, const CefRefPtr<CefMessageRouterBrowserSide::Callback>& cb)
	{
		if (!selection.create || uam::DeclaresCapability(provider, uam::vcs::VcsCapability::BranchCreate)) return false;
		cb->Failure(400, std::string(provider.Declaration().display_name) + " selection does not create repository folders.");
		return true;
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
	const uam::VcsProvider* provider = &BranchProvider(payload);
	const std::string parent = payload.value("svnParent", std::string{});
	uam::query_handler_async::RunAsyncCefQuery(SlowQueryRunner(request->chat), m_asyncLifetime, cb, [request, provider, parent, stop = m_svnBranchDiscoveryStop.get_token()]()
	{
		const uam::AppState snapshot = BranchSnapshot(*request);
		const uam::VcsBranchListing listing = provider->ListBranches(snapshot, request->chat, parent, stop);
		if (!listing.ok) return uam::query_handler_async::AsyncFailure(400, listing.error);
		const bool located = provider->BranchesAreLocated();
		nlohmann::json values = nlohmann::json::array();
		for (const uam::VcsBranch& branch : listing.branches)
		{
			nlohmann::json value = {{"name", branch.name}, {"current", branch.current}};
			if (located) value["path"] = branch.path;
			values.push_back(std::move(value));
		}
		nlohmann::json reply = {{"branches", values}, {"vcsType", uam::VcsTypeToString(listing.type)}};
		if (located) reply["svnContext"] = BranchContextJson(listing.context);
		return uam::query_handler_async::AsyncSuccess(std::move(reply));
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
	const uam::VcsProvider* provider = &BranchProvider(payload);
	const uam::VcsBranchSelection selection = ParseBranchSelection(*provider, payload);
	const std::string& branch = selection.branch;
	const bool create = selection.create;
	if (RefuseUndeclaredBranchCreation(*provider, selection, cb)) return;
	// Only a system that creates branches has a pending first-message branch to withdraw. The pending branch is Git's
	// (pending_git_branch_name, applied through the default provider), the only system that declares BranchCreate today.
	if (uam::DeclaresCapability(*provider, uam::vcs::VcsCapability::BranchCreate) && !create && !chat->pending_git_branch_name.empty() && branch == chat->workspace_branch_name)
	{
		// Withdrawing a pending first-message branch is metadata only, so dirty files cannot trap it.
		chat->pending_git_branch_name.clear();
		uam::acp_detail::ScheduleChatSave(m_app, *chat, 0.0);
		uam::PushStateUpdateIfChanged(browser, m_app);
		cb->Success(nlohmann::json{{"branch", branch}}.dump());
		return;
	}
	const std::shared_ptr<BranchRequest> request = CaptureBranchRequest(m_app, *chat);
	uam::query_handler_async::RunAsyncCefQuery(SlowQueryRunner(request->chat), m_asyncLifetime, cb, [request, provider, selection, stop = m_svnBranchDiscoveryStop.get_token()]()
	{
		const uam::AppState snapshot = BranchSnapshot(*request);
		const uam::VcsBranchOutcome checked = provider->CheckBranchSelection(snapshot, request->chat, selection, stop);
		if (!checked.ok) return uam::query_handler_async::AsyncFailure(checked.conflict ? 409 : 400, checked.error);
		return uam::query_handler_async::AsyncSuccess(nlohmann::json::object());
	}, [this, browser, payload, cb, request, branch, create](uam::query_handler_async::AsyncCefResult& result)
	{
		if (!result.ok) return;
		if (!BranchRequestCurrent(m_app, *request))
		{ result = uam::query_handler_async::AsyncFailure(409, "The chat changed before branch selection."); return; }
		ChatSession* current = ChatDomainService().FindChatById(m_app, request->chat.id);
		// Creation reaches here only for a system that declares it.
		if (create && uam::GitWorktreeService::BranchCreationWaitsForFirstMessage(*current) && ChatRuntimeIdleForPendingBranch(m_app, *current))
		{
			// Avoid branches for chats that never start; the first send creates it from the same HEAD.
			current->pending_git_branch_name = branch;
			uam::acp_detail::ScheduleChatSave(m_app, *current, 0.0);
			uam::PushStateUpdateIfChanged(browser, m_app);
			result = uam::query_handler_async::AsyncSuccess({{"branch", branch}, {"pending", true}});
			return;
		}
		ContinueSwitchChatBranch(browser, payload, cb);
		result.callback_deferred = true;
	});
}
bool UamQueryHandler::ApplyPendingChatBranch(CefRefPtr<CefBrowser> browser, ChatSession& chat, CefRefPtr<Callback> cb, std::function<void()> resume)
{
	if (chat.pending_git_branch_name.empty()) return false;
	if (!uam::GitWorktreeService::BranchCreationWaitsForFirstMessage(chat))
	{
		// The chat already started on its checked-out branch, so a late branch would split its history.
		chat.pending_git_branch_name.clear();
		uam::acp_detail::ScheduleChatSave(m_app, chat, 0.0);
		uam::PushStateUpdateIfChanged(browser, m_app);
		return false;
	}
	// The pending choice may still be saving; switching requires settled history.
	CefRefPtr<PendingBranchStopTask> task = new PendingBranchStopTask(m_asyncLifetime, [this, browser, cb, resume = std::move(resume), id = chat.id](bool expired)
	{
		if (!expired && (m_app.pending_chat_save_at_by_chat_id.contains(id) || m_app.pending_chat_save_tasks.contains(id))) return false;
		const ChatSession* current = ChatDomainService().FindChatById(m_app, id);
		if (!current) { cb->Failure(404, "Chat not found."); return true; }
		// Read the name now, so a withdrawal or rename while saving is honoured.
		if (current->pending_git_branch_name.empty()) { CefRefPtr<Callback>(new PendingBranchCallback(cb, {}, resume))->Success(CefString()); return true; }
		ContinueSwitchChatBranch(browser, {{"chatId", id}, {"branch", current->pending_git_branch_name}, {"create", true}},
		    new PendingBranchCallback(cb, current->pending_git_branch_name, resume));
		return true;
	});
	task->Execute();
	return true;
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
			{ cb->Failure(409, expired ? uam::AcpStopRefusalMessage(m_app, waiting->chat.id, "The idle runtime is still stopping. Retry after it exits.") : "The chat changed while its idle runtime was stopping. Open the branch picker again."); return true; }
			if (!BranchRuntimeSettled(m_app, waiting->chat.id)) return false;
			ContinueSwitchChatBranch(browser, payload, cb);
			return true;
		    });
		if (!CefPostDelayedTask(TID_UI, task, 50)) cb->Failure(409, "The runtime stop queue is unavailable. Retry after it exits.");
		return;
	}
	if (!BranchRuntimeSettled(m_app, chat->id) || m_workspaceDeletionPending)
	{ cb->Failure(409, uam::AcpStopRefusalMessage(m_app, chat->id, "Stop the chat runtime and wait for history to save before switching branches.")); return; }
	const uam::VcsProvider* provider = &BranchProvider(payload);
	const uam::VcsBranchSelection selection = ParseBranchSelection(*provider, payload);
	if (RefuseUndeclaredBranchCreation(*provider, selection, cb)) return;
	const std::shared_ptr<BranchRequest> request = CaptureBranchRequest(m_app, *chat);
	// Unlike the shared mutation lease, exclusivity prevents adding a worktree user during checkout.
	request->lease = m_app.worktree_retention_gate.TryAcquireCleanup();
	if (!request->lease) { cb->Failure(409, "A workspace operation is finishing. Try again shortly."); return; }
	m_app.worktree_operation_chat_ids.insert(chat->id);
	uam::query_handler_async::DispatchBranchOperation(m_app.worktree_operation_chat_ids, request->chat.id, [&]()
	{
		return uam::query_handler_async::RunAsyncCefQuery(SlowQueryRunner(request->chat), m_asyncLifetime, cb, [request, provider, selection, stop = m_svnBranchDiscoveryStop.get_token()]()
		{
			const uam::AppState snapshot = BranchSnapshot(*request);
			const uam::VcsBranchOutcome checked = provider->CheckBranchSwitch(snapshot, request->chat, selection, stop);
			if (!checked.ok) return uam::query_handler_async::AsyncFailure(checked.conflict ? 409 : 400, checked.error);
			return uam::query_handler_async::AsyncSuccess(nlohmann::json::object());
		}, [this, browser, cb, request, provider, selection](uam::query_handler_async::AsyncCefResult& result)
		{
			if (!result.ok || !BranchRequestCurrent(m_app, *request) || !BranchRuntimeSettled(m_app, request->chat.id))
			{
				m_app.worktree_operation_chat_ids.erase(request->chat.id);
				if (result.ok) result = uam::query_handler_async::AsyncFailure(409, "The chat changed before branch switching.");
				return;
			}
			uam::query_handler_async::DispatchBranchOperation(m_app.worktree_operation_chat_ids, request->chat.id, [&]()
			{
				return uam::query_handler_async::RunAsyncCefQuery(SlowQueryRunner(request->chat), m_asyncLifetime, cb, [request, provider, selection, stop = m_svnBranchDiscoveryStop.get_token()]()
				{
					uam::AppState snapshot = BranchSnapshot(*request);
					const uam::VcsBranchOutcome outcome = provider->SwitchBranch(snapshot, request->chat, selection, stop);
					if (!outcome.ok) return uam::query_handler_async::AsyncFailure(409, outcome.error);
					request->chat_updated = outcome.chat_updated;
					nlohmann::json reply = {{"branch", outcome.branch}};
					if (provider->BranchesAreLocated())
					{
						reply["vcsType"] = std::string(provider->Declaration().id);
						reply["svnContext"] = BranchContextJson(outcome.context);
					}
					return uam::query_handler_async::AsyncSuccess(std::move(reply));
				}, [this, browser, request](uam::query_handler_async::AsyncCefResult& switched)
				{
					m_app.worktree_operation_chat_ids.erase(request->chat.id);
					if (!switched.ok) return;
					ChatSession* current = ChatDomainService().FindChatById(m_app, request->chat.id);
					if (!current || m_app.data_root != request->root || current->metadata_incarnation != request->chat.metadata_incarnation ||
					    !ChatDomainService().MatchesBranchHistoryIdentity(*current, request->identity))
					{ switched = uam::query_handler_async::AsyncFailure(409, "The saved branch changed while the chat workspace was being replaced. Reload this chat."); return; }
					if (request->chat_updated)
					{
						current->workspace_branch_name = request->chat.workspace_branch_name;
						current->pending_git_branch_name = request->chat.pending_git_branch_name;
						current->updated_at = request->chat.updated_at;
					}
					uam::PushStateUpdateIfChanged(browser, m_app);
				});
			});
			result.callback_deferred = true;
		});
	});
}
