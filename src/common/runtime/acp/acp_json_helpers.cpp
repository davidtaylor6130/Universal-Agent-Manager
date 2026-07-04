#include "common/runtime/acp/acp_session_internal.h"

#include "common/utils/nlohmann_json_utils.h"
#include "common/utils/string_utils.h"

#include <sstream>
#include <string>
#include <string_view>

namespace uam::acp_detail
{

std::string RecentStderrTail(const AcpSessionState& session)
{
	return CapDiagnosticString(session.recent_stderr, kMaxAcpDiagnosticDetailBytes);
}

std::string JsonDiagnosticStringValue(const nlohmann::json& object, const char* key)
{
	const nlohmann::json* value = uam::nlohmann_json::FindField(object, key);
	if (value == nullptr || value->is_null())
	{
		return "";
	}
	if (value->is_string())
	{
		return value->get_ref<const std::string&>();
	}
	if (value->is_boolean())
	{
		return value->get<bool>() ? "true" : "false";
	}
	if (value->is_number_integer() || value->is_number_unsigned() || value->is_number_float())
	{
		return value->dump();
	}
	return CapDiagnosticString(value->dump(), kMaxAcpDiagnosticDetailBytes);
}

std::string JsonDiagnosticStringValueOr(const nlohmann::json& object, const char* key, const std::string& fallback)
{
	const std::string value = JsonDiagnosticStringValue(object, key);
	return uam::strings::NonEmptyOrFallback(value, fallback);
}

bool JsonBooleanValueOr(const nlohmann::json& object, const char* key, bool fallback)
{
	const nlohmann::json* value = uam::nlohmann_json::FindField(object, key);
	if (value == nullptr || value->is_null())
	{
		return fallback;
	}
	if (value->is_boolean())
	{
		return value->get<bool>();
	}
	if (value->is_string())
	{
		const std::string_view text = value->get_ref<const std::string&>();
		if (text == "true")
		{
			return true;
		}
		if (text == "false")
		{
			return false;
		}
	}
	return fallback;
}

int JsonIntegerValueOr(const nlohmann::json& object, const char* key, int fallback)
{
	return uam::nlohmann_json::IntFieldStrict(object, key).value_or(fallback);
}

nlohmann::json JsonObjectValue(const nlohmann::json& object, const char* key)
{
	const nlohmann::json* value = uam::nlohmann_json::FindObjectField(object, key);
	return value == nullptr ? nlohmann::json::object() : *value;
}

nlohmann::json JsonArrayValue(const nlohmann::json& object, const char* key)
{
	const nlohmann::json* value = uam::nlohmann_json::FindArrayField(object, key);
	return value == nullptr ? nlohmann::json::array() : *value;
}

std::string CodexTurnErrorMessage(const nlohmann::json& error)
{
	if (!error.is_object())
	{
		return "Codex app-server error.";
	}
	const std::string message = JsonDiagnosticStringValue(error, "message");
	return uam::strings::NonEmptyOrFallback(message, "Codex app-server error.");
}

std::string CodexTurnErrorDetails(const AcpSessionState& session, const nlohmann::json& params, const nlohmann::json& error)
{
	std::ostringstream detail;
	bool has_detail = false;
	const auto append_line = [&](const std::string& line)
	{
		if (line.empty())
		{
			return;
		}
		if (has_detail)
		{
			detail << "\n";
		}
		detail << line;
		has_detail = true;
	};

	if (const nlohmann::json* will_retry = uam::nlohmann_json::FindField(params, "willRetry");
	    will_retry != nullptr && will_retry->is_boolean())
	{
		append_line(std::string("willRetry=") + (will_retry->get<bool>() ? "true" : "false"));
	}
	const std::string thread_id = JsonDiagnosticStringValue(params, "threadId");
	if (!thread_id.empty())
	{
		append_line("threadId=" + thread_id);
	}
	const std::string turn_id = JsonDiagnosticStringValue(params, "turnId");
	if (!turn_id.empty())
	{
		append_line("turnId=" + turn_id);
	}
	if (error.is_object())
	{
		const std::string additional_details = JsonDiagnosticStringValue(error, "additionalDetails");
		if (!additional_details.empty())
		{
			append_line("additionalDetails=" + additional_details);
		}
		const nlohmann::json* codex_error_info = uam::nlohmann_json::FindField(error, "codexErrorInfo");
		if (codex_error_info != nullptr && !codex_error_info->is_null())
		{
			append_line("codexErrorInfo=" + CapDiagnosticString(codex_error_info->dump(), kMaxAcpDiagnosticDetailBytes));
		}
	}
	if (!session.recent_stderr.empty())
	{
		append_line("stderr_tail=" + RecentStderrTail(session));
	}
	return detail.str();
}

std::string FormatAcpFailureMessage(const AcpSessionState& session, const AcpFailureDetails& details)
{
	std::ostringstream out;
	out << RuntimeDisplayName(session) << " " << uam::strings::NonEmptyOrFallback(details.method, "request") << " failed";
	if (!details.request_id.empty() || details.has_code)
	{
		std::vector<std::string> detail_parts;
		if (!details.request_id.empty())
		{
			detail_parts.push_back("id=" + details.request_id);
		}
		if (details.has_code)
		{
			detail_parts.push_back("code=" + std::to_string(details.code));
		}
		out << " (" << uam::strings::JoinNonEmpty(detail_parts, ", ") << ")";
	}
	out << ": " << (details.message.empty() ? (std::string(RuntimeDisplayName(session)) + " request failed.") : details.message);
	if (details.has_detail)
	{
		out << " See diagnostics/stderr details.";
	}
	return out.str();
}

} // namespace uam::acp_detail
