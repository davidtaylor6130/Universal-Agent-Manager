#include "cef/uam_query_handler.h"
#include "cef/uam_bridge_request.h"
#include "cef/uam_cef_security.h"

#include "include/wrapper/cef_helpers.h"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

UamQueryHandler::UamQueryHandler(uam::AppState& app, std::string trusted_ui_index_url) : m_app(app), m_trustedUiIndexUrl(std::move(trusted_ui_index_url))
{
}

// ---------------------------------------------------------------------------
// CefMessageRouterBrowserSide::Handler
// ---------------------------------------------------------------------------

bool UamQueryHandler::DispatchAction(std::string_view action, CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb)
{
	const std::string chat_id = payload.value("chatId", "");
	if ((!chat_id.empty() && m_app.worktree_operation_chat_ids.contains(chat_id)) ||
	    (!m_app.worktree_operation_chat_ids.empty() && (action == "deleteFolder" || action == "rescanFolderChats")))
	{
		cb->Failure(409, "Wait for the chat worktree operation to finish.");
		return true;
	}

	struct Route
	{
		std::string_view action;
		ActionHandler handler;
	};

	static constexpr Route kRoutes[] = {
		{"getInitialState", &UamQueryHandler::HandleGetInitialState},
		{"selectSession", &UamQueryHandler::HandleSelectSession},
		{"getChatMessages", &UamQueryHandler::HandleGetChatMessages},
		{"getToolCallContent", &UamQueryHandler::HandleGetToolCallContent},
		{"createSession", &UamQueryHandler::HandleCreateSession},
		{"branchFromMessage", &UamQueryHandler::HandleBranchFromMessage},
		{"openNativeSessionChat", &UamQueryHandler::HandleOpenNativeSessionChat},
		{"renameSession", &UamQueryHandler::HandleRenameSession},
		{"setChatPinned", &UamQueryHandler::HandleSetChatPinned},
		{"setChatProvider", &UamQueryHandler::HandleSetChatProvider},
		{"setChatModel", &UamQueryHandler::HandleSetChatModel},
		{"setChatCodexOptions", &UamQueryHandler::HandleSetChatCodexOptions},
		{"setChatApprovalMode", &UamQueryHandler::HandleSetChatApprovalMode},
		{"setChatAutoApproveCommands", &UamQueryHandler::HandleSetChatAutoApproveCommands},
		{"setChatCommandSafetyTier", &UamQueryHandler::HandleSetChatCommandSafetyTier},
		{"setChatMemoryEnabled", &UamQueryHandler::HandleSetChatMemoryEnabled},
		{"setMemorySettings", &UamQueryHandler::HandleSetMemorySettings},
		{"setVoiceInputSettings", &UamQueryHandler::HandleSetVoiceInputSettings},
		{"setSidebarSettings", &UamQueryHandler::HandleSetSidebarSettings},
		{"setUpdateSettings", &UamQueryHandler::HandleSetUpdateSettings},
		{"setProviderChatDefaults", &UamQueryHandler::HandleSetProviderChatDefaults},
		{"setEditorSettings", &UamQueryHandler::HandleSetEditorSettings},
		{"setShellActions", &UamQueryHandler::HandleSetShellActions},
		{"applyShellActions", &UamQueryHandler::HandleApplyShellActions},
		{"refreshCliProviderVersion", &UamQueryHandler::HandleRefreshCliProviderVersion},
		{"refreshAllCliProviderVersions", &UamQueryHandler::HandleRefreshAllCliProviderVersions},
		{"applyCliProviderVersion", &UamQueryHandler::HandleApplyCliProviderVersion},
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
		{"createFolder", &UamQueryHandler::HandleCreateFolder},
		{"renameFolder", &UamQueryHandler::HandleRenameFolder},
		{"deleteFolder", &UamQueryHandler::HandleDeleteFolder},
		{"toggleFolder", &UamQueryHandler::HandleToggleFolder},
		{"reorderFolders", &UamQueryHandler::HandleReorderFolders},
		{"rescanFolderChats", &UamQueryHandler::HandleRescanFolderChats},
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
		{"createChatWorktree", &UamQueryHandler::HandleCreateChatWorktree},
		{"discardChatWorktreeChanges", &UamQueryHandler::HandleDiscardChatWorktreeChanges},
		{"portChatWorktreeChanges", &UamQueryHandler::HandlePortChatWorktreeChanges},
		{"getVcsCommitStatus", &UamQueryHandler::HandleGetVcsCommitStatus},
		{"getVcsFileDiff", &UamQueryHandler::HandleGetVcsFileDiff},
		{"commitVcsChanges", &UamQueryHandler::HandleCommitVcsChanges},
		{"generateVcsCommitMessage", &UamQueryHandler::HandleGenerateVcsCommitMessage},
		{"listMemoryScanCandidates", &UamQueryHandler::HandleListMemoryScanCandidates},
		{"scanCurrentChats", &UamQueryHandler::HandleScanCurrentChats},
		{"startCliTerminal", &UamQueryHandler::HandleStartCli},
		{"stopCliTerminal", &UamQueryHandler::HandleStopCli},
		{"steerCliTerminal", &UamQueryHandler::HandleSteerCliTerminal},
		{"resizeCliTerminal", &UamQueryHandler::HandleResizeCli},
		{"writeCliInput", &UamQueryHandler::HandleWriteCliInput},
		{"startDictation", &UamQueryHandler::HandleStartDictation},
		{"stopDictation", &UamQueryHandler::HandleStopDictation},
		{"stageChatAttachments", &UamQueryHandler::HandleStageChatAttachments},
		{"sendAcpPrompt", &UamQueryHandler::HandleSendAcpPrompt},
		{"manageQueuedAcpPrompt", &UamQueryHandler::HandleManageQueuedAcpPrompt},
		{"discoverProviderModels", &UamQueryHandler::HandleDiscoverProviderModels},
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

bool UamQueryHandler::OnQuery(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int64_t /*query_id*/, const CefString& request, bool /*persistent*/, CefRefPtr<Callback> callback)
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

	try
	{
		if (!DispatchAction(action, browser, payload, callback))
		{
			callback->Failure(404, "Unknown action: " + action);
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

	return true;
}

void UamQueryHandler::OnQueryCanceled(CefRefPtr<CefBrowser> /*browser*/, CefRefPtr<CefFrame> /*frame*/, int64_t /*query_id*/)
{
	// Persistent queries are not used; nothing to cancel.
}
