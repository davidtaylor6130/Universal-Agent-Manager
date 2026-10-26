#include "common/config/build_features.h"
#include "cef/uam_query_handler.h"
#include "cef/uam_bridge_request.h"
#include "cef/uam_cef_security.h"
#include "cef/uam_window_chrome.h"
#include "common/usage/multi_host_history.h"
#include "common/platform/app_crash_supervisor.h"
#include "app/data_folder_settings_service.h"
#include "cef/uam_query_handler_async.h"

#include "include/cef_task.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_helpers.h"

#include <nlohmann/json.hpp>
#include <algorithm>
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
		if (action == "quitApplication")
		{
			callback->Success("{}");
			CefPostTask(TID_UI, new CloseWindowTask(window));
			return;
		}
		if (action == "restartApplication")
		{
			if (!uam::platform::RequestApplicationRestart())
			{
				callback->Failure(409, "Automatic restart is unavailable. Close and open Universal Agent Manager again.");
				return;
			}
			callback->Success("{}");
			CefPostTask(TID_UI, new CloseWindowTask(window));
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

void UamQueryHandler::HandleRestartToApplyDataFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	if (!uam::DataFolderSettingsService::kTransfersValidated)
	{
		cb->Failure(409, "Restart to apply a data folder remains disabled until native handoff validation is adequate.");
		return;
	}
	const auto view = browser && !browser->IsPopup() ? CefBrowserView::GetForBrowser(browser) : nullptr;
	const auto window = view ? view->GetWindow() : nullptr;
	const auto service = m_app.data_folder_settings;
	if (!window || !service || service->StartupRoot() != m_app.data_root || !payload.is_object() || payload.size() != 1 || !payload.contains("expectedRevision") || !payload["expectedRevision"].is_string() || payload["expectedRevision"].get_ref<const std::string&>().empty() || payload["expectedRevision"].get_ref<const std::string&>().size() > 300)
	{
		cb->Failure(400, "Restart requires the exact observed local folder revision and native window owner.");
		return;
	}
	const auto revision = payload["expectedRevision"].get<std::string>();
	if (std::any_of(m_app.acp_sessions.begin(), m_app.acp_sessions.end(), [](const auto& session) { return session && session->running; }) || std::any_of(m_app.cli_terminals.begin(), m_app.cli_terminals.end(), [](const auto& terminal) { return terminal && terminal->running; }))
	{
		cb->Failure(409, "Stop active provider sessions before requesting a data-folder transfer. Their teardown has no generic confirmed-stop receipt.");
		return;
	}
	(void)uam::query_handler_async::RunAsyncCefQuery(
	    m_asyncLifetime, cb,
	    [service, revision]()
	    {
		    const auto armed = service->ArmRestart(revision);
		    return armed.committed ? uam::query_handler_async::AsyncSuccess(nlohmann::json::object()) : uam::query_handler_async::AsyncFailure(409, armed.error);
	    },
	    [window](uam::query_handler_async::AsyncCefResult& result)
	    {
		    if (!result.ok)
			    return;
		    if (!uam::platform::RequestApplicationRestart(true))
		    {
			    result = uam::query_handler_async::AsyncFailure(409, "Supervised restart is unavailable. The folder choice remains pending; no transfer occurred.");
			    return;
		    }
		    CefPostTask(TID_UI, new CloseWindowTask(window));
	    });
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

UamQueryHandler::UamQueryHandler(uam::AppState& app, std::string trusted_ui_index_url)
    : UamQueryHandler(app, std::move(trusted_ui_index_url), true)
{
}

UamQueryHandler::UamQueryHandler(uam::AppState& app, std::string trusted_ui_index_url, bool start_background_readers)
    : m_app(app), m_trustedUiIndexUrl(std::move(trusted_ui_index_url))
{
	if (start_background_readers)
	{
		m_usageHistory = std::make_shared<uam::usage::MultiHostHistory>(m_app.data_root, m_app.settings.execution_hosts);
		RefreshAllowanceTargets();
	}
}

UamQueryHandler::~UamQueryHandler()
{
	m_asyncLifetime.reset();
	for (const std::pair<const std::string, std::shared_ptr<std::stop_source>>& request : m_titleRequests) request.second->request_stop();
	for (const auto& request : m_nativeHistoryRequests) request.second->request_stop();
#if UAM_ENABLE_MOBILE_COMPANION
	if (m_companion) m_companion->Stop();
#endif
}

bool UamQueryHandler::StartCompanion(CefRefPtr<CefBrowser> browser)
{
#if UAM_ENABLE_MOBILE_COMPANION
	const std::weak_ptr<void> lifetime = m_asyncLifetime;
	m_companion = UamCompanionService::Create(m_app.data_root, [this, lifetime, browser](const nlohmann::json& request, CefRefPtr<Callback> callback) {
		if (request.value("action", std::string{}) == "setVisibleChatIds" || request.value("action", std::string{}) == "releaseChatMessages")
		{ callback->Failure(403, "Desktop transcript ownership is required."); return; }
		if (request.value("action", std::string{}) == "getLegacyChatImports" || request.value("action", std::string{}) == "resolveLegacyChatImport")
		{ callback->Failure(403, "Previous-version chat history is decided on the desktop."); return; }
		if (lifetime.expired()) { callback->Failure(503, "UAM is shutting down."); return; }
		if (!DispatchAction(request.at("action").get<std::string>(), browser, request.value("payload", nlohmann::json::object()), callback))
			callback->Failure(404, "Unknown companion action.");
	});
	const nlohmann::json status = m_companion->Status();
	return status.value("configured", false) && status.value("enabled", false);
#else
	return false;
#endif
}

bool UamQueryHandler::CompanionStartupFailed() const
{
#if UAM_ENABLE_MOBILE_COMPANION
	return m_companion && m_companion->Status().value("state", std::string{}) == "failed";
#else
	return false;
#endif
}

// ---------------------------------------------------------------------------
// CefMessageRouterBrowserSide::Handler
// ---------------------------------------------------------------------------

bool UamQueryHandler::DispatchAction(std::string_view action, CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
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
		{"captureChatRepositoryBaseline", &UamQueryHandler::HandleCaptureChatRepositoryBaseline},
		{"listSpeechVoices", &UamQueryHandler::HandleListSpeechVoices},
		{"startSpeechOutput", &UamQueryHandler::HandleStartSpeechOutput},
		{"stopSpeechOutput", &UamQueryHandler::HandleStopSpeechOutput},
		{"getSpeechOutputState", &UamQueryHandler::HandleGetSpeechOutputState},
		{"getUsageHistory", &UamQueryHandler::HandleGetUsageHistory},
		{"getProviderAllowances", &UamQueryHandler::HandleGetProviderAllowances},
		{"getInitialState", &UamQueryHandler::HandleGetInitialState},
		{"getChatRuntimeMetrics", &UamQueryHandler::HandleGetChatRuntimeMetrics},
		{"getStartupNativeImports", &UamQueryHandler::HandleGetStartupNativeImports},
		{"resolveStartupNativeImports", &UamQueryHandler::HandleResolveStartupNativeImports},
		{"getAppPermissions", &UamQueryHandler::HandleGetAppPermissions},
		{"requestAppPermission", &UamQueryHandler::HandleRequestAppPermission},
		{"openAppPermissionSettings", &UamQueryHandler::HandleOpenAppPermissionSettings},
#if UAM_ENABLE_MOBILE_COMPANION
		{"repairCompanionLanAddress", &UamQueryHandler::HandleRepairCompanionLanAddress},
#endif
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
		{"listManagedSubagents", &UamQueryHandler::HandleListManagedSubagents},
		{"cancelManagedSubagent", &UamQueryHandler::HandleCancelManagedSubagent},
		{"submitManagedSubagentFollowup", &UamQueryHandler::HandleSubmitManagedSubagentFollowup},
		{"resumeAgentRun", &UamQueryHandler::HandleResumeAgentRun},
		{"setChatCommandSafetyTier", &UamQueryHandler::HandleSetChatCommandSafetyTier},
		// Feature routes exist only in builds that include the feature; excluded actions are unknown.
#if UAM_ENABLE_COMPUTER_USE
		{"setChatComputerUseEnabled", &UamQueryHandler::HandleSetChatComputerUseEnabled},
		{"setChatComputerUseBackend", &UamQueryHandler::HandleSetChatComputerUseBackend},
		{"setComputerUseControl", &UamQueryHandler::HandleSetComputerUseControl},
		{"getComputerUseSettings", &UamQueryHandler::HandleGetComputerUseSettings},
		{"setComputerUseSettings", &UamQueryHandler::HandleSetComputerUseSettings},
		{"listComputerUseApplications", &UamQueryHandler::HandleListComputerUseApplications},
		{"checkComputerUsePermissions", &UamQueryHandler::HandleCheckComputerUsePermissions},
		{"requestComputerUsePermission", &UamQueryHandler::HandleRequestComputerUsePermission},
		{"openComputerUseSystemSettings", &UamQueryHandler::HandleOpenComputerUseSystemSettings},
#endif
#if UAM_ENABLE_MOBILE_COMPANION
		{"getCompanionSettings", &UamQueryHandler::HandleGetCompanionSettings},
		{"restartCompanion", &UamQueryHandler::HandleRestartCompanion},
		{"setCompanionEnabled", &UamQueryHandler::HandleSetCompanionEnabled},
		{"getCompanionToken", &UamQueryHandler::HandleGetCompanionToken},
#endif
		{"setChatMemoryEnabled", &UamQueryHandler::HandleSetChatMemoryEnabled},
		{"setChatSmallModelMode", &UamQueryHandler::HandleSetChatSmallModelMode},
#if UAM_ENABLE_SSH
		{"setChatRemoteRecovery", &UamQueryHandler::HandleSetChatRemoteRecovery},
#endif
		{"moveChatWorkspace", &UamQueryHandler::HandleMoveChatWorkspace},
		{"getMemorySettings", &UamQueryHandler::HandleGetMemorySettings},
		{"setMemorySettings", &UamQueryHandler::HandleSetMemorySettings},
		{"readCentralInstructionFile", &UamQueryHandler::HandleReadCentralInstructionFile},
		{"saveCentralInstructionFile", &UamQueryHandler::HandleSaveCentralInstructionFile},
		{"setCentralProviderConfiguration", &UamQueryHandler::HandleSetCentralProviderConfiguration},
		{"setMcpServers", &UamQueryHandler::HandleSetMcpServers},
		{"getProviderMcpStatus", &UamQueryHandler::HandleGetProviderMcpStatus},
		{"setProviderMcpConfiguration", &UamQueryHandler::HandleSetProviderMcpConfiguration},
#if UAM_ENABLE_RUNTIME_OPENCODE_CLI
		{"previewOpenCodeMcpImport", &UamQueryHandler::HandlePreviewOpenCodeMcpImport},
#endif
		{"setUamAgentPreferences", &UamQueryHandler::HandleSetUamAgentPreferences},
		{"setSidebarSettings", &UamQueryHandler::HandleSetSidebarSettings},
		{"setVcsSettings", &UamQueryHandler::HandleSetVcsSettings},
		{"setStorageSettings", &UamQueryHandler::HandleSetStorageSettings},
		{"setProviderAvailability", &UamQueryHandler::HandleSetProviderAvailability},
		{"setUpdateSettings", &UamQueryHandler::HandleSetUpdateSettings},
		{"setProviderChatDefaults", &UamQueryHandler::HandleSetProviderChatDefaults},
		{"setEditorSettings", &UamQueryHandler::HandleSetEditorSettings},
#if UAM_ENABLE_SSH
		{"setExecutionRoutingSettings", &UamQueryHandler::HandleSetExecutionRoutingSettings},
		{"listRepositoryMatches", &UamQueryHandler::HandleListRepositoryMatches},
		{"releaseExecutionRoutingReservation", &UamQueryHandler::HandleReleaseExecutionRoutingReservation},
#endif
		{"exportLocalChats", &UamQueryHandler::HandleExportLocalChats},
		{"importLocalChats", &UamQueryHandler::HandleImportLocalChats},
		{"getLegacyChatImports", &UamQueryHandler::HandleGetLegacyChatImports},
		{"resolveLegacyChatImport", &UamQueryHandler::HandleResolveLegacyChatImport},
		{"setShellActions", &UamQueryHandler::HandleSetShellActions},
		{"applyShellActions", &UamQueryHandler::HandleApplyShellActions},
		{"dismissShellActionNotification", &UamQueryHandler::HandleDismissShellActionNotification},
		{"refreshCliProviderVersion", &UamQueryHandler::HandleRefreshCliProviderVersion},
		{"refreshAllCliProviderVersions", &UamQueryHandler::HandleRefreshAllCliProviderVersions},
		{"applyCliProviderVersions", &UamQueryHandler::HandleApplyCliProviderVersions},
		{"applyCliProviderVersion", &UamQueryHandler::HandleApplyCliProviderVersion},
#if UAM_ENABLE_SSH
		{"previewRemoteHost", &UamQueryHandler::HandlePreviewRemoteHost},
		{"installRemoteHost", &UamQueryHandler::HandleInstallRemoteHost},
		{"saveRemoteHostOptions", &UamQueryHandler::HandleSaveRemoteHostOptions},
		{"checkRemoteHostInstruction", &UamQueryHandler::HandleCheckRemoteHostInstruction},
		{"removeRemoteHost", &UamQueryHandler::HandleRemoveRemoteHost},
		{"listRemoteDirectories", &UamQueryHandler::HandleListRemoteDirectories},
#endif
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
		{"captureChatReferenceContext", &UamQueryHandler::HandleCaptureChatReferenceContext},
		{"getVcsFileDiff", &UamQueryHandler::HandleGetVcsFileDiff},
		{"getWorkspaceFilePreview", &UamQueryHandler::HandleGetWorkspaceFilePreview},
		{"commitVcsChanges", &UamQueryHandler::HandleCommitVcsChanges},
		{"generateVcsCommitMessage", &UamQueryHandler::HandleGenerateVcsCommitMessage},
		{"listMemoryScanCandidates", &UamQueryHandler::HandleListMemoryScanCandidates},
		{"scanCurrentChats", &UamQueryHandler::HandleScanCurrentChats},
		{"recordRuntimeActivity", &UamQueryHandler::HandleRuntimeActivity},
#if UAM_ENABLE_MOBILE_COMPANION
		{"companionCliTerminal", &UamQueryHandler::HandleCompanionCli},
#endif
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
		{"setNativeModelOption", &UamQueryHandler::HandleSetNativeModelOption},
		{"refreshNativeModelOptions", &UamQueryHandler::HandleRefreshNativeModelOptions},
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

	if (action == "getWindowChrome" || action == "windowCommand" || action == "restartApplication" || action == "quitApplication")
	{
		HandleWindowChrome(browser, action, payload, callback);
		return true;
	}
	// Local profile selection is desktop-only, outside the companion DispatchAction path.
	if (action == "restartToApplyDataFolder")
	{
		HandleRestartToApplyDataFolder(browser, payload, callback);
		return true;
	}
	if (action == "getDataFolderSettings" || action == "chooseDataFolder" || action == "cancelDataFolderChoice" || action == "rollbackDataFolderTransfer")
	{
		HandleDataFolderSettings(action, payload, callback);
		return true;
	}

	const std::string port_chat_id = action == "portChatWorktreeChanges" && payload.contains("chatId") && payload["chatId"].is_string() ? payload["chatId"].get<std::string>() : std::string();
	const std::unordered_map<std::string, std::shared_ptr<uam::AppState::GitPortRequest>>::const_iterator previous_port = m_app.git_port_requests.find(port_chat_id);
	const std::shared_ptr<uam::AppState::GitPortRequest> prior_port = previous_port == m_app.git_port_requests.end() ? nullptr : previous_port->second;
	try
	{
		if (action == "submitManagedSubagentFollowup")
			m_managedFollowupQueryIdentities[callback.get()] = ChatIconQueryIdentity{browser ? browser->GetIdentifier() : -1, frame ? frame->GetIdentifier().ToString() : "", query_id};
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

	m_managedFollowupQueryIdentities.erase(callback.get());
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
	const auto followup = m_managedFollowupQueryOperations.find(goal_identity);
	if (followup != m_managedFollowupQueryOperations.end())
	{
		if (const auto operation = followup->second.lock()) operation->cancelled = true;
		m_managedFollowupQueryOperations.erase(followup);
	}
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
