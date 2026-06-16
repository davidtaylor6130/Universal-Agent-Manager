#include "common/runtime/acp/acp_session_internal.h"

namespace uam::acp_detail
{

void ResetAcpWaitState(AcpSessionState& session)
{
	session.wait_started_time_s = 0.0;
	session.wait_is_stale = false;
	session.wait_stale_reason.clear();
}

void ResetAcpPendingInteractionState(AcpSessionState& session)
{
	session.waiting_for_permission = false;
	session.waiting_for_user_input = false;
	session.pending_permission = AcpPendingPermissionState{};
	session.pending_user_input = AcpPendingUserInputState{};
	ResetAcpWaitState(session);
}

void ResetAcpTurnStreamState(AcpSessionState& session)
{
	session.pending_assistant_thoughts.clear();
	session.tool_calls.clear();
	session.plan_entries.clear();
	session.plan_summary.clear();
	session.codex_agent_message_text_by_item_id.clear();
	session.codex_last_agent_message_item_id.clear();
	session.codex_streamed_reasoning_keys.clear();
	session.codex_last_reasoning_section.clear();
	session.turn_events.clear();
}

void ClearAcpStartupModelRequest(AcpSessionState& session)
{
	session.startup_model_request_id = 0;
	session.pending_startup_model_id.clear();
}

void BeginAcpPendingWait(AcpSessionState& session, std::string_view lifecycle_state)
{
	const double now = GetAppTimeSeconds();
	session.wait_started_time_s = now;
	session.last_runtime_activity_time_s = now;
	session.wait_is_stale = false;
	session.wait_stale_reason.clear();
	session.processing = true;
	session.lifecycle_state.assign(lifecycle_state);
}

void ClearAcpPendingWait(AcpSessionState& session)
{
	if (!uam::AcpSessionIsWaitingForInput(session))
	{
		ResetAcpWaitState(session);
	}
}

std::string ActiveAcpWaitRequestId(const AcpSessionState& session)
{
	if (!session.pending_permission.request_id_json.empty())
	{
		return session.pending_permission.request_id_json;
	}
	return session.pending_user_input.request_id_json;
}

std::string ActiveAcpWaitToolId(const AcpSessionState& session)
{
	if (!session.pending_permission.tool_call_id.empty())
	{
		return session.pending_permission.tool_call_id;
	}
	return session.pending_user_input.item_id;
}

} // namespace uam::acp_detail
