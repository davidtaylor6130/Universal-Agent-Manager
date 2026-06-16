#pragma once

#include "common/runtime/app_time.h"
#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/state/app_state.h"
#include "common/provider/provider_profile.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
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

// Session-type predicates
bool AcpSessionMatchesProvider(const AcpSessionState& session, std::string_view protocol_kind, std::string_view canonical_provider_id);
bool IsCodexSession(const AcpSessionState& session);
bool IsClaudeSession(const AcpSessionState& session);
bool IsOpenCodeSession(const AcpSessionState& session);
bool IsCopilotSession(const AcpSessionState& session);
bool IsGenericAcpSession(const AcpSessionState& session);
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
const char* InvalidResumeDiagnosticReason(const AcpSessionState& session);
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
nlohmann::json BuildNewSessionRequest(int request_id, const std::string& cwd);
nlohmann::json BuildLoadSessionRequest(int request_id, const std::string& session_id, const std::string& cwd);
nlohmann::json BuildCodexThreadStartRequest(int request_id, const ChatSession& chat, const std::string& cwd);
nlohmann::json BuildCodexThreadResumeRequest(int request_id, const ChatSession& chat, const std::string& cwd);
nlohmann::json BuildGeminiSessionSetupRequest(int request_id, const ChatSession& chat, const std::string& cwd, bool load_session_supported);
nlohmann::json BuildCodexSessionSetupRequest(int request_id, const ChatSession& chat, const std::string& cwd);
nlohmann::json BuildPromptRequest(int request_id, const std::string& session_id, const std::string& text);
nlohmann::json BuildCodexTurnStartRequest(int request_id, const std::string& thread_id, const std::string& text, const ChatSession& chat, const std::string& active_model_id);
nlohmann::json BuildCancelNotification(const std::string& session_id);
nlohmann::json BuildCodexTurnInterruptRequest(int request_id, const std::string& thread_id, const std::string& turn_id);
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
void BeginAcpPendingWait(AcpSessionState& session, std::string_view lifecycle_state);
void ClearAcpPendingWait(AcpSessionState& session);
std::string ActiveAcpWaitRequestId(const AcpSessionState& session);
std::string ActiveAcpWaitToolId(const AcpSessionState& session);

} // namespace uam::acp_detail
