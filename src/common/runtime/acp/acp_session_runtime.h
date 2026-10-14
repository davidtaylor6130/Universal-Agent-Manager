#pragma once
#include <functional>

#include "cef/cef_includes.h"
#include "common/state/app_state.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <utility>

namespace uam
{

	AcpSessionState* FindAcpSessionForChat(AppState& app, const std::string& chat_id);
	const AcpSessionState* FindAcpSessionForChat(const AppState& app, const std::string& chat_id);

bool SendAcpPrompt(AppState& app, const std::string& chat_id, const std::string& text, const std::vector<std::string>& markdown_store_files = {}, const std::vector<MessageAttachment>& attachments = {}, bool goal_mode = false, std::string* error_out = nullptr, const std::string& goal_id = {}, bool computer_use_mode = false);
bool SendAcpPrompt(AppState& app, const std::string& chat_id, const std::string& text, std::string* error_out);
bool SteerAcpPrompt(AppState& app, const std::string& chat_id, const std::string& text, const std::vector<std::string>& markdown_store_files = {}, const std::vector<MessageAttachment>& attachments = {}, bool goal_mode = false, std::string* error_out = nullptr, const std::string& goal_id = {}, bool computer_use_mode = false);
bool RemoveQueuedAcpPrompt(AppState& app, const std::string& chat_id, std::size_t index, std::string* error_out = nullptr, const std::optional<std::pair<std::string, int>>& expected = std::nullopt);
bool SteerQueuedAcpPrompt(AppState& app, const std::string& chat_id, std::size_t index, std::string* error_out = nullptr, const std::optional<std::pair<std::string, int>>& expected = std::nullopt);
/// <summary>Updates an undispatched prompt while preserving captured context and optional retained attachment IDs.</summary>
bool EditQueuedAcpPrompt(AppState& app, const std::string& chat_id, const std::string& prompt_id, int expected_revision,
    const std::string& text, std::string* error_out = nullptr,
    const std::optional<std::vector<std::string>>& retained_attachment_ids = std::nullopt);
/// <summary>Reorders the exact observed queue; stale identities or revisions reject the operation.</summary>
bool ReorderQueuedAcpPrompts(AppState& app, const std::string& chat_id, const std::vector<std::pair<std::string, int>>& expected, const std::vector<std::string>& ordered_ids, std::string* error_out = nullptr);
/// <summary>Durably resumes only the exact observed held queue before scheduling delivery.</summary>
bool ResumeQueuedAcpPrompts(AppState& app, const std::string& chat_id, const std::vector<std::pair<std::string, int>>& expected, std::string* error_out = nullptr);
bool StartAcpModelDiscovery(AppState& app, const std::string& chat_id, std::string* error_out = nullptr, bool stop_when_complete = false);
bool StartEphemeralAcpModelDiscovery(AppState& app, const std::string& provider_id, const std::string& workspace_directory, const std::string& execution_host_id = "local", std::string* error_out = nullptr);
bool QueueAcpModelDiscoveryCompatibilityRetry(AppState& app, const std::string& chat_id, const std::string& provider_id, const std::string& workspace_directory, const std::string& execution_host_id, const std::string& blocked_reason);
bool RetryCompatibilityBlockedAcpModelDiscoveries(AppState& app);
bool RetryLastAcpPrompt(AppState& app, const std::string& chat_id, std::string* error_out = nullptr);
/// <summary>Raw authored indices excluded from delivered history only when their own interval has no output.</summary>
std::vector<int> KnownUnsentAcpMessageIndices(const AppState& app, const ChatSession& chat);
/// <summary>Latest legacy draft authorized for explicit fresh-session retry without replaying any earlier draft.</summary>
std::vector<int> RecoverableUnsentAcpMessageIndices(const AppState& app, const ChatSession& chat);
bool RetryFailedAcpMessage(AppState& app, const std::string& chat_id, int message_index, std::string* error_out = nullptr);
bool DrainNextQueuedAcpUserPrompt(AppState& app, AcpSessionState& session, ChatSession& chat);
bool PersistQueuedAcpUserPromptsAsInterrupted(AcpSessionState& session, ChatSession& chat);
/// <summary>Requests native compaction in a ready idle session without deleting saved history.</summary>
bool CompactAcpSession(AppState& app, const std::string& chat_id, std::string* error_out = nullptr);
bool CancelAcpTurn(AppState& app, const std::string& chat_id,
                   std::string* error_out = nullptr, bool update_goal_state = true,
                   bool preserve_queued_prompts = false);
void FinalizeAcpTurnInactivityTimeout(AppState& app, AcpSessionState& session, ChatSession& chat, bool maintenance_operation = false);
bool HandleAcpTurnInactivityTimeout(AppState& app, AcpSessionState& session, ChatSession& chat, double now_seconds);
/// <summary>Stops a provider; internal recovery may preserve held intent instead of discarding it.</summary>
bool StopAcpSession(AppState& app, const std::string& chat_id, AcpStopPurpose purpose = AcpStopPurpose::Interrupt, bool preserve_held_queue = false);
bool SetAcpSessionMode(AppState& app,
                       const std::string& chat_id,
                       const std::string& mode_id,
                       std::string* error_out = nullptr,
                       std::optional<std::string> previous_chat_mode_id = std::nullopt,
                       std::optional<std::string> previous_command_safety_tier = std::nullopt);
bool SetAcpSessionModel(AppState& app, const std::string& chat_id, const std::string& model_id, std::string* error_out = nullptr, std::optional<std::string> previous_chat_model_id = std::nullopt);
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
	bool AcpStopInProgress(const AppState& app, std::string_view chat_id);
	bool EnsureAcpStopProgress(AppState& app, std::string_view chat_id);
	void TransferStdioProcessFields(platform::StdioProcessPlatformFields& source,
	                                platform::StdioProcessPlatformFields& destination);
	void QueueAcpProcessStop(AppState& app, platform::StdioProcessPlatformFields& process, std::string_view chat_id = {}, AcpStopPurpose purpose = AcpStopPurpose::Interrupt, bool observe_exit = false);
	bool PrepareCodexThreadForRuntimeLaunch(AppState& app, const ChatSession& chat, std::string* error_out = nullptr);
	std::size_t RestoreRemoteAcpSessionsAfterRestart(AppState& app, bool initial_restore = true);
	std::size_t RetryPendingRemoteAcpSessionHydration(AppState& app);
	bool FlushPendingChatSaves(AppState& app, bool force = false);
	struct WorkspaceChatSaveTicket;
	enum class WorkspaceChatSaveRevocation { OwnedRevoked, ForeignNewer, NotOwned };
	/// <summary>Schedules a full workspace save with an independently owned acknowledgement.</summary>
	std::shared_ptr<WorkspaceChatSaveTicket> ScheduleWorkspaceChatSave(AppState& app, const ChatSession& chat, std::function<void(bool)> completion);
	/// <summary>Revokes only an unchanged captured workspace publication or its own failed retry; returns NotOwned before full snapshot capture, including unstarted force barriers.</summary>
	WorkspaceChatSaveRevocation RevokeWorkspaceChatSave(AppState& app, const std::shared_ptr<WorkspaceChatSaveTicket>& ticket);

