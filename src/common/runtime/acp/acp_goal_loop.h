#pragma once

#include "cef/cef_includes.h"
#include "common/state/app_state.h"

#include <string_view>

namespace uam::acp_detail
{

bool ResumeGoal(AppState& app, const std::string& chat_id, const std::string& goal_id, std::string* error_out = nullptr);
bool HandleGoalReviewCompletion(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser);
void ScheduleGoalReviewAfterSuccessfulTurn(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser);
void CompletePromptTurnAndHandleGoalLoop(AppState& app, AcpSessionState& session, ChatSession& chat, std::string_view lifecycle_state, CefRefPtr<CefBrowser> browser, bool continue_goal_loop = true);
bool ResumeStalledGoalLoopIfNeeded(AppState& app, AcpSessionState& session, ChatSession& chat, CefRefPtr<CefBrowser> browser, double now_seconds);

} // namespace uam::acp_detail
