#include "common/config/build_features.h"
#include "cef/uam_query_handler.h"
#include "cef/uam_bridge_request.h"
#include "cef/uam_cef_security.h"
#include "cef/uam_window_chrome.h"
#include "common/usage/multi_host_history.h"

#include "include/cef_task.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_helpers.h"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace
{
	/// Retains the mutation lease until workers and their completion callbacks release this request.
	class WorkspaceMutationCallback final : public CefMessageRouterBrowserSide::Callback
	{
	  public:
		WorkspaceMutationCallback(CefRefPtr<Callback> callback, std::shared_ptr<uam::WorkspaceLeaseGate::Lease> lease)
		    : m_callback(std::move(callback)), m_lease(std::move(lease)) {}
		void Success(const CefString& response) override { m_callback->Success(response); }
		void Success(const void* data, size_t size) override { m_callback->Success(data, size); }
		void Failure(int code, const CefString& message) override { m_callback->Failure(code, message); }

	  private:
		CefRefPtr<Callback> m_callback;
		std::shared_ptr<uam::WorkspaceLeaseGate::Lease> m_lease;
		IMPLEMENT_REFCOUNTING(WorkspaceMutationCallback);
	};

	/// Closing runs after the bridge acknowledgement, through the window's normal CanClose path.
	class CloseWindowTask final : public CefTask
	{
	  public:
		explicit CloseWindowTask(CefRefPtr<CefWindow> window) : m_window(std::move(window)) {}
		void Execute() override { if (m_window->IsValid()) m_window->Close(); }

	  private:
		CefRefPtr<CefWindow> m_window;
		IMPLEMENT_REFCOUNTING(CloseWindowTask);
	};

	/// Privileged window chrome bridge. Deliberately outside DispatchAction so the companion cannot reach it.
	void HandleWindowChrome(CefRefPtr<CefBrowser> browser, const std::string& action, const nlohmann::json& payload, CefRefPtr<CefMessageRouterBrowserSide::Callback> callback)
	{
		const CefRefPtr<CefBrowserView> view = browser && !browser->IsPopup() ? CefBrowserView::GetForBrowser(browser) : nullptr;
		const CefRefPtr<CefWindow> window = view ? view->GetWindow() : nullptr;
		if (!window)
		{
			callback->Failure(503, "The native window is unavailable.");
			return;
		}
		std::string command;
		if (action == "windowCommand")
		{
			const nlohmann::json::const_iterator value = payload.find("command");
			if (!uam::cef::kCustomWindowChrome)
			{
				callback->Failure(400, "Window controls are native on this platform.");
				return;
			}
			if (value == payload.end() || !value->is_string() || ((command = value->get<std::string>()) != "minimize" && command != "toggleMaximize" && command != "close"))
			{
				callback->Failure(400, "Unknown window command.");
				return;
			}
			if (command == "minimize") window->Minimize();
			else if (command == "toggleMaximize") window->IsMaximized() ? window->Restore() : window->Maximize();
		}
		const nlohmann::json state = {
		    {"platform", uam::cef::kWindowChromePlatform},
		    {"maximized", window->IsMaximized()},
		    {"fullscreen", window->IsFullscreen()},
		};
		callback->Success(state.dump());
		if (command == "close") CefPostTask(TID_UI, new CloseWindowTask(window));
	}
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

UamQueryHandler::UamQueryHandler(uam::AppState& app, std::string trusted_ui_index_url) : m_app(app), m_trustedUiIndexUrl(std::move(trusted_ui_index_url))
{
	m_usageHistory = std::make_shared<uam::usage::MultiHostHistory>(m_app.data_root, m_app.settings.execution_hosts);
	RefreshAllowanceTargets();
}

UamQueryHandler::~UamQueryHandler()
{
	m_asyncLifetime.reset();
	for (const std::pair<const std::string, std::shared_ptr<std::stop_source>>& request : m_titleRequests) request.second->request_stop();
#if UAM_ENABLE_MOBILE_COMPANION
	if (m_companion) m_companion->Stop();
#endif
}

void UamQueryHandler::StartCompanion(CefRefPtr<CefBrowser> browser)
{
#if UAM_ENABLE_MOBILE_COMPANION
	const std::weak_ptr<void> lifetime = m_asyncLifetime;
	m_companion = UamCompanionServer::StartFromEnvironment([this, lifetime, browser](const nlohmann::json& request, CefRefPtr<Callback> callback) {
		if (request.value("action", std::string{}) == "setVisibleChatIds" || request.value("action", std::string{}) == "releaseChatMessages")
		{ callback->Failure(403, "Desktop transcript ownership is required."); return; }
		if (lifetime.expired()) { callback->Failure(503, "UAM is shutting down."); return; }
		if (!DispatchAction(request.at("action").get<std::string>(), browser, request.value("payload", nlohmann::json::object()), callback))
			callback->Failure(404, "Unknown companion action.");
	}, m_app.data_root);
#endif
}

// ---------------------------------------------------------------------------
// CefMessageRouterBrowserSide::Handler
// ---------------------------------------------------------------------------

bool UamQueryHandler::DispatchAction(std::string_view action, CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if ((!UAM_ENABLE_SSH && (action.find("RemoteHost") != std::string_view::npos || action == "listRemoteDirectories")) ||
	    (!UAM_ENABLE_COMPUTER_USE && action.find("ComputerUse") != std::string_view::npos) ||
	    (!UAM_ENABLE_MOBILE_COMPANION && action.find("Companion") != std::string_view::npos))
	{
		cb->Failure(400, "This feature is disabled in this build.");
		return true;
	}
	const std::string chat_id = payload.value("chatId", "");
	if (uam::RequiresWorkspaceMutationLease(action))
	{
		std::shared_ptr<uam::WorkspaceLeaseGate::Lease> lease = m_app.worktree_retention_gate.TryAcquireMutation();
		if (!lease)
		{
			cb->Failure(409, "Workspace cleanup is finishing. Try again shortly.");
			return true;
		}
		cb = new WorkspaceMutationCallback(cb, std::move(lease));
	}
	const bool is_runtime_stop = action == "cancelAcpTurn" || action == "stopAcpSession" || action == "cancelChatTitle";
	const bool is_cached_history_read = action == "getChatMessages" || action == "getToolCallContent" || action == "getAttachmentImagePreview";
	if (m_workspaceDeletionPending && is_cached_history_read && !chat_id.empty() &&
	    m_app.worktree_operation_chat_ids.contains(chat_id))
	{
		cb->Failure(409, "Chat deletion is in progress.");
		return true;
	}
	if ((!is_runtime_stop && !is_cached_history_read && !chat_id.empty() && m_app.worktree_operation_chat_ids.contains(chat_id)) ||
	    (!m_app.worktree_operation_chat_ids.empty() && (action == "deleteFolder" || action == "deleteFolders" || action == "rescanFolderChats")))
	{
		cb->Failure(409, "Wait for the chat worktree operation to finish.");
		return true;
	}

	// Read-only queries and unrelated runtime cancellation stay available during disk work.
	if (m_workspaceDeletionPending && !action.starts_with("get") && !action.starts_with("list") &&
	    action != "selectSession" && !is_runtime_stop)
	{
		cb->Failure(409, "Wait for workspace deletion to finish.");
		return true;
	}

	struct Route
	{
		std::string_view action;
		ActionHandler handler;
	};

	static constexpr Route kRoutes[] = {
		{"getUsageHistory", &UamQueryHandler::HandleGetUsageHistory},
		{"getProviderAllowances", &UamQueryHandler::HandleGetProviderAllowances},
		{"getInitialState", &UamQueryHandler::HandleGetInitialState},
		{"getStartupNativeImports", &UamQueryHandler::HandleGetStartupNativeImports},
		{"resolveStartupNativeImports", &UamQueryHandler::HandleResolveStartupNativeImports},
		{"getAppPermissions", &UamQueryHandler::HandleGetAppPermissions},
		{"requestAppPermission", &UamQueryHandler::HandleRequestAppPermission},
		{"openAppPermissionSettings", &UamQueryHandler::HandleOpenAppPermissionSettings},
		{"repairCompanionLanAddress", &UamQueryHandler::HandleRepairCompanionLanAddress},
		{"selectSession", &UamQueryHandler::HandleSelectSession},
		{"acknowledgeChatAttention", &UamQueryHandler::HandleAcknowledgeChatAttention},
		{"setChatSnoozedUntil", &UamQueryHandler::HandleSetChatSnoozedUntil},
		{"setChatSettled", &UamQueryHandler::HandleSetChatSettled},
		{"setVisibleChatIds", &UamQueryHandler::HandleSetVisibleChatIds},
		{"releaseChatMessages", &UamQueryHandler::HandleReleaseChatMessages},
		{"getChatMessages", &UamQueryHandler::HandleGetChatMessages},
		{"getChatFileChangeReceipts", &UamQueryHandler::HandleGetChatFileChangeReceipts},
		{"applyOrganizationAction", &UamQueryHandler::HandleOrganizationAction},
		{"getToolCallContent", &UamQueryHandler::HandleGetToolCallContent},
		{"getAttachmentImagePreview", &UamQueryHandler::HandleGetAttachmentImagePreview},
		{"createSideChat", &UamQueryHandler::HandleCreateSideChat},
		{"dismissSideChat", &UamQueryHandler::HandleDismissSideChat},
		{"createSession", &UamQueryHandler::HandleCreateSession},
		{"branchFromMessage", &UamQueryHandler::HandleBranchFromMessage},
		{"retryFailedMessage", &UamQueryHandler::HandleRetryFailedMessage},
		{"openNativeSessionChat", &UamQueryHandler::HandleOpenNativeSessionChat},
		{"renameSession", &UamQueryHandler::HandleRenameSession},
		{"regenerateChatTitle", &UamQueryHandler::HandleRegenerateChatTitle},
		{"cancelChatTitle", &UamQueryHandler::HandleCancelChatTitle},
		{"setChatPinned", &UamQueryHandler::HandleSetChatPinned},
		{"setChatProvider", &UamQueryHandler::HandleSetChatProvider},
		{"setChatModel", &UamQueryHandler::HandleSetChatModel},
		{"setChatCodexOptions", &UamQueryHandler::HandleSetChatCodexOptions},
		{"setChatApprovalMode", &UamQueryHandler::HandleSetChatApprovalMode},
		{"setChatUamAgent", &UamQueryHandler::HandleSetChatUamAgent},
		{"setChatUamControlEnabled", &UamQueryHandler::HandleSetChatUamControlEnabled},
		{"listUamAgents", &UamQueryHandler::HandleListUamAgents},
		{"browseProviderAgentImport", &UamQueryHandler::HandleBrowseProviderAgentImport},
		{"previewProviderAgentImport", &UamQueryHandler::HandlePreviewProviderAgentImport},
		{"importProviderAgent", &UamQueryHandler::HandleImportProviderAgent},
		{"listAgentDefinitions", &UamQueryHandler::HandleListAgentDefinitions},
		{"saveAgentDefinition", &UamQueryHandler::HandleSaveAgentDefinition},
		{"deleteAgentDefinition", &UamQueryHandler::HandleDeleteAgentDefinition},
		{"getManagedAgentTranscript", &UamQueryHandler::HandleGetManagedAgentTranscript},
		{"resumeAgentRun", &UamQueryHandler::HandleResumeAgentRun},
		{"setChatCommandSafetyTier", &UamQueryHandler::HandleSetChatCommandSafetyTier},
		{"setChatComputerUseEnabled", &UamQueryHandler::HandleSetChatComputerUseEnabled},
		{"setChatComputerUseBackend", &UamQueryHandler::HandleSetChatComputerUseBackend},
		{"setComputerUseControl", &UamQueryHandler::HandleSetComputerUseControl},
		{"getComputerUseSettings", &UamQueryHandler::HandleGetComputerUseSettings},
		{"setComputerUseSettings", &UamQueryHandler::HandleSetComputerUseSettings},
		{"listComputerUseApplications", &UamQueryHandler::HandleListComputerUseApplications},
		{"checkComputerUsePermissions", &UamQueryHandler::HandleCheckComputerUsePermissions},
		{"requestComputerUsePermission", &UamQueryHandler::HandleRequestComputerUsePermission},
		{"openComputerUseSystemSettings", &UamQueryHandler::HandleOpenComputerUseSystemSettings},
		{"getCompanionSettings", &UamQueryHandler::HandleGetCompanionSettings},
		{"setCompanionEnabled", &UamQueryHandler::HandleSetCompanionEnabled},
		{"getCompanionToken", &UamQueryHandler::HandleGetCompanionToken},
		{"setChatMemoryEnabled", &UamQueryHandler::HandleSetChatMemoryEnabled},
		{"setChatSmallModelMode", &UamQueryHandler::HandleSetChatSmallModelMode},
		{"setChatRemoteRecovery", &UamQueryHandler::HandleSetChatRemoteRecovery},
		{"moveChatWorkspace", &UamQueryHandler::HandleMoveChatWorkspace},
		{"setMemorySettings", &UamQueryHandler::HandleSetMemorySettings},
		{"readCentralInstructionFile", &UamQueryHandler::HandleReadCentralInstructionFile},
		{"saveCentralInstructionFile", &UamQueryHandler::HandleSaveCentralInstructionFile},
		{"setCentralProviderConfiguration", &UamQueryHandler::HandleSetCentralProviderConfiguration},
		{"setMcpServers", &UamQueryHandler::HandleSetMcpServers},
		{"getProviderMcpStatus", &UamQueryHandler::HandleGetProviderMcpStatus},
		{"setProviderMcpConfiguration", &UamQueryHandler::HandleSetProviderMcpConfiguration},
		{"previewOpenCodeMcpImport", &UamQueryHandler::HandlePreviewOpenCodeMcpImport},
		{"setUamAgentPreferences", &UamQueryHandler::HandleSetUamAgentPreferences},
		{"setSidebarSettings", &UamQueryHandler::HandleSetSidebarSettings},
		{"setVcsSettings", &UamQueryHandler::HandleSetVcsSettings},
		{"setStorageSettings", &UamQueryHandler::HandleSetStorageSettings},
		{"setProviderAvailability", &UamQueryHandler::HandleSetProviderAvailability},
		{"setUpdateSettings", &UamQueryHandler::HandleSetUpdateSettings},
		{"setProviderChatDefaults", &UamQueryHandler::HandleSetProviderChatDefaults},
		{"setEditorSettings", &UamQueryHandler::HandleSetEditorSettings},
		{"setExecutionRoutingSettings", &UamQueryHandler::HandleSetExecutionRoutingSettings},
		{"listRepositoryMatches", &UamQueryHandler::HandleListRepositoryMatches},
		{"releaseExecutionRoutingReservation", &UamQueryHandler::HandleReleaseExecutionRoutingReservation},
		{"exportLocalChats", &UamQueryHandler::HandleExportLocalChats},
		{"importLocalChats", &UamQueryHandler::HandleImportLocalChats},
		{"setShellActions", &UamQueryHandler::HandleSetShellActions},
		{"applyShellActions", &UamQueryHandler::HandleApplyShellActions},
		{"dismissShellActionNotification", &UamQueryHandler::HandleDismissShellActionNotification},
		{"refreshCliProviderVersion", &UamQueryHandler::HandleRefreshCliProviderVersion},
		{"refreshAllCliProviderVersions", &UamQueryHandler::HandleRefreshAllCliProviderVersions},
		{"applyCliProviderVersions", &UamQueryHandler::HandleApplyCliProviderVersions},
		{"applyCliProviderVersion", &UamQueryHandler::HandleApplyCliProviderVersion},
		{"previewRemoteHost", &UamQueryHandler::HandlePreviewRemoteHost},
		{"installRemoteHost", &UamQueryHandler::HandleInstallRemoteHost},
		{"saveRemoteHostOptions", &UamQueryHandler::HandleSaveRemoteHostOptions},
		{"checkRemoteHostInstruction", &UamQueryHandler::HandleCheckRemoteHostInstruction},
		{"removeRemoteHost", &UamQueryHandler::HandleRemoveRemoteHost},
		{"listRemoteDirectories", &UamQueryHandler::HandleListRemoteDirectories},
		{"browseMarkdownStoreDirectory", &UamQueryHandler::HandleBrowseMarkdownStoreDirectory},
		{"setMarkdownStoreDirectory", &UamQueryHandler::HandleSetMarkdownStoreDirectory},
		{"listMarkdownStoreEntries", &UamQueryHandler::HandleListMarkdownStoreEntries},
		{"createMarkdownStoreEntry", &UamQueryHandler::HandleCreateMarkdownStoreEntry},
		{"updateMarkdownStoreEntry", &UamQueryHandler::HandleUpdateMarkdownStoreEntry},
		{"setMarkdownStoreFavorite", &UamQueryHandler::HandleSetMarkdownStoreFavorite},
		{"browseMarkdownStoreImport", &UamQueryHandler::HandleBrowseMarkdownStoreImport},
		{"previewMarkdownStoreImports", &UamQueryHandler::HandlePreviewMarkdownStoreImports},
		{"importMarkdownStoreEntries", &UamQueryHandler::HandleImportMarkdownStoreEntries},
		{"revealMarkdownStoreEntry", &UamQueryHandler::HandleRevealMarkdownStoreEntry},
		{"editMarkdownStoreEntry", &UamQueryHandler::HandleEditMarkdownStoreEntry},
		{"deleteSession", &UamQueryHandler::HandleDeleteSession},
		{"deleteSessions", &UamQueryHandler::HandleDeleteSessions},
		{"createFolder", &UamQueryHandler::HandleCreateFolder},
		{"renameFolder", &UamQueryHandler::HandleRenameFolder},
		{"deleteFolder", &UamQueryHandler::HandleDeleteFolder},
		{"deleteFolders", &UamQueryHandler::HandleDeleteFolders},
		{"toggleFolder", &UamQueryHandler::HandleToggleFolder},
		{"reorderFolders", &UamQueryHandler::HandleReorderFolders},
		{"rescanFolderChats", &UamQueryHandler::HandleRescanFolderChats},
		{"previewUnsortedWorkspaceFolders", &UamQueryHandler::HandlePreviewUnsortedWorkspaceFolders},
		{"rebuildUnsortedWorkspaceFolders", &UamQueryHandler::HandleRebuildUnsortedWorkspaceFolders},
		{"setCustomIcon", &UamQueryHandler::HandleSetCustomIcon},
		{"createResourceCollection", &UamQueryHandler::HandleCreateResourceCollection},
		{"renameResourceCollection", &UamQueryHandler::HandleRenameResourceCollection},
		{"deleteResourceCollection", &UamQueryHandler::HandleDeleteResourceCollection},
		{"toggleResourceCollection", &UamQueryHandler::HandleToggleResourceCollection},
		{"reorderResourceCollections", &UamQueryHandler::HandleReorderResourceCollections},
		{"addResourceReference", &UamQueryHandler::HandleAddResourceReference},
		{"removeResourceReference", &UamQueryHandler::HandleRemoveResourceReference},
		{"reorderResourceReferences", &UamQueryHandler::HandleReorderResourceReferences},
		{"browseFolderDirectory", &UamQueryHandler::HandleBrowseFolderDirectory},
		{"searchChatMessages", &UamQueryHandler::HandleSearchChatMessages},
		{"listMemoryEntries", &UamQueryHandler::HandleListMemoryEntries},
		{"createMemoryEntry", &UamQueryHandler::HandleCreateMemoryEntry},
		{"deleteMemoryEntry", &UamQueryHandler::HandleDeleteMemoryEntry},
		{"openMemoryRoot", &UamQueryHandler::HandleOpenMemoryRoot},
		{"revealMemoryEntry", &UamQueryHandler::HandleRevealMemoryEntry},
		{"openWorkspaceDirectory", &UamQueryHandler::HandleOpenWorkspaceDirectory},
		{"openWorkspaceEditor", &UamQueryHandler::HandleOpenWorkspaceEditor},
		{"openWorkspaceTerminal", &UamQueryHandler::HandleOpenWorkspaceTerminal},
		{"getChatWorktreeStatus", &UamQueryHandler::HandleGetChatWorktreeStatus},
		{"listChatBranches", &UamQueryHandler::HandleListChatBranches},
		{"switchChatBranch", &UamQueryHandler::HandleSwitchChatBranch},
		{"createChatWorktree", &UamQueryHandler::HandleCreateChatWorktree},
		{"discardChatWorktreeChanges", &UamQueryHandler::HandleDiscardChatWorktreeChanges},
		{"portChatWorktreeChanges", &UamQueryHandler::HandlePortChatWorktreeChanges},
		{"previewChatTurnRollback", &UamQueryHandler::HandlePreviewChatTurnRollback},
		{"rollbackChatTurn", &UamQueryHandler::HandleRollbackChatTurn},
		{"getWorkspaceVcsContext", &UamQueryHandler::HandleGetWorkspaceVcsContext},
		{"getVcsCommitStatus", &UamQueryHandler::HandleGetVcsCommitStatus},
		{"getVcsFileDiff", &UamQueryHandler::HandleGetVcsFileDiff},
		{"getWorkspaceFilePreview", &UamQueryHandler::HandleGetWorkspaceFilePreview},
		{"commitVcsChanges", &UamQueryHandler::HandleCommitVcsChanges},
		{"generateVcsCommitMessage", &UamQueryHandler::HandleGenerateVcsCommitMessage},
		{"listMemoryScanCandidates", &UamQueryHandler::HandleListMemoryScanCandidates},
		{"scanCurrentChats", &UamQueryHandler::HandleScanCurrentChats},
		{"recordRuntimeActivity", &UamQueryHandler::HandleRuntimeActivity},
		{"companionCliTerminal", &UamQueryHandler::HandleCompanionCli},
		{"startCliTerminal", &UamQueryHandler::HandleStartCli},
		{"stopCliTerminal", &UamQueryHandler::HandleStopCli},
		{"steerCliTerminal", &UamQueryHandler::HandleSteerCliTerminal},
		{"resizeCliTerminal", &UamQueryHandler::HandleResizeCli},
		{"writeCliInput", &UamQueryHandler::HandleWriteCliInput},
		{"setVoiceInputSettings", &UamQueryHandler::HandleSetVoiceInputSettings},
		{"startDictation", &UamQueryHandler::HandleStartDictation},
		{"stopDictation", &UamQueryHandler::HandleStopDictation},
		{"stageChatAttachments", &UamQueryHandler::HandleStageChatAttachments},
		{"sendAcpPrompt", &UamQueryHandler::HandleSendAcpPrompt},
		{"compactAcpSession", &UamQueryHandler::HandleCompactAcpSession},
		{"resumeQueuedAcpPrompts", &UamQueryHandler::HandleResumeQueuedAcpPrompts},
		{"manageQueuedAcpPrompt", &UamQueryHandler::HandleManageQueuedAcpPrompt},
		{"discoverProviderModels", &UamQueryHandler::HandleDiscoverProviderModels},
		{"setAcpConfigOption", &UamQueryHandler::HandleSetAcpConfigOption},
		{"cancelAcpTurn", &UamQueryHandler::HandleCancelAcpTurn},
		{"resolveAcpPermission", &UamQueryHandler::HandleResolveAcpPermission},
		{"resolveAcpUserInput", &UamQueryHandler::HandleResolveAcpUserInput},
		{"stopAcpSession", &UamQueryHandler::HandleStopAcpSession},
		{"writeClipboardText", &UamQueryHandler::HandleWriteClipboardText},
		{"setTheme", &UamQueryHandler::HandleSetTheme},
		{"listThemes", &UamQueryHandler::HandleListThemes},
		{"saveTheme", &UamQueryHandler::HandleSaveTheme},
		{"deleteTheme", &UamQueryHandler::HandleDeleteTheme},
		{"setGoal", &UamQueryHandler::HandleSetGoal},
		{"updateGoalStatus", &UamQueryHandler::HandleUpdateGoalStatus},
		{"updateGoalObjective", &UamQueryHandler::HandleUpdateGoalObjective},
		{"setActiveGoal", &UamQueryHandler::HandleSetActiveGoal},
		{"resumeGoal", &UamQueryHandler::HandleResumeGoal},
		{"removeGoal", &UamQueryHandler::HandleRemoveGoal},
	};

	for (const Route& route : kRoutes)
	{
		if (route.action == action)
		{
			(this->*route.handler)(browser, payload, cb);
			return true;
		}
	}

	return false;
}

bool UamQueryHandler::OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int64_t query_id, const CefString& request, bool /*persistent*/, CefRefPtr<Callback> callback)
{
	CEF_REQUIRE_UI_THREAD();

	if (!uam::cef::IsTrustedMainFrame(frame, m_trustedUiIndexUrl))
	{
		callback->Failure(403, "Privileged bridge is restricted to the bundled UI.");
		return true;
	}

	const uam::cef::BridgeRequestParseResult parsed = uam::cef::ParseBridgeRequest(request.ToString());
	if (!parsed.ok)
	{
		callback->Failure(parsed.status, parsed.error);
		return true;
	}

	const std::string& action = parsed.request.action;
	const nlohmann::json& payload = parsed.request.payload;

	if (action == "getWindowChrome" || action == "windowCommand")
	{
		HandleWindowChrome(browser, action, payload, callback);
		return true;
	}

	const std::string port_chat_id = action == "portChatWorktreeChanges" && payload.contains("chatId") && payload["chatId"].is_string() ? payload["chatId"].get<std::string>() : std::string();
	const std::unordered_map<std::string, std::shared_ptr<uam::AppState::GitPortRequest>>::const_iterator previous_port = m_app.git_port_requests.find(port_chat_id);
	const std::shared_ptr<uam::AppState::GitPortRequest> prior_port = previous_port == m_app.git_port_requests.end() ? nullptr : previous_port->second;
	try
	{
		if (action == "searchChatMessages") m_searchQueryId = query_id;
		if (action == "setGoal" || action == "updateGoalStatus" || action == "resumeGoal" ||
		    action == "updateGoalObjective" || action == "setActiveGoal" || action == "removeGoal")
			m_goalQueryIdentities[callback.get()] = ChatIconQueryIdentity{browser ? browser->GetIdentifier() : -1, frame ? frame->GetIdentifier().ToString() : "", query_id};
		if (action == "setCustomIcon") BeginChatIconQuery(callback.get(),
		    browser ? browser->GetIdentifier() : -1, frame ? frame->GetIdentifier().ToString() : "", query_id);
		if (!DispatchAction(action, browser, payload, callback))
		{
			callback->Failure(404, "Unknown action: " + action);
		}
		if (!port_chat_id.empty())
		{
			const std::unordered_map<std::string, std::shared_ptr<uam::AppState::GitPortRequest>>::const_iterator current = m_app.git_port_requests.find(port_chat_id);
			const std::weak_ptr<void> lifetime = m_asyncLifetime;
			if (current != m_app.git_port_requests.end() && current->second != prior_port &&
			    !current->second->handler_lifetime.owner_before(lifetime) && !lifetime.owner_before(current->second->handler_lifetime))
			{
				current->second->query_id = query_id;
				current->second->browser_id = browser ? browser->GetIdentifier() : -1;
				current->second->frame_id = frame ? frame->GetIdentifier().ToString() : "";
			}
		}
	}
	catch (const nlohmann::json::exception& ex)
	{
		callback->Failure(400, std::string("Invalid bridge payload: ") + ex.what());
	}
	catch (const std::exception& ex)
	{
		callback->Failure(500, std::string("Bridge request failed: ") + ex.what());
	}

	m_chatIconQueryIdentities.erase(callback.get());
	m_goalQueryIdentities.erase(callback.get());
	return true;
}

