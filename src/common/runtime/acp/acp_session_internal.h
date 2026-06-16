#pragma once

#include "common/runtime/app_time.h"
#include "common/state/app_state.h"
#include "common/provider/provider_profile.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>

namespace uam::acp_detail
{

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

} // namespace uam::acp_detail
