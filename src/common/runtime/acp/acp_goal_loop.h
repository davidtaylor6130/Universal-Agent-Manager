#pragma once

#include "cef/cef_includes.h"
#include "common/state/app_state.h"

#include <string_view>

namespace uam::acp_detail
{

bool HandleGoalReviewCompletion(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser);
void ScheduleGoalReviewAfterSuccessfulTurn(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser);
void CompletePromptTurnAndHandleGoalLoop(AppState& app, AcpSessionState& session, ChatSession& chat, std::string_view lifecycle_state, CefRefPtr<CefBrowser> browser);

} // namespace uam::acp_detail
