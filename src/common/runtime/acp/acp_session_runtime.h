#pragma once

#include "cef/cef_includes.h"
#include "common/state/app_state.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace uam
{

	AcpSessionState* FindAcpSessionForChat(AppState& app, const std::string& chat_id);
	const AcpSessionState* FindAcpSessionForChat(const AppState& app, const std::string& chat_id);

bool SendAcpPrompt(AppState& app, const std::string& chat_id, const std::string& text, const std::vector<std::string>& markdown_store_files = {}, const std::vector<MessageAttachment>& attachments = {}, bool goal_mode = false, std::string* error_out = nullptr, const std::string& goal_id = {}, bool computer_use_mode = false);
bool SendAcpPrompt(AppState& app, const std::string& chat_id, const std::string& text, std::string* error_out);
bool SteerAcpPrompt(AppState& app, const std::string& chat_id, const std::string& text, const std::vector<std::string>& markdown_store_files = {}, const std::vector<MessageAttachment>& attachments = {}, bool goal_mode = false, std::string* error_out = nullptr, const std::string& goal_id = {}, bool computer_use_mode = false);
bool RemoveQueuedAcpPrompt(AppState& app, const std::string& chat_id, std::size_t index, std::string* error_out = nullptr);
bool SteerQueuedAcpPrompt(AppState& app, const std::string& chat_id, std::size_t index, std::string* error_out = nullptr);
bool StartAcpModelDiscovery(AppState& app, const std::string& chat_id, std::string* error_out = nullptr, bool stop_when_complete = false);
bool StartEphemeralAcpModelDiscovery(AppState& app, const std::string& provider_id, const std::string& workspace_directory, const std::string& execution_host_id = "local", std::string* error_out = nullptr);
bool QueueAcpModelDiscoveryCompatibilityRetry(AppState& app, const std::string& chat_id, const std::string& provider_id, const std::string& workspace_directory, const std::string& execution_host_id, const std::string& blocked_reason);
bool RetryCompatibilityBlockedAcpModelDiscoveries(AppState& app);
bool RetryLastAcpPrompt(AppState& app, const std::string& chat_id, std::string* error_out = nullptr);
bool DrainNextQueuedAcpUserPrompt(AppState& app, AcpSessionState& session, ChatSession& chat);
bool CancelAcpTurn(AppState& app, const std::string& chat_id, std::string* error_out = nullptr);
void FinalizeAcpTurnInactivityTimeout(AppState& app, AcpSessionState& session, ChatSession& chat);
bool HandleAcpTurnInactivityTimeout(AppState& app, AcpSessionState& session, ChatSession& chat, double now_seconds);
bool StopAcpSession(AppState& app, const std::string& chat_id);
bool SetAcpSessionMode(AppState& app,
                       const std::string& chat_id,
                       const std::string& mode_id,
                       std::string* error_out = nullptr,
                       std::optional<std::string> previous_chat_mode_id = std::nullopt,
                       std::optional<std::string> previous_command_safety_tier = std::nullopt);
bool SetAcpSessionModel(AppState& app, const std::string& chat_id, const std::string& model_id, std::string* error_out = nullptr, std::optional<std::string> previous_chat_model_id = std::nullopt);
bool SetAcpSessionReasoningEffort(AppState& app, const std::string& chat_id, const std::string& reasoning_effort, std::string* error_out = nullptr, std::optional<std::string> previous_chat_reasoning_effort = std::nullopt);
bool SetAcpSessionConfigOption(AppState& app, const std::string& chat_id, const std::string& config_id, const std::string& value, std::string* error_out = nullptr);
bool TryAutoApprovePendingAcpPermission(AppState& app, const std::string& chat_id, std::string* error_out = nullptr);
bool ResolveAcpPermission(AppState& app,
                          const std::string& chat_id,
                          const std::string& request_id_json,
                          const std::string& option_id,
                          bool cancelled,
                          std::string* error_out = nullptr);
bool ResolveAcpUserInput(AppState& app,
                         const std::string& chat_id,
                         const std::string& request_id_json,
                         const std::map<std::string, std::vector<std::string>>& answers,
                         std::string* error_out = nullptr);

	bool PollAllAcpSessions(AppState& app, CefRefPtr<CefBrowser> browser = nullptr);
	void FlushPendingChatSaves(AppState& app);
	void FastStopAcpSessionsForExit(AppState& app);

	std::vector<std::string> BuildAcpLaunchArgvForTests(const ChatSession& chat);
	std::string BuildAcpLaunchDetailForTests(const std::filesystem::path& workspace_root, const ChatSession& chat);
	std::string BuildAcpLaunchDetailForTests(const AppState& app, const std::filesystem::path& workspace_root, const ChatSession& chat);
	std::string BuildAcpInitializeRequestForTests(int request_id);
	std::string BuildAcpNewSessionRequestForTests(int request_id, const std::string& cwd);
	std::string BuildGeminiSessionSetupRequestForTests(int request_id, const ChatSession& chat, const std::string& cwd, bool load_session_supported);
	std::string BuildAcpPromptRequestForTests(int request_id, const std::string& session_id, const std::string& text);
	std::string BuildAcpSetModeRequestForTests(int request_id, const std::string& session_id, const std::string& mode_id);
	std::string BuildAcpSetModelRequestForTests(int request_id, const std::string& session_id, const std::string& model_id);
	std::string BuildCodexInitializeRequestForTests(int request_id);
	std::string BuildCodexInitializedNotificationForTests();
	std::string BuildCodexModelListRequestForTests(int request_id);
	std::string BuildCodexSessionSetupRequestForTests(int request_id, const ChatSession& chat, const std::string& cwd);
	std::string BuildCodexThreadStartRequestForTests(int request_id, const ChatSession& chat, const std::string& cwd);
	std::string BuildCodexThreadResumeRequestForTests(int request_id, const ChatSession& chat, const std::string& cwd);
	std::string BuildCodexTurnStartRequestForTests(int request_id, const std::string& thread_id, const std::string& text, const ChatSession& chat, const std::string& active_model_id = "");
	std::string BuildCodexTurnInterruptRequestForTests(int request_id, const std::string& thread_id, const std::string& turn_id);
	std::string BuildCodexUserInputResponseForTests(const std::string& request_id_json, const std::map<std::string, std::vector<std::string>>& answers);
	std::string ResolveAcpSessionResumeIdForTests(const AppState& app, const ChatSession& chat);
	bool ProcessAcpLineForTests(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& line);
	bool IsValidCodexThreadIdForTests(const std::string& thread_id);
	bool UpdateAcpStaleWaitForTests(AcpSessionState& session, double now_seconds);
	std::string AutoApproveOptionIdForTests(const AcpPendingPermissionState& pending);
	bool ResumeStalledGoalLoopForTests(AppState& app, AcpSessionState& session, ChatSession& chat, double now_seconds);
	double AcpReconnectDelaySecondsForTests(int attempt);
	void ScheduleAcpReconnectForTests(AcpSessionState& session, double now_seconds);

} // namespace uam
