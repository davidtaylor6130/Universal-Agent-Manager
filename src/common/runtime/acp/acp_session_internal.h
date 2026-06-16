#pragma once

#include "common/state/app_state.h"
#include "common/provider/provider_profile.h"

#include <string>
#include <string_view>

namespace uam::acp_detail
{

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

} // namespace uam::acp_detail
