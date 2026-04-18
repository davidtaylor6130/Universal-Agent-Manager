#pragma once

#include "cef/cef_includes.h"
#include "common/state/app_state.h"
#include <nlohmann/json.hpp>

#include <string>

/// <summary>
/// Handles all window.cefQuery() requests from the React frontend.
/// Registered with CefMessageRouterBrowserSide so that every call to
/// window.cefQuery({ request: JSON, onSuccess, onFailure }) arrives here.
///
/// All actions are dispatched synchronously on the CEF UI thread.
/// Long-running work (streaming, terminal I/O) is handled by the existing
/// runtime services — this handler only queues the work and responds.
/// </summary>
class UamQueryHandler : public CefMessageRouterBrowserSide::Handler
{
  public:
	explicit UamQueryHandler(uam::AppState& app, std::string trusted_ui_index_url);
	~UamQueryHandler() override = default;

	bool OnQuery(CefRefPtr<CefBrowser>  browser,
	             CefRefPtr<CefFrame>    frame,
	             int64_t                query_id,
	             const CefString&       request,
	             bool                   persistent,
	             CefRefPtr<Callback>    callback) override;

	void OnQueryCanceled(CefRefPtr<CefBrowser> browser,
	                     CefRefPtr<CefFrame>   frame,
	                     int64_t               query_id) override;

  private:
	uam::AppState& m_app;
	std::string    m_trustedUiIndexUrl;

	// ---- action handlers -------------------------------------------------
	void HandleGetInitialState(CefRefPtr<CefBrowser> browser, CefRefPtr<Callback> cb);
	void HandleSelectSession  (CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleCreateSession  (CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleRenameSession  (CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetChatModel   (CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetChatApprovalMode(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleDeleteSession  (CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleCreateFolder   (CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleRenameFolder   (CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleDeleteFolder   (CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleToggleFolder   (CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleBrowseFolderDirectory(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleStartCli       (CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleStopCli        (CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleResizeCli      (const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleWriteCliInput  (CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSendAcpPrompt  (CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleCancelAcpTurn  (CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleResolveAcpPermission(CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleStopAcpSession (CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
	void HandleSetTheme       (CefRefPtr<CefBrowser> browser, const nlohmann::json& payload, CefRefPtr<Callback> cb);
};
