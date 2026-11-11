#pragma once

#include "common/runtime/acp/acp_session_state_helpers.h"
#include "common/config/execution_host_config.h"
#include "common/state/app_state.h"

namespace uam
{

// A stale local wait may let the machine sleep: the local process survives it.
// A remote wait may not: sleep drops SSH and can lose the turn awaiting the answer.
inline bool AcpStaleWaitMaySleep(const AcpSessionState& session)
{
	const bool remote = !session.process_execution_host_id.empty() && session.process_execution_host_id != execution_hosts::kLocalHostId;
	return !remote && session.wait_is_stale && AcpSessionIsWaitingForInput(session);
}

inline bool RuntimeShouldKeepSystemAwake(const AppState& app)
{
	for (const auto& terminal : app.cli_terminals)
	{
		if (terminal != nullptr && terminal->running && (terminal->generation_in_progress || terminal->turn_state == CliTerminalTurnState::Busy || terminal->lifecycle_state == CliTerminalLifecycleState::Busy || terminal->lifecycle_state == CliTerminalLifecycleState::Unknown))
		{
			return true;
		}
	}

	for (const auto& session : app.acp_sessions)
	{
		const bool stale_wait = session != nullptr && AcpStaleWaitMaySleep(*session);
		if (session != nullptr && session->running && !stale_wait && AcpSessionHasBlockingRuntimeWork(*session))
		{
			return true;
		}
	}

	for (const ChatSession& chat : app.chats)
	{
		if (chat.active_goal_id.empty())
		{
			continue;
		}

		bool stale_wait = false;
		for (const auto& session : app.acp_sessions)
		{
			if (session != nullptr && session->chat_id == chat.id && AcpStaleWaitMaySleep(*session))
			{
				stale_wait = true;
				break;
			}
		}
		if (stale_wait)
		{
			continue;
		}

		for (const Goal& goal : chat.goals)
		{
			if (goal.id == chat.active_goal_id && goal.status == GoalStatus::Active)
			{
				return true;
			}
		}
	}

	return false;
}

} // namespace uam
