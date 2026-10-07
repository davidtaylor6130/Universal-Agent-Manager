#pragma once

#include "app/execution_routing_service.h"
#include "common/state/app_state.h"

namespace uam
{
	std::string ExecutionRoutingConnectionIdentity(const ExecutionHost& host);
	std::string ExecutionRoutingProviderIdentity(const AppState& app, const ProviderProfile& provider);
	/// Persist assignment and its reservation before the caller starts any provider operation.
	ExecutionRoutingDecision AdmitExecutionRouting(AppState& app, ChatSession& chat, bool terminal = false);
	bool ExecutionRoutingOwnsWork(const AppState& app, const ChatSession& chat);
	bool RestoreExecutionRoutingReservations(AppState& app);
	bool PollExecutionRouting(AppState& app);
	/// Clears only local admission metadata after an explicit acknowledgement. Never stops a remote process.
	bool ReleaseUnresolvedExecutionRouting(AppState& app, const ExecutionRoutingAssignment& expected, bool acknowledged, std::string& error);
}
