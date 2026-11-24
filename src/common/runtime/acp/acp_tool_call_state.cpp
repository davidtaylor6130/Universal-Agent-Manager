#include "common/runtime/acp/acp_session_internal.h"

#include "common/utils/string_utils.h"

#include <cstddef>
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

AcpToolCallState& RestoreToolCallFromChat(AcpSessionState& session, const ChatSession& chat, const std::string& id)
{
	for (AcpToolCallState& tool : session.tool_calls) if (tool.id == id) return tool;
	AcpToolCallState& tool = UpsertToolCall(session, id);
	for (std::size_t index = 0; index < chat.messages.size(); ++index)
	{
		for (const ToolCall& saved : chat.messages[index].tool_calls)
		{
			if (saved.id != id) continue;
			tool.title = saved.name;
			tool.kind = saved.kind;
			tool.status = saved.status;
			tool.args_json = saved.args_json;
			tool.content = saved.result_text;
			tool.is_sub_agent = saved.is_sub_agent;
			tool.sub_agent_id = saved.sub_agent_id;
			tool.sub_agent_title = saved.sub_agent_title;
			tool.sub_agent_children = saved.sub_agent_children;
			tool.sub_agent_children_authoritative = saved.sub_agent_children_authoritative;
			tool.approval_status = saved.approval_status;
			tool.task_id = saved.task_id;
			tool.child_activity = saved.child_activity;
			session.tool_call_message_indices[id] = static_cast<int>(index);
			return tool;
		}
	}
	return tool;
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
	// The input can arrive on a later update than the one that opened the call.
	if (const auto input = update.find("rawInput"); input != update.end())
		tool_call.declared_timeout_s = runtime.AcpToolDeclaredTimeoutSeconds(tool_call.kind, *input);
	runtime.ApplyAcpToolMetadata(tool_call, update);
}

} // namespace uam::acp_detail