void UamQueryHandler::BeginChatIconQuery(Callback* callback, int browser_id, const std::string& frame_id, int64_t query_id)
{
	m_chatIconQueryIdentities[callback] = ChatIconQueryIdentity{browser_id, frame_id, query_id};
}

void UamQueryHandler::CancelChatIconQuery(int browser_id, const std::string& frame_id, int64_t query_id)
{
	const ChatIconQueryIdentity identity{browser_id, frame_id, query_id};
	const std::map<ChatIconQueryIdentity, std::function<void()>>::iterator icon = m_chatIconCancellations.find(identity);
	if (icon != m_chatIconCancellations.end())
	{
		std::function<void()> cancel = std::move(icon->second);
		m_chatIconCancellations.erase(icon);
		cancel();
	}
}

void UamQueryHandler::OnQueryCanceled(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int64_t query_id)
{
	const ChatIconQueryIdentity goal_identity{browser ? browser->GetIdentifier() : -1, frame ? frame->GetIdentifier().ToString() : "", query_id};
	const std::map<ChatIconQueryIdentity, std::weak_ptr<GoalMutationOperation>>::iterator goal_operation = m_goalQueryOperations.find(goal_identity);
	if (goal_operation != m_goalQueryOperations.end())
	{
		if (const std::shared_ptr<GoalMutationOperation> operation = goal_operation->second.lock()) operation->cancelled = true;
		m_goalQueryOperations.erase(goal_operation);
	}
	CancelChatIconQuery(browser ? browser->GetIdentifier() : -1, frame ? frame->GetIdentifier().ToString() : "", query_id);
	if (query_id == m_searchQueryId)
	{
		++*m_searchGeneration;
		m_searchQueryId = -1;
	}
	const std::weak_ptr<void> lifetime = m_asyncLifetime;
	for (const std::pair<const std::string, std::shared_ptr<uam::AppState::GitPortRequest>>& entry : m_app.git_port_requests)
	{
		const uam::AppState::GitPortRequest& request = *entry.second;
		if (browser && frame && request.query_id == query_id && request.browser_id == browser->GetIdentifier() && request.frame_id == frame->GetIdentifier().ToString() &&
		    !request.handler_lifetime.owner_before(lifetime) && !lifetime.owner_before(request.handler_lifetime)) request.cancellation->request_stop();
	}
}
