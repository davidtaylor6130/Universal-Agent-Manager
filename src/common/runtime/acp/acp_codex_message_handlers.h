#pragma once

#include "cef/cef_includes.h"
#include "common/state/app_state.h"

namespace uam::acp_detail
{

void HandleCodexMessage(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message, CefRefPtr<CefBrowser> browser);

} // namespace uam::acp_detail
