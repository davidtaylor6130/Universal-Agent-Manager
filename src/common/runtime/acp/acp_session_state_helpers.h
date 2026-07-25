#pragma once

#include "common/state/app_state.h"

namespace uam
{

inline bool AcpSessionIsWaitingForInput(const AcpSessionState& session)
{
	return session.waiting_for_permission || session.waiting_for_user_input;
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
	return session.initialize_request_id != 0 ||
	       session.session_setup_request_id != 0 ||
	       session.startup_model_request_id != 0 ||
	       session.mode_change_request_id != 0 ||
	       session.model_change_request_id != 0 ||
	       session.prompt_request_id != 0 ||
	       session.cancel_request_id != 0;
}

inline bool AcpSessionHasBlockingRuntimeWork(const AcpSessionState& session)
{
	return AcpSessionHasActiveTurn(session) || AcpSessionHasPendingRuntimeRequest(session);
}

} // namespace uam
