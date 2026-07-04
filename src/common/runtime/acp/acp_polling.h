#pragma once

#include "cef/cef_includes.h"
#include "common/state/app_state.h"

#include <string>

namespace uam::acp_detail
{

bool ProcessAcpLine(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& line, CefRefPtr<CefBrowser> browser);
bool DrainStdout(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser);
bool DrainStderr(AcpSessionState& session);
void MarkAcpProcessExited(AcpSessionState& session, bool has_exit_code = false, int exit_code = 0);

} // namespace uam::acp_detail