	/// <summary>Acknowledges an icon intent only after a matching full snapshot is published.</summary>
	bool DeferChatIconSaveResult(AppState& app, const std::string& chat_id, const CustomIcon& expected, std::function<bool()> owned, std::function<void(bool)> completion);
	/// <summary>Defers an admission acknowledgement until the owning input is durable in its snapshot or queue journal.</summary>
	bool DeferChatSaveResult(AppState& app, const std::string& chat_id, std::function<void(bool)> completion, bool queued_admission = false);
	void ResolvePendingChatSaveResults(AppState& app, const std::string& chat_id, bool saved);
	void FinalizePendingAcpRemoteStopsForExit(AppState& app);
	void FastStopAcpSessionsForExit(AppState& app);

	std::vector<std::string> BuildAcpLaunchArgvForTests(const ChatSession& chat);
	std::string BuildAcpLaunchDetailForTests(const std::filesystem::path& workspace_root, const ChatSession& chat);
	std::string BuildAcpLaunchDetailForTests(const AppState& app, const std::filesystem::path& workspace_root, const ChatSession& chat);
	std::string BuildAcpSetModeRequestForTests(int request_id, const std::string& session_id, const std::string& mode_id);
	std::string BuildAcpSetModelRequestForTests(int request_id, const std::string& session_id, const std::string& model_id);
	std::string BuildCodexUserInputResponseForTests(const std::string& request_id_json, const std::map<std::string, std::vector<std::string>>& answers);
	std::string ResolveAcpSessionResumeIdForTests(const AppState& app, const ChatSession& chat);
	bool ProcessAcpLineForTests(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& line);
	bool UpdateAcpStaleWaitForTests(AcpSessionState& session, double now_seconds);
	std::string AutoApproveOptionIdForTests(const AcpPendingPermissionState& pending);
	bool ResumeStalledGoalLoopForTests(AppState& app, AcpSessionState& session, ChatSession& chat, double now_seconds);
	double AcpReconnectDelaySecondsForTests(int attempt);
	void ScheduleAcpReconnectForTests(AcpSessionState& session, double now_seconds);
	void RecordAcpReconnectFailureForTests(AppState& app, AcpSessionState& session,
	                                       ChatSession& chat, double now_seconds,
	                                       const std::string& error);

} // namespace uam
