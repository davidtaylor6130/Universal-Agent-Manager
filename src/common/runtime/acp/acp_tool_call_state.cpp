#include "common/runtime/acp/acp_session_internal.h"

#include "common/utils/string_utils.h"

#include <string>

namespace uam::acp_detail
{

AcpToolCallState& UpsertToolCall(AcpSessionState& session, const std::string& id)
{
	for (AcpToolCallState& tool_call : session.tool_calls)
	{
		if (tool_call.id == id)
		{
			return tool_call;
		}
	}

	AcpToolCallState tool_call;
	tool_call.id = id;
	session.tool_calls.push_back(std::move(tool_call));
	return session.tool_calls.back();
}

bool LooksLikeSubAgentTool(const nlohmann::json& update, const AcpToolCallState& tool_call, const IProviderRuntime& runtime)
{
	if (JsonBooleanValueOr(update, "isSubAgent", false) || JsonBooleanValueOr(update, "subAgent", false))
	{
		return true;
	}

	if (WordMatchesAnyCaseInsensitive(tool_call.kind, {"subagent", "sub-agent"}) ||
	    WordMatchesAnyCaseInsensitive(tool_call.title, {"subagent", "sub-agent"}))
	{
		return true;
	}

	return runtime.ProviderRecognizesSubagentTool(tool_call.title);
}

void ApplySubAgentMetadata(AcpToolCallState& tool_call, const nlohmann::json& update, const IProviderRuntime& runtime)
{
	const std::string sub_agent_id = uam::strings::NonEmptyOrFallback(
	    JsonDiagnosticStringValue(update, "subAgentId"),
	    uam::strings::NonEmptyOrFallback(JsonDiagnosticStringValue(update, "agentId"), JsonDiagnosticStringValue(update, "sessionId")));
	const std::string sub_agent_title = uam::strings::NonEmptyOrFallback(
	    JsonDiagnosticStringValue(update, "subAgentTitle"),
	    uam::strings::NonEmptyOrFallback(JsonDiagnosticStringValue(update, "agentName"), JsonDiagnosticStringValue(update, "agent")));

	if (!sub_agent_id.empty())
	{
		tool_call.sub_agent_id = sub_agent_id;
	}
	if (!sub_agent_title.empty())
	{
		tool_call.sub_agent_title = sub_agent_title;
	}
	if (LooksLikeSubAgentTool(update, tool_call, runtime) || !tool_call.sub_agent_id.empty() || !tool_call.sub_agent_title.empty())
	{
		tool_call.is_sub_agent = true;
	}
}

} // namespace uam::acp_detail
