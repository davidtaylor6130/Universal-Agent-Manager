#pragma once

#include "common/models/app_models.h"
#include "common/platform/child_process_priority.h"

namespace uam
{
	/// <summary>
	/// The scheduling class of work done for a chat: a sub-agent transcript, a goal iteration transcript and an
	/// automation chat are agent work; every other chat serves its user's own turns and keeps normal priority.
	/// </summary>
	inline platform::ChildProcessClass ChatWorkProcessClass(const ChatSession& chat)
	{
		if (!chat.agent_run_id.empty() || !chat.goal_owner_chat_id.empty() || !chat.automation_token_id.empty())
			return platform::ChildProcessClass::Agent;
		return platform::ChildProcessClass::UserTurn;
	}
}
