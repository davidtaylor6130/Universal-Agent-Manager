#pragma once

#include "cef/cef_includes.h"
#include "common/state/app_state.h"

namespace uam::acp_detail
{

void HandleSessionUpdate(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& params, CefRefPtr<CefBrowser> browser);

} // namespace uam::acp_detail
