#include "common/runtime/acp/acp_session_internal.h"

#include "common/provider/provider_ids.h"
#include "common/provider/provider_profile_constants.h"
#include "common/provider/runtime/provider_build_config.h"
#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <iostream>
#include <sstream>
#include <string>

namespace uam::acp_detail
{

bool AcpSessionMatchesProvider(const AcpSessionState& session, std::string_view protocol_kind, std::string_view canonical_provider_id)
{
	return session.protocol_kind == protocol_kind || uam::provider_ids::IsCliProviderAliasOf(session.provider_id, canonical_provider_id);
}

bool IsCodexSession(const AcpSessionState& session)
{
	return AcpSessionMatchesProvider(session, uam::provider_profile_constants::kProtocolCodexAppServer, uam::provider_ids::kCodexCli);
}

bool IsClaudeSession(const AcpSessionState& session)
{
	return AcpSessionMatchesProvider(session, uam::provider_profile_constants::kProtocolClaudeCodeStreamJson, uam::provider_ids::kClaudeCli);
}

bool IsOpenCodeSession(const AcpSessionState& session)
{
	return AcpSessionMatchesProvider(session, uam::provider_profile_constants::kProtocolOpenCodeAcp, uam::provider_ids::kOpenCodeCli);
}

bool IsCopilotSession(const AcpSessionState& session)
{
	return AcpSessionMatchesProvider(session, uam::provider_profile_constants::kProtocolCopilotAcp, uam::provider_ids::kCopilotCli);
}

bool IsGenericAcpSession(const AcpSessionState& session)
{
	return IsOpenCodeSession(session) || IsCopilotSession(session);
}

const char* RuntimeDisplayName(const AcpSessionState& session)
{
	if (IsCodexSession(session))
	{
		return "Codex app-server";
	}
	if (IsClaudeSession(session))
	{
		return "Claude stream-json";
	}
	if (IsOpenCodeSession(session))
	{
		return "OpenCode ACP";
	}
	if (IsCopilotSession(session))
	{
		return "GitHub Copilot ACP";
	}
	return "Gemini ACP";
}

std::string MessageProviderId(const AcpSessionState& session)
{
	return uam::strings::NonEmptyOrFallback(session.provider_id, provider_build_config::FirstEnabledProviderId());
}

std::string AcpTimestampNow()
{
	return uam::time::IsoUtcTimestampNow();
}

std::string ProviderStructuredProtocolOrDefault(const ProviderProfile& provider)
{
	return uam::provider_profile_constants::StructuredProtocolOrGemini(provider.structured_protocol);
}

std::string CapDiagnosticString(const std::string& value, std::size_t max_bytes)
{
	if (value.size() <= max_bytes)
	{
		return value;
	}

	std::ostringstream out;
	out << value.substr(0, max_bytes) << "... [truncated " << (value.size() - max_bytes) << " bytes]";
	return out.str();
}

std::string SanitizeLogField(const std::string& value)
{
	std::string sanitized = CapDiagnosticString(value, kMaxAcpLogFieldBytes);
	for (char& ch : sanitized)
	{
		if (ch == '\n' || ch == '\r' || ch == '\t')
		{
			ch = ' ';
		}
	}
	return sanitized;
}

std::string QuoteLogField(const std::string& value)
{
	std::string quoted;
	quoted.reserve(value.size() + 2);
	quoted.push_back('"');
	for (const char ch : SanitizeLogField(value))
	{
		if (ch == '"' || ch == '\\')
		{
			quoted.push_back('\\');
		}
		quoted.push_back(ch);
	}
	quoted.push_back('"');
	return quoted;
}

std::string AcpProcessHandleLabel(const AcpSessionState& session)
{
#if defined(_WIN32)
	if (session.process_info.dwProcessId != 0)
	{
		return std::to_string(static_cast<unsigned long long>(session.process_info.dwProcessId));
	}
#elif defined(__APPLE__)
	if (session.child_pid > 0)
	{
		return std::to_string(static_cast<long long>(session.child_pid));
	}
#endif
	return uam::strings::NonEmptyOrFallback(session.last_process_id, "0");
}

void AppendAcpLogField(std::ostringstream& out, const char* name, const std::string& value)
{
	if (!value.empty())
	{
		out << ' ' << name << '=' << value;
	}
}

void AppendQuotedAcpLogField(std::ostringstream& out, const char* name, const std::string& value)
{
	if (!value.empty())
	{
		out << ' ' << name << '=' << QuoteLogField(value);
	}
}

std::string JsonRpcIdToDiagnosticString(const nlohmann::json& id)
{
	if (id.is_null())
	{
		return "";
	}
	if (id.is_string())
	{
		return id.get<std::string>();
	}
	return id.dump();
}

nlohmann::json JsonRpcIdOrNull(const nlohmann::json& message)
{
	return uam::nlohmann_json::ValueOrNull(uam::nlohmann_json::FindField(message, "id"));
}

std::string FormatAcpDiagnosticLogLine(const AcpSessionState& session, const AcpDiagnosticEntryState& entry)
{
	std::ostringstream out;
	out << "[acp-diag]";
	AppendAcpLogField(out, "event", entry.event);
	AppendAcpLogField(out, "reason", entry.reason);
	AppendAcpLogField(out, "chat_id", session.chat_id);
	AppendAcpLogField(out, "session_id", session.session_id);
	AppendAcpLogField(out, "process_id", AcpProcessHandleLabel(session));
	AppendAcpLogField(out, "lifecycle_state", entry.lifecycle_state);
	AppendAcpLogField(out, "method", entry.method);
	AppendAcpLogField(out, "request_id", entry.request_id);
	if (entry.has_code)
	{
		out << " code=" << entry.code;
	}
	AppendQuotedAcpLogField(out, "message", entry.message);
	AppendQuotedAcpLogField(out, "detail", entry.detail);
	AppendAcpLogField(out, "t", entry.time);
	return out.str();
}

void AppendAcpDiagnostic(
    AcpSessionState& session,
    const std::string& event,
    const std::string& reason,
    const std::string& method,
    const std::string& request_id,
    bool has_code,
    int code,
    const std::string& message,
    const std::string& detail)
{
	AcpDiagnosticEntryState entry;
	entry.time = AcpTimestampNow();
	entry.event = CapDiagnosticString(event, kMaxAcpDiagnosticFieldBytes);
	entry.reason = CapDiagnosticString(reason, kMaxAcpDiagnosticFieldBytes);
	entry.method = CapDiagnosticString(method, kMaxAcpDiagnosticFieldBytes);
	entry.request_id = CapDiagnosticString(request_id, kMaxAcpDiagnosticFieldBytes);
	entry.has_code = has_code;
	entry.code = code;
	entry.message = CapDiagnosticString(message, kMaxAcpDiagnosticFieldBytes);
	entry.detail = CapDiagnosticString(detail, kMaxAcpDiagnosticDetailBytes);
	entry.lifecycle_state = session.lifecycle_state;

	std::cerr << FormatAcpDiagnosticLogLine(session, entry) << '\n';

	session.diagnostics.push_back(std::move(entry));
	if (session.diagnostics.size() > kMaxAcpDiagnosticEntries)
	{
		session.diagnostics.erase(session.diagnostics.begin(), session.diagnostics.begin() + static_cast<std::ptrdiff_t>(session.diagnostics.size() - kMaxAcpDiagnosticEntries));
	}
}

const char* InvalidResumeProviderLabel(const AcpSessionState& session)
{
	if (IsOpenCodeSession(session))
	{
		return "OpenCode";
	}
	if (IsCopilotSession(session))
	{
		return "GitHub Copilot";
	}
	return "Gemini";
}

const char* InvalidResumeDiagnosticReason(const AcpSessionState& session)
{
	if (IsOpenCodeSession(session))
	{
		return "opencode_invalid_resume_id_ignored";
	}
	if (IsCopilotSession(session))
	{
		return "copilot_invalid_resume_id_ignored";
	}
	return "gemini_invalid_resume_id_ignored";
}

void AppendInvalidResumeDiagnostic(AcpSessionState& session, const std::string& raw_resume_id)
{
	const std::string message = "Ignoring invalid " + std::string(InvalidResumeProviderLabel(session)) + " session id and starting a new session.";
	AppendAcpDiagnostic(session, "session_setup", InvalidResumeDiagnosticReason(session), "", "", false, 0, message, "nativeSessionId=" + raw_resume_id);
}

bool MarkAcpRuntimeActivity(AcpSessionState& session, double now_seconds)
{
	session.last_runtime_activity_time_s = now_seconds;
	if (session.wait_is_stale)
	{
		session.wait_is_stale = false;
		session.wait_stale_reason.clear();
		return true;
	}
	return false;
}

} // namespace uam::acp_detail
