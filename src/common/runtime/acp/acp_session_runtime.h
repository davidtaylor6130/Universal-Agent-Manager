#pragma once

#include "common/state/app_state.h"

#include <string>

namespace uam
{

AcpSessionState* FindAcpSessionForChat(AppState& app, const std::string& chat_id);
const AcpSessionState* FindAcpSessionForChat(const AppState& app, const std::string& chat_id);

bool SendAcpPrompt(AppState& app, const std::string& chat_id, const std::string& text, std::string* error_out = nullptr);
bool CancelAcpTurn(AppState& app, const std::string& chat_id, std::string* error_out = nullptr);
bool StopAcpSession(AppState& app, const std::string& chat_id);
bool ResolveAcpPermission(AppState& app,
                          const std::string& chat_id,
                          const std::string& request_id_json,
                          const std::string& option_id,
                          bool cancelled,
                          std::string* error_out = nullptr);

bool PollAllAcpSessions(AppState& app);
void FastStopAcpSessionsForExit(AppState& app);

std::string BuildAcpInitializeRequestForTests(int request_id);
std::string BuildAcpNewSessionRequestForTests(int request_id, const std::string& cwd);
std::string BuildAcpPromptRequestForTests(int request_id, const std::string& session_id, const std::string& text);
bool ProcessAcpLineForTests(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& line);

} // namespace uam
