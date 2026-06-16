#pragma once

#include "common/state/app_state.h"
#include "common/provider/provider_profile.h"

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

std::string CapDiagnosticString(const std::string& value, std::size_t max_bytes);
std::string SanitizeLogField(const std::string& value);
std::string QuoteLogField(const std::string& value);
std::string AcpProcessHandleLabel(const AcpSessionState& session);
void AppendAcpLogField(std::ostringstream& out, const char* name, const std::string& value);
void AppendQuotedAcpLogField(std::ostringstream& out, const char* name, const std::string& value);

} // namespace uam::acp_detail
