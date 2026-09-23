#pragma once

#include "cef/companion_server.h"
#include <memory>
#include <stop_token>

#include "cef/cef_includes.h"
#include "common/state/app_state.h"
#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <unordered_map>

namespace uam::query_handler_async { struct AsyncCefResult; }

/// <summary>
/// Handles all window.cefQuery() requests from the React frontend.
/// Registered with CefMessageRouterBrowserSide so that every call to
/// window.cefQuery({ request: JSON, onSuccess, onFailure }) arrives here.
///
/// Dispatches actions on the CEF UI thread and offloads blocking operations.
/// Async workers capture snapshots; completions stop when this handler is destroyed.
/// </summary>
class UamQueryHandler : public CefMessageRouterBrowserSide::Handler
{
  public:
	explicit UamQueryHandler(uam::AppState& app, std::string trusted_ui_index_url);
	~UamQueryHandler() override;
	void StartCompanion(CefRefPtr<CefBrowser> browser);

	bool OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int64_t query_id, const CefString& request, bool persistent, CefRefPtr<Callback> callback) override;

	void OnQueryCanceled(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int64_t query_id) override;

  private:
	uam::AppState& m_app;
	CefRefPtr<UamCompanionServer> m_companion;
	// Workers hold weak references so queued callbacks cannot outlive this handler.
	std::shared_ptr<void> m_asyncLifetime = std::make_shared<char>();
	std::unordered_map<std::string, std::shared_ptr<std::stop_source>> m_nativeHistoryRequests;
	std::string m_trustedUiIndexUrl;
	using ActionHandler = void (UamQueryHandler::*)(CefRefPtr<CefBrowser>, const nlohmann::json&, CefRefPtr<Callback>);
	bool DispatchAction(std::string_view action, CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);

	// ---- action handlers -------------------------------------------------
	void HandleGetInitialState(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSelectSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleGetChatMessages(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleGetToolCallContent(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleCreateSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleRetryFailedMessage(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleBranchFromMessage(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleOpenNativeSessionChat(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	uam::query_handler_async::AsyncCefResult FinishOpenNativeSessionChat(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, const ChatSession* native_snapshot = nullptr);
	void HandleRenameSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetChatPinned(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetChatProvider(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetChatModel(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetChatCodexOptions(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetChatApprovalMode(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetChatUamAgent(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetChatUamControlEnabled(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleListUamAgents(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleBrowseProviderAgentImport(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandlePreviewProviderAgentImport(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleImportProviderAgent(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleGetManagedAgentTranscript(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleResumeAgentRun(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetChatCommandSafetyTier(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetChatComputerUseEnabled(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetChatComputerUseBackend(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetComputerUseControl(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleGetComputerUseSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetComputerUseSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleListComputerUseApplications(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleCheckComputerUsePermissions(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleRequestComputerUsePermission(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleOpenComputerUseSystemSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleGetCompanionSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetCompanionEnabled(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleGetCompanionToken(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetChatMemoryEnabled(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetChatSmallModelMode(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetMemorySettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetMcpServers(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetUamAgentPreferences(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetSidebarSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetUpdateSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetProviderChatDefaults(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetEditorSettings(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleExportLocalChats(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleImportLocalChats(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetShellActions(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleApplyShellActions(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleDismissShellActionNotification(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleRefreshCliProviderVersion(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleRefreshAllCliProviderVersions(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleApplyCliProviderVersion(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandlePreviewRemoteHost(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleInstallRemoteHost(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleRemoveRemoteHost(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleListRemoteDirectories(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleBrowseMarkdownStoreDirectory(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetMarkdownStoreDirectory(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleListMarkdownStoreEntries(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleCreateMarkdownStoreEntry(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleUpdateMarkdownStoreEntry(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetMarkdownStoreFavorite(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleBrowseMarkdownStoreImport(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandlePreviewMarkdownStoreImports(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleImportMarkdownStoreEntries(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleRevealMarkdownStoreEntry(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleEditMarkdownStoreEntry(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleDeleteSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleDeleteSessions(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);

	void HandleCreateFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleRenameFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleDeleteFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleToggleFolder(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleReorderFolders(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleRescanFolderChats(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandlePreviewUnsortedWorkspaceFolders(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleRebuildUnsortedWorkspaceFolders(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleCreateResourceCollection(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleRenameResourceCollection(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleDeleteResourceCollection(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleToggleResourceCollection(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleReorderResourceCollections(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleAddResourceReference(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleRemoveResourceReference(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleReorderResourceReferences(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleBrowseFolderDirectory(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSearchChatMessages(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleListMemoryEntries(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleCreateMemoryEntry(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleDeleteMemoryEntry(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleOpenMemoryRoot(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleRevealMemoryEntry(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleOpenWorkspaceDirectory(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleOpenWorkspaceEditor(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleOpenWorkspaceTerminal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleGetChatWorktreeStatus(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleCreateChatWorktree(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleDiscardChatWorktreeChanges(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandlePortChatWorktreeChanges(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandlePreviewChatTurnRollback(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleRollbackChatTurn(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleGetVcsCommitStatus(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleGetVcsFileDiff(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleCommitVcsChanges(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleGenerateVcsCommitMessage(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleListMemoryScanCandidates(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleScanCurrentChats(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleStartCli(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleStopCli(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSteerCliTerminal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleResizeCli(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleWriteCliInput(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleStartDictation(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleStopDictation(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleStageChatAttachments(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSendAcpPrompt(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleManageQueuedAcpPrompt(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleDiscoverProviderModels(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetAcpConfigOption(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleCancelAcpTurn(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleResolveAcpPermission(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleResolveAcpUserInput(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleStopAcpSession(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleWriteClipboardText(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetTheme(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleListThemes(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSaveTheme(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleDeleteTheme(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetGoal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleUpdateGoalStatus(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleUpdateGoalObjective(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetActiveGoal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleResumeGoal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleRemoveGoal(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
};
