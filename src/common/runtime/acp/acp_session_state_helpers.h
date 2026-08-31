#pragma once

#include "common/state/app_state.h"

#include <algorithm>

namespace uam
{
	enum class AcpTurnInactivityRecoveryAction
	{
		None,
		Cancel,
		Stop,
	};

inline bool AcpSessionIsWaitingForInput(const AcpSessionState& session)
{
	return session.waiting_for_permission || session.waiting_for_user_input;
}

inline AcpTurnInactivityRecoveryAction AcpTurnInactivityRecovery(const AcpSessionState& session, double now_seconds, double timeout_seconds, double cancel_grace_seconds = 5.0)
{
	if (session.inactivity_timeout_pending)
	{
		return session.cancel_requested_time_s > 0.0 && now_seconds - session.cancel_requested_time_s >= cancel_grace_seconds ? AcpTurnInactivityRecoveryAction::Stop : AcpTurnInactivityRecoveryAction::None;
	}
	if (!session.running || !session.processing || session.cancel_requested || AcpSessionIsWaitingForInput(session) || !session.queued_prompt.empty())
	{
		return AcpTurnInactivityRecoveryAction::None;
	}
	const double latest_activity = std::max(session.turn_started_time_s, session.last_runtime_activity_time_s);
	return latest_activity > 0.0 && now_seconds - latest_activity >= timeout_seconds ? AcpTurnInactivityRecoveryAction::Cancel : AcpTurnInactivityRecoveryAction::None;
}

inline bool AcpSessionHasActiveTurn(const AcpSessionState& session)
{
	return session.processing ||
	       AcpSessionIsWaitingForInput(session) ||
	       !session.queued_prompt.empty() ||
	       !session.queued_user_prompts.empty() ||
	       session.prompt_request_id != 0;
}

inline bool AcpSessionHasCancelableWork(const AcpSessionState& session)
{
	return AcpSessionHasActiveTurn(session) || session.cancel_request_id != 0;
}

inline bool AcpSessionHasPendingCancel(const AcpSessionState& session)
{
	return session.cancel_requested || session.cancel_request_id != 0;
}

inline bool AcpSessionHasPendingRuntimeRequest(const AcpSessionState& session)
{
	return session.initialize_request_id != 0 || session.session_setup_request_id != 0 || session.startup_model_request_id != 0 || session.reasoning_change_request_id != 0 || session.config_option_change_request_id != 0 || session.mode_change_request_id != 0 || session.model_change_request_id != 0 || session.awaiting_model_config_options || session.prompt_request_id != 0 || session.cancel_request_id != 0;
}

inline bool AcpSessionHasDeferredUserQueueOnly(const AcpSessionState& session)
{
	return !session.running &&
	       session.reconnect_pending &&
	       !session.queued_user_prompts.empty() &&
	       !session.processing &&
	       session.queued_prompt.empty() &&
	       !AcpSessionIsWaitingForInput(session) &&
	       !AcpSessionHasPendingRuntimeRequest(session);
}

inline bool AcpSessionHasBlockingRuntimeWork(const AcpSessionState& session)
{
	return AcpSessionHasActiveTurn(session) || AcpSessionHasPendingRuntimeRequest(session);
}

} // namespace uam
