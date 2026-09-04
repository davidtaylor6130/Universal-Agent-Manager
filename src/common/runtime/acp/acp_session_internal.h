#pragma once

#include "app/goal_service.h"
#include "common/provider/provider_runtime.h"
#include "common/runtime/app_time.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/state/app_state.h"
#include "common/provider/provider_profile.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace uam::acp_detail
{

// Plain data: ACP failure detail bag (used by FormatAcpFailureMessage)
struct AcpFailureDetails
{
	std::string method;
	std::string request_id;
	bool has_code = false;
	int code = 0;
	std::string message;
	bool has_detail = false;
};


const char* RuntimeDisplayName(const AcpSessionState& session);
std::string MessageProviderId(const AcpSessionState& session);
std::string AcpTimestampNow();
std::string ProviderStructuredProtocolOrDefault(const ProviderProfile& provider);

// Diagnostic string utilities
inline constexpr std::size_t kMaxAcpLogFieldBytes = 512;
inline constexpr std::size_t kMaxAcpDiagnosticEntries = 80;
inline constexpr std::size_t kMaxAcpDiagnosticFieldBytes = 4096;
inline constexpr std::size_t kMaxAcpDiagnosticDetailBytes = 8192;

std::string CapDiagnosticString(const std::string& value, std::size_t max_bytes);
std::string SanitizeLogField(const std::string& value);
std::string QuoteLogField(const std::string& value);
std::string AcpProcessHandleLabel(const AcpSessionState& session);
void AppendAcpLogField(std::ostringstream& out, const char* name, const std::string& value);
void AppendQuotedAcpLogField(std::ostringstream& out, const char* name, const std::string& value);

std::string JsonRpcIdToDiagnosticString(const nlohmann::json& id);
nlohmann::json JsonRpcIdOrNull(const nlohmann::json& message);
std::string FormatAcpDiagnosticLogLine(const AcpSessionState& session, const AcpDiagnosticEntryState& entry);
// clang-format off
void AppendAcpDiagnostic(
    AcpSessionState& session,
    const std::string& event,
    const std::string& reason,
    const std::string& method = "",
    const std::string& request_id = "",
    bool has_code = false,
    int code = 0,
    const std::string& message = "",
    const std::string& detail = "");
// clang-format on
const char* InvalidResumeProviderLabel(const AcpSessionState& session);
std::string InvalidResumeDiagnosticReason(const AcpSessionState& session);
void AppendInvalidResumeDiagnostic(AcpSessionState& session, const std::string& raw_resume_id);
bool MarkAcpRuntimeActivity(AcpSessionState& session, double now_seconds = GetAppTimeSeconds());

// JSON field extraction helpers
std::string JsonDiagnosticStringValue(const nlohmann::json& object, const char* key);
std::string JsonDiagnosticStringValueOr(const nlohmann::json& object, const char* key, const std::string& fallback);
bool JsonBooleanValueOr(const nlohmann::json& object, const char* key, bool fallback);
int JsonIntegerValueOr(const nlohmann::json& object, const char* key, int fallback);
nlohmann::json JsonObjectValue(const nlohmann::json& object, const char* key);
nlohmann::json JsonArrayValue(const nlohmann::json& object, const char* key);
std::string RecentStderrTail(const AcpSessionState& session);
std::string CodexTurnErrorMessage(const nlohmann::json& error);
std::string CodexTurnErrorDetails(const AcpSessionState& session, const nlohmann::json& params, const nlohmann::json& error);
std::string FormatAcpFailureMessage(const AcpSessionState& session, const AcpFailureDetails& details);

// ACP lifecycle state string constants
inline constexpr const char* kAcpLifecycleStarting = "starting";
inline constexpr const char* kAcpLifecycleReady = "ready";
inline constexpr const char* kAcpLifecycleProcessing = "processing";
inline constexpr const char* kAcpLifecycleWaitingPermission = "waitingPermission";
inline constexpr const char* kAcpLifecycleWaitingUserInput = "waitingUserInput";
inline constexpr const char* kAcpLifecycleStopped = "stopped";
inline constexpr const char* kAcpLifecycleError = "error";

// Replay and session sizing constants
inline constexpr std::size_t kMaxRecentStderrBytes = 16 * 1024;
inline constexpr std::size_t kMinAssistantReplayPrefixBytes = 32;
inline constexpr double kAcpStaleWaitSeconds = 120.0;
inline constexpr double kGoalLoopResumeIdleSeconds = 5.0;

// Goal turn kind labels
inline constexpr std::string_view kGoalTurnKindNone = "";
inline constexpr std::string_view kGoalTurnKindWorkerContinuation = "worker_continuation";
inline constexpr std::string_view kGoalTurnKindReview = "review";

// ACP failure detail bag for load retries
struct AcpInvalidLoadRetryDetails
{
	AcpFailureDetails failure;
	std::string error_data;
	std::string detail_text;
	std::string formatted_error;
};

// ACP wire I/O
bool WriteAcpMessage(AcpSessionState& session, const nlohmann::json& message, std::string* error_out = nullptr);

// Permission handler helpers
void SendJsonRpcError(AcpSessionState& session, const nlohmann::json& id, int code, const std::string& message);
nlohmann::json BuildGenericPermissionOutcomeResult(const std::string& option_id, bool cancelled);
bool SendPermissionResponse(AcpSessionState& session, const std::string& request_id_json, const std::string& option_id, bool cancelled, std::string* error_out = nullptr);
bool IsRejectPermissionOption(const std::string& id, const std::string& name, const std::string& kind);
bool IsAcceptPermissionOption(const std::string& id, const std::string& name, const std::string& kind);
std::string AutoApproveOptionId(const AcpPendingPermissionState& pending);
std::string RejectPermissionOptionId(const AcpPendingPermissionState& pending);
struct PermissionReviewDecision
{
	std::string decision;
	std::string reason;
};
std::optional<PermissionReviewDecision> ParsePermissionReviewOutput(std::string_view output);
bool TryAutoApprovePendingPermission(AppState& app, AcpSessionState& session, const ChatSession& chat, std::string* error_out = nullptr);
void QueueAcpPermission(AppState& app, AcpSessionState& session, ChatSession& chat, AcpPendingPermissionState pending);
void AdvanceAcpPermissionQueue(AppState& app, AcpSessionState& session, const ChatSession& chat, std::string* error_out = nullptr);
bool PollPermissionReviewTasks(AppState& app);
void StopPermissionReviewTasks(AppState& app, std::string_view chat_id = {}, std::string_view request_id_json = {});
void CancelPendingAcpPermissions(AcpSessionState& session, std::string* error_out = nullptr);
void AppendIgnoredRequestDuringCancelDiagnostic(AcpSessionState& session, const nlohmann::json& message, const char* reason, const char* diagnostic_message);
nlohmann::json BuildCodexUserInputResponse(const std::string& request_id_json, const std::map<std::string, std::vector<std::string>>& answers);
bool SendCodexUserInputResponse(AcpSessionState& session, const std::string& request_id_json, const std::map<std::string, std::vector<std::string>>& answers, std::string* error_out = nullptr);
void ApplyCommandSafetyDecision(const AppState& app, const ChatSession& chat, AcpPendingPermissionState& pending);
void HandlePermissionRequest(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message);

// Tool call state helpers
AcpToolCallState& UpsertToolCall(AcpSessionState& session, const std::string& id);
bool LooksLikeSubAgentTool(const nlohmann::json& update, const AcpToolCallState& tool_call, const IProviderRuntime& runtime);
void ApplySubAgentMetadata(AcpToolCallState& tool_call, const nlohmann::json& update, const IProviderRuntime& runtime);

// Message sync helpers
Message& EnsureAssistantMessage(ChatSession& chat, AcpSessionState& session);
bool AppendThoughtChunk(ChatSession& chat, AcpSessionState& session, const std::string& chunk);
std::string AppendAssistantChunk(ChatSession& chat, AcpSessionState& session, const std::string& chunk);
ToolCall PersistedToolCallFromAcpToolCall(const AcpToolCallState& tool_call);
bool UpsertPersistedToolCall(std::vector<ToolCall>& tool_calls, const AcpToolCallState& tool_call);
bool SyncAcpToolCallsToAssistantMessage(ChatSession& chat, AcpSessionState& session, bool create_if_missing);
bool FinalizeActiveAcpToolCallsAsCancelled(ChatSession& chat, AcpSessionState& session);
bool FinalizeActiveAcpToolCallsAsFailed(ChatSession& chat, AcpSessionState& session);
MessagePlanEntry PersistedPlanEntryFromAcpPlanEntry(const AcpPlanEntryState& entry);
std::vector<MessagePlanEntry> PersistedPlanEntriesFromAcpPlanEntries(const std::vector<AcpPlanEntryState>& entries);
bool MessagePlanEntriesEqual(const std::vector<MessagePlanEntry>& lhs, const std::vector<MessagePlanEntry>& rhs);
bool SyncAcpPlanToAssistantMessage(ChatSession& chat, AcpSessionState& session, bool create_if_missing);

// Goal loop helpers
std::string MessageTextForGoalReview(const ChatSession& chat, int index);
std::string GoalTextPrefixForDiagnostics(const std::string& text);
std::string GoalLoopDiagnosticDetail(const AcpSessionState& session, const std::string& goal_id, const std::string& text = "");
void AppendGoalLoopDiagnostic(AcpSessionState& session, const std::string& reason, const std::string& goal_id, const std::string& text = "");
int64_t EstimateGoalTurnTokens(const ChatSession& chat, const AcpSessionState& session);
bool CanQueueGoalInternalPrompt(const AcpSessionState& session);
std::string NormalizeGoalNextPrompt(const std::string& prompt);
bool GoalBlockerStopsImmediately(const std::string& blocker_kind);
void ApplyGoalProgressUpdate(Goal& goal, const GoalService::ReviewDecision& decision);

// Turn event helpers
bool MessageBlocksEqual(const std::vector<MessageBlock>& lhs, const std::vector<MessageBlock>& rhs);
bool PersistableTurnEvent(const AcpTurnEventState& event);
bool CanMergeTurnEventWithLastBlock(const AcpTurnEventState& event, const std::vector<MessageBlock>& blocks);
bool HasMatchingTurnBlock(const std::vector<MessageBlock>& blocks, const AcpTurnEventState& event);
std::vector<MessageBlock> MessageBlocksFromTurnEvents(const AcpSessionState& session);
void RestoreTurnEventsFromMessageBlocks(AcpSessionState& session, const Message& message);
bool SyncMessageBlocksFromTurnEvents(Message& message, const AcpSessionState& session);
Message* CurrentAssistantMessage(ChatSession& chat, const AcpSessionState& session);
const Message* CurrentAssistantMessage(const ChatSession& chat, const AcpSessionState& session);
bool SyncCurrentAssistantMessageBlocksFromTurnEvents(ChatSession& chat, AcpSessionState& session);
void AppendAssistantTextTurnEvent(AcpSessionState& session, const std::string& chunk);
bool AppendThoughtTurnEvent(AcpSessionState& session, const std::string& chunk);
bool HasTurnToolEvent(const AcpSessionState& session, const std::string& tool_call_id);
void AppendToolTurnEventIfNeeded(AcpSessionState& session, const std::string& tool_call_id);
void AppendPermissionTurnEventIfNeeded(AcpSessionState& session, const std::string& request_id_json, const std::string& tool_call_id);
void AppendUserInputTurnEventIfNeeded(AcpSessionState& session, const std::string& request_id_json, const std::string& item_id);
void AppendPlanTurnEventIfNeeded(AcpSessionState& session);

// Replay state helpers
void RememberAssistantReplayPrefixes(AcpSessionState& session, const ChatSession& chat, int turn_user_message_index);
void RememberLoadHistoryReplayUpdates(AcpSessionState& session, const ChatSession& chat, int turn_user_message_index);
std::string StripKnownAssistantReplayPrefix(const AcpSessionState& session, const std::string& text);
std::string AssistantDeltaForIncomingText(const AcpSessionState& session, const std::string& current_assistant_text, const std::string& incoming_text);
bool ReplayUpdateTypesCompatible(const std::string& expected, const std::string& incoming);
bool ReplayToolUpdateMatches(const AcpReplayUpdateState& expected, const nlohmann::json& update, const std::string& update_type);
bool ReplayTextUpdateMatches(const AcpReplayUpdateState& expected, const std::string& update_type, const std::string& incoming_text, std::string& live_suffix);
bool TryConsumeLoadHistoryReplayUpdate(AcpSessionState& session, const nlohmann::json& update, const std::string& update_type, const std::string& incoming_text, std::string& live_text);
std::string JsonRpcIdToStableString(const nlohmann::json& id);
nlohmann::json StableStringToJsonRpcId(const std::string& request_id_json);
int JsonRpcNumericId(const nlohmann::json& id);
void AppendRecentStderr(AcpSessionState& session, const std::string& chunk);

// Claude content text helpers
nlohmann::json BuildClaudeInputMessage(const std::string& text);
std::string ContentTextFromJson(const nlohmann::json& content);
std::string ToolCallContentTextFromJson(const nlohmann::json& tool_call);
std::string ClaudeContentTextFromMessage(const nlohmann::json& message);
std::string StripLeadingLineBreaks(std::string value);
bool StartsWithLineBreak(const std::string& value);
void AppendThoughtText(std::string& target, const std::string& chunk, bool starts_new_block);

// Message diagnostic detail helpers
std::string AcpMessageMethodForDiagnostics(const nlohmann::json& message);
std::string AcpMessageRequestIdForDiagnostics(const nlohmann::json& message);
std::string PromptLengthDetail(const nlohmann::json& params);
std::string AcpMessageDetailForDiagnostics(const nlohmann::json& message);

// Protocol request builders
nlohmann::json BuildInitializeRequest(int request_id);
nlohmann::json BuildCodexInitializeRequest(int request_id);
nlohmann::json BuildCodexInitializedNotification();
nlohmann::json BuildCodexModelListRequest(int request_id);
nlohmann::json BuildCodexRateLimitsReadRequest(int request_id);
nlohmann::json BuildNewSessionRequest(int request_id, const std::string& cwd, const ChatSession* chat = nullptr);
nlohmann::json BuildLoadSessionRequest(int request_id, const std::string& session_id, const std::string& cwd, const ChatSession* chat = nullptr);
nlohmann::json BuildResumeSessionRequest(int request_id, const std::string& session_id,
	                                     const std::string& cwd, const ChatSession* chat = nullptr);
nlohmann::json BuildCodexThreadStartRequest(int request_id, const ChatSession& chat, const std::string& cwd);
nlohmann::json BuildCodexThreadResumeRequest(int request_id, const ChatSession& chat, const std::string& cwd);
nlohmann::json BuildGeminiSessionSetupRequest(int request_id, const ChatSession& chat, const std::string& cwd, bool load_session_supported);
nlohmann::json BuildCodexSessionSetupRequest(int request_id, const ChatSession& chat, const std::string& cwd);
nlohmann::json BuildPromptRequest(int request_id, const std::string& session_id, const std::string& text, const std::string& reasoning_effort = "");
nlohmann::json BuildCodexTurnStartRequest(int request_id, const std::string& thread_id, const std::string& text, const ChatSession& chat, const std::string& active_model_id);
nlohmann::json BuildCancelNotification(const std::string& session_id);
nlohmann::json BuildCodexTurnInterruptRequest(int request_id, const std::string& thread_id, const std::string& turn_id);
nlohmann::json BuildSetConfigOptionRequest(int request_id, const std::string& session_id, const std::string& config_id, const std::string& value);
nlohmann::json BuildSetModeRequest(int request_id, const std::string& session_id, const std::string& mode_id);
nlohmann::json BuildSetModelRequest(int request_id, const std::string& session_id, const std::string& model_id);

// Protocol session state helpers
bool TextContainsAnyCaseInsensitive(std::string_view text, std::initializer_list<std::string_view> needles);
bool WordMatchesAnyCaseInsensitive(std::string_view text, std::initializer_list<std::string_view> words);
bool AcpSessionCanSendQueuedPrompt(const AcpSessionState& session);
bool GeminiErrorLooksLikeInvalidSessionId(const std::string& error_message, const std::string& error_data);
std::string AppApprovalModeId(std::string_view mode_id);
std::string ProviderApprovalModeId(const AcpSessionState& session, const std::string& mode_id);

// Launch argv + request id helpers
std::vector<std::string> BuildAcpLaunchArgv(const ProviderProfile& provider, const ChatSession& chat);
std::string JoinAcpArgvForDiagnostics(const std::vector<std::string>& argv);
std::string AcpWorkingDirectoryString(const std::filesystem::path& workspace_root);
std::string BuildAcpLaunchDetail(const ProviderProfile& provider, const AppState& app, const std::filesystem::path& workspace_root, const ChatSession& chat);
std::string BuildAcpLaunchDetail(const AppState& app, const std::filesystem::path& workspace_root, const ChatSession& chat);
int NextAcpRequestId(AcpSessionState& session, const std::string& method);

// Resume-id resolution per provider
std::string ValidCodexResumeId(const ChatSession& chat);
std::string ValidGeminiResumeId(const ChatSession& chat);
std::string ValidGenericAcpResumeId(const ChatSession& chat);
std::string ResolvedAcpResumeIdForChat(const AppState& app, const ChatSession& chat);

// Wait-state management
void ResetAcpWaitState(AcpSessionState& session);
void ResetAcpPendingInteractionState(AcpSessionState& session);
void ResetAcpTurnStreamState(AcpSessionState& session);
void ClearAcpStartupModelRequest(AcpSessionState& session);
void ClearAcpReasoningChangeRequest(AcpSessionState& session);
void ClearAcpConfigOptionChangeRequest(AcpSessionState& session);
void ClearAcpModeChangeRequest(AcpSessionState& session);
bool RollbackAcpModeChange(AcpSessionState& session, ChatSession& chat);
void ClearAcpModelChangeRequest(AcpSessionState& session);
bool UpdateAcpConfigOptions(AcpSessionState& session, const nlohmann::json& config_options);
bool UpdateCopilotReasoningFromConfigOptions(AcpSessionState& session, ChatSession& chat, const nlohmann::json& config_options);
bool ReconcileCopilotReasoningEffort(AppState& app, AcpSessionState& session, ChatSession& chat);
void BeginAcpPendingWait(AcpSessionState& session, std::string_view lifecycle_state);
void ClearAcpPendingWait(AcpSessionState& session);
std::string ActiveAcpWaitRequestId(const AcpSessionState& session);
std::string ActiveAcpWaitToolId(const AcpSessionState& session);

// Session lifecycle helpers
bool UpdateAcpStaleWait(AcpSessionState& session, double now_seconds);
bool SendInitialize(AcpSessionState& session, std::string* error_out = nullptr);
void ResetAcpRuntimeState(AcpSessionState& session);
AcpSessionState& EnsureAcpSessionForChat(AppState& app, const ChatSession& chat);
bool StopAcpProcessForRestart(AppState& app, AcpSessionState& session, const ChatSession& chat);
bool FailAcpSessionSetupWrite(AppState& app, AcpSessionState& session, ChatSession& chat, const std::string& fallback_message);
bool StartAcpProcessForChat(AppState& app, AcpSessionState& session, ChatSession& chat, std::string* error_out = nullptr);
bool SendSessionSetupIfReady(AppState& app, AcpSessionState& session, ChatSession& chat);
bool RetrySessionNewAfterInvalidLoad(AppState& app, AcpSessionState& session, ChatSession& chat, const AcpInvalidLoadRetryDetails& details);
bool SendStartupModeIfNeeded(AcpSessionState& session, const ChatSession& chat);
bool SendStartupModelIfNeeded(AcpSessionState& session, const ChatSession& chat);
bool SendQueuedPromptIfReady(AppState& app, AcpSessionState& session, ChatSession& chat);
bool SendDeferredCodexInterruptIfReady(AcpSessionState& session);
bool ResumeQueuedUserPromptsAfterSessionSetup(AppState& app, AcpSessionState& session, ChatSession& chat);
bool SaveChatQuietly(AppState& app, const ChatSession& chat);
void ScheduleChatSave(AppState& app, const ChatSession& chat, double delay_seconds = 0.5);
bool SetChatNativeSessionIdIfChanged(ChatSession& chat, std::string_view session_id);
void SyncResolvedNativeSessionIdForChat(AppState& app, const ChatSession& chat, std::string_view session_id, std::string_view previous_session_id = {});
void CompletePromptTurn(AcpSessionState& session, std::string_view lifecycle_state);
bool QueueGoalInternalPrompt(AppState& app, AcpSessionState& session, ChatSession& chat,
                             const std::string& prompt, bool review_turn,
                             const std::string& model_id = {}, bool fresh_session = false);
bool ScheduleTurnCheckpointPreflight(AppState& app, AcpSessionState& session, const ChatSession& chat);
void ClearGoalReviewState(AcpSessionState& session);
void FailAcpTurnOrSession(AcpSessionState& session, ChatSession* chat,
	                      const std::string& message);
void MarkAcpChatUnseenIfBackground(AppState& app, const ChatSession& chat);
void InvalidateAcpTransport(AppState& app, AcpSessionState& session, ChatSession& chat,
                           const std::string& message);
void RecoverDisconnectedRemoteAcpTransport(AppState& app, AcpSessionState& session,
                                           ChatSession& chat, const std::string& message);

// ACP response handler helpers
std::string PendingRequestSummary(const AcpSessionState& session);
std::string ErrorDataForDiagnostics(const nlohmann::json& error);
void UpdateAcpModesFromJson(AcpSessionState& session, const nlohmann::json& modes);
void UpdateAcpModelsFromJson(AcpSessionState& session, const nlohmann::json& models);
void HandleAcpRequest(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message);
bool ReplayPersistedInteractionResponseIfMatched(
    AppState& app, AcpSessionState& session, ChatSession& chat,
    const nlohmann::json& request);
void HandleAcpResponse(AppState& app, AcpSessionState& session, ChatSession& chat, const nlohmann::json& message);

} // namespace uam::acp_detail

// Cross-reference: avoid pulling in CEF-dependent acp_session_runtime.h from lifecycle code
namespace uam
{
AcpSessionState* FindAcpSessionForChat(AppState& app, const std::string& chat_id);
const AcpSessionState* FindAcpSessionForChat(const AppState& app, const std::string& chat_id);
} // namespace uam
