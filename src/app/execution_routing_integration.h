#pragma once
#include <string_view>

#include "app/execution_routing_service.h"
#include "common/state/app_state.h"

namespace uam
{
	/// Exact canonical projections shared with routing identity hashing; no execution authority.
	std::string ExecutionRoutingConnectionConfiguration(const ExecutionHost& host);
	std::string ExecutionRoutingProviderConfiguration(const AppState& app, const ProviderProfile& provider, std::string_view host_id = "local");
	std::string ExecutionRoutingConnectionIdentity(const ExecutionHost& host);
	std::string ExecutionRoutingProviderIdentity(const AppState& app, const ProviderProfile& provider, std::string_view host_id = "local");
	/// One-time carry-over of identities saved in the earlier format, for configurations that did not change.
	/// Call after chats and settings load and before reservations are restored. True when settings need saving.
	bool MigrateExecutionRoutingIdentities(AppState& app);
	/// Persist assignment and its reservation before the caller starts any provider operation.
	ExecutionRoutingDecision AdmitExecutionRouting(AppState& app, ChatSession& chat, bool terminal = false);
	bool ExecutionRoutingOwnsWork(const AppState& app, const ChatSession& chat);
	bool RestoreExecutionRoutingReservations(AppState& app);
	bool PollExecutionRouting(AppState& app);
	/// Contacts hosts whose last health check failed and hosts holding a reservation restored without its
	/// owning work, off the app thread, one contact per host at a time and with a growing delay. A contact
	/// restores Ready; the "no such process" answer of the unchanged helper the reservation was made on
	/// releases it. Never stops or starts a remote process.
	bool PollRemoteHostRecovery(AppState& app);
	/// Clears only local admission metadata after an explicit acknowledgement. Never stops a remote process.
	bool ReleaseUnresolvedExecutionRouting(AppState& app, const ExecutionRoutingAssignment& expected, bool acknowledged, std::string& error);
}
