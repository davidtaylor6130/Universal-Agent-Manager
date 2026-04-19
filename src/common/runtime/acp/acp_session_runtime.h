#pragma once

#include "common/state/app_state.h"

#include <filesystem>
#include <string>
#include <vector>

namespace uam
{

AcpSessionState* FindAcpSessionForChat(AppState& app, const std::string& chat_id);
const AcpSessionState* FindAcpSessionForChat(const AppState& app, const std::string& chat_id);

bool SendAcpPrompt(AppState& app, const std::string& chat_id, const std::string& text, std::string* error_out = nullptr);
bool CancelAcpTurn(AppState& app, const std::string& chat_id, std::string* error_out = nullptr);
bool StopAcpSession(AppState& app, const std::string& chat_id);
bool SetAcpSessionMode(AppState& app, const std::string& chat_id, const std::string& mode_id, std::string* error_out = nullptr);
bool SetAcpSessionModel(AppState& app, const std::string& chat_id, const std::string& model_id, std::string* error_out = nullptr);
bool ResolveAcpPermission(AppState& app,
                          const std::string& chat_id,
                          const std::string& request_id_json,
                          const std::string& option_id,
                          bool cancelled,
                          std::string* error_out = nullptr);

bool PollAllAcpSessions(AppState& app);
void FastStopAcpSessionsForExit(AppState& app);

std::vector<std::string> BuildAcpLaunchArgvForTests(const ChatSession& chat);
std::string BuildAcpLaunchDetailForTests(const std::filesystem::path& workspace_root, const ChatSession& chat);
std::string BuildAcpInitializeRequestForTests(int request_id);
std::string BuildAcpNewSessionRequestForTests(int request_id, const std::string& cwd);
std::string BuildAcpPromptRequestForTests(int request_id, const std::string& session_id, const std::string& text);
std::string BuildAcpSetModeRequestForTests(int request_id, const std::string& session_id, const std::string& mode_id);
std::string BuildAcpSetModelRequestForTests(int request_id, const std::string& session_id, const std::string& model_id);
std::string BuildCodexInitializeRequestForTests(int request_id);
std::string BuildCodexInitializedNotificationForTests();
std::string BuildCodexModelListRequestForTests(int request_id);
std::string BuildCodexSessionSetupRequestForTests(int request_id, const ChatSession& chat, const std::string& cwd);
std::string BuildCodexThreadStartRequestForTests(int request_id, const ChatSession& chat, const std::string& cwd);
std::string BuildCodexThreadResumeRequestForTests(int request_id, const ChatSession& chat, const std::string& cwd);
std::string BuildCodexTurnStartRequestForTests(int request_id, const std::string& thread_id, const std::string& text, const ChatSession& chat);
std::string BuildCodexTurnInterruptRequestForTests(int request_id, const std::string& thread_id, const std::string& turn_id);
bool ProcessAcpLineForTests(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& line);
bool IsValidCodexThreadIdForTests(const std::string& thread_id);

} // namespace uam
