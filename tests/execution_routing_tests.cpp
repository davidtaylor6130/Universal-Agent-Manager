#include "test_harness.h"
#include "app/execution_routing_service.h"

#include <barrier>

using namespace uam_test;

namespace
{
	uam::ExecutionRoutingCandidate RoutingHost(const std::string& id, std::size_t slots = 1)
	{
		uam::ExecutionRoutingCandidate candidate;
		candidate.host_id = id;
		candidate.connection_identity = "connection-" + id;
		candidate.confirmed_connection_identity = candidate.connection_identity;
		candidate.provider_configuration_identity = "provider-config";
		candidate.confirmed_provider_configuration_identity = candidate.provider_configuration_identity;
		candidate.pool_enabled = true;
		candidate.provider_confirmed = true;
		candidate.runner_ready = true;
		candidate.runner_compatible = true;
		candidate.provider_supported = true;
		candidate.max_active_turns = slots;
		candidate.mappings.push_back({"local:/repo", "folder-" + id, "/mapped/repo"});
		return candidate;
	}

	uam::ExecutionRoutingRequest RoutingRequest(const std::string& chat_id)
	{
		uam::ExecutionRoutingRequest request;
		request.chat_id = chat_id;
		request.automatic_enabled = true;
		request.source_workspace_identity = "local:/repo";
		return request;
	}
}

UAM_TEST(ExecutionRoutingUsesLeastOccupiedEligibleHostAndStableTieBreak)
{
	uam::ExecutionRoutingService service;
	const std::vector<uam::ExecutionRoutingCandidate> hosts{RoutingHost("z", 3), RoutingHost("a", 3)};
	const uam::ExecutionRoutingDecision tie = service.Admit(RoutingRequest("first"), hosts, {});
	UAM_ASSERT_EQ(tie.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(tie.assignment.host_id, std::string("a"));
	const uam::ExecutionRoutingDecision next = service.Admit(RoutingRequest("second"), hosts, {{"first", "a"}, {"first", "a"}});
	UAM_ASSERT_EQ(next.assignment.host_id, std::string("z"));
	UAM_ASSERT_EQ(next.assignment.destination_folder_id, std::string("folder-z"));
}

UAM_TEST(ExecutionRoutingRequiresConfirmedCurrentCapabilityAndExactWorkspace)
{
	const uam::ExecutionRoutingCandidate ready = RoutingHost("remote");
	for (int invalidation = 0; invalidation < 8; ++invalidation)
	{
		uam::ExecutionRoutingService service;
		uam::ExecutionRoutingCandidate candidate = ready;
		switch (invalidation)
		{
		case 0: candidate.pool_enabled = false; break;
		case 1: candidate.provider_confirmed = false; break;
		case 2: candidate.runner_ready = false; break;
		case 3: candidate.runner_compatible = false; break;
		case 4: candidate.provider_supported = false; break;
		case 5: candidate.confirmed_connection_identity = "stale"; break;
		case 6: candidate.confirmed_provider_configuration_identity = "stale"; break;
		case 7: candidate.max_active_turns = 0; break;
		}
		UAM_ASSERT_EQ(service.Admit(RoutingRequest("candidate"), {candidate}, {}).status, uam::ExecutionRoutingStatus::Waiting);
	}
	uam::ExecutionRoutingService service;
	uam::ExecutionRoutingRequest request = RoutingRequest("worktree");
	request.source_workspace_identity = "local:/repo/worktrees/branch";
	UAM_ASSERT_EQ(service.Admit(request, {ready}, {}).status, uam::ExecutionRoutingStatus::Waiting);
	request.automatic_enabled = false;
	UAM_ASSERT_EQ(service.Admit(request, {ready}, {}).status, uam::ExecutionRoutingStatus::Disabled);
	UAM_ASSERT_EQ(service.Admit(RoutingRequest("no-local-fallback"), {RoutingHost("local")}, {}).status, uam::ExecutionRoutingStatus::Waiting);
}

UAM_TEST(ExecutionRoutingConcurrentAdmissionsReserveOneSlotAtomically)
{
	uam::ExecutionRoutingService service;
	const std::vector<uam::ExecutionRoutingCandidate> hosts{RoutingHost("remote")};
	std::barrier gate(3);
	uam::ExecutionRoutingDecision first;
	uam::ExecutionRoutingDecision second;
	std::jthread first_thread([&] { gate.arrive_and_wait(); first = service.Admit(RoutingRequest("first"), hosts, {}); });
	std::jthread second_thread([&] { gate.arrive_and_wait(); second = service.Admit(RoutingRequest("second"), hosts, {}); });
	gate.arrive_and_wait();
	first_thread.join();
	second_thread.join();
	const int admitted = static_cast<int>(first.status == uam::ExecutionRoutingStatus::Assigned) +
	    static_cast<int>(second.status == uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(admitted, 1);
}

UAM_TEST(ExecutionRoutingExplicitOverrideBypassesAutoSlotsButConsumesOccupancy)
{
	uam::ExecutionRoutingService service;
	uam::ExecutionRoutingCandidate host = RoutingHost("remote");
	host.provider_confirmed = false;
	uam::ExecutionRoutingRequest explicit_request = RoutingRequest("explicit");
	explicit_request.automatic_enabled = false;
	explicit_request.explicit_host_id = "remote";
	explicit_request.explicit_folder_id = "original-folder";
	explicit_request.explicit_directory = "/original/cwd";
	const uam::ExecutionRoutingDecision explicit_result = service.Admit(explicit_request, {host}, {{"busy-chat", "remote"}});
	UAM_ASSERT_EQ(explicit_result.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(explicit_result.assignment.destination_directory, std::string("/original/cwd"));
	host.provider_confirmed = true;
	host.max_active_turns = 2;
	UAM_ASSERT_EQ(service.Admit(RoutingRequest("automatic"), {host}, {{"busy-chat", "remote"}}).status, uam::ExecutionRoutingStatus::Waiting);
	UAM_ASSERT(!service.Release("explicit", false));
	UAM_ASSERT(service.Release("explicit", true));
	UAM_ASSERT_EQ(service.Admit(RoutingRequest("automatic"), {host}, {{"busy-chat", "remote"}}).status, uam::ExecutionRoutingStatus::Assigned);
}

UAM_TEST(ExecutionRoutingRestartPreservesUncertainOwnerAndOriginalDestination)
{
	uam::ExecutionRoutingService original;
	const uam::ExecutionRoutingDecision assigned = original.Admit(RoutingRequest("accepted"), {RoutingHost("a")}, {});
	uam::ExecutionRoutingService restored;
	UAM_ASSERT(restored.RestoreReservations({assigned.assignment}));
	UAM_ASSERT(!restored.Release("accepted", false));
	uam::ExecutionRoutingCandidate changed_mapping = RoutingHost("a");
	changed_mapping.mappings.front().destination_directory = "/different/repo";
	const uam::ExecutionRoutingDecision pinned = restored.Admit(RoutingRequest("accepted"), {changed_mapping, RoutingHost("b")}, {{"accepted", "a"}});
	UAM_ASSERT_EQ(pinned.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(pinned.assignment.destination_directory, std::string("/mapped/repo"));
	changed_mapping.runner_ready = false;
	UAM_ASSERT_EQ(restored.Admit(RoutingRequest("accepted"), {changed_mapping}, {}).reason, uam::ExecutionRoutingReason::HostUnavailable);
	changed_mapping.runner_ready = true;
	changed_mapping.provider_configuration_identity = "different-provider-config";
	UAM_ASSERT_EQ(restored.Admit(RoutingRequest("accepted"), {changed_mapping}, {}).reason, uam::ExecutionRoutingReason::OwnershipChanged);
	changed_mapping.provider_configuration_identity = "provider-config";
	changed_mapping.connection_identity = "different-machine";
	const uam::ExecutionRoutingDecision changed = restored.Admit(RoutingRequest("accepted"), {changed_mapping, RoutingHost("b")}, {});
	UAM_ASSERT_EQ(changed.status, uam::ExecutionRoutingStatus::Waiting);
	UAM_ASSERT_EQ(changed.reason, uam::ExecutionRoutingReason::OwnershipChanged);
	UAM_ASSERT_EQ(changed.assignment.host_id, std::string("a"));
	const uam::ExecutionRoutingDecision missing = restored.Admit(RoutingRequest("accepted"), {RoutingHost("b")}, {});
	UAM_ASSERT_EQ(missing.status, uam::ExecutionRoutingStatus::Waiting);
	UAM_ASSERT_EQ(missing.assignment.host_id, std::string("a"));
	UAM_ASSERT_EQ(restored.Admit(RoutingRequest("next"), {RoutingHost("a")}, {}).status, uam::ExecutionRoutingStatus::Waiting);
}

UAM_TEST(ExecutionRoutingRejectsConflictingSnapshotsWithoutChangingReservations)
{
	uam::ExecutionRoutingService service;
	const uam::ExecutionRoutingCandidate host = RoutingHost("a", 2);
	UAM_ASSERT_EQ(service.Admit(RoutingRequest("bad-host"), {host, host}, {}).status, uam::ExecutionRoutingStatus::Invalid);
	UAM_ASSERT_EQ(service.Admit(RoutingRequest("bad-owner"), {host}, {{"unknown-outbox", "a"}, {"unknown-outbox", "b"}}).status, uam::ExecutionRoutingStatus::Invalid);
	UAM_ASSERT_EQ(service.Admit(RoutingRequest("unknown-outbox"), {host}, {{"unknown-outbox", "a"}}).status, uam::ExecutionRoutingStatus::Invalid);
	const uam::ExecutionRoutingDecision assigned = service.Admit(RoutingRequest("accepted"), {host}, {});
	uam::ExecutionRoutingAssignment wrong = assigned.assignment;
	wrong.host_id = "b";
	UAM_ASSERT(!service.RestoreReservations({wrong}));
	UAM_ASSERT_EQ(service.Admit(RoutingRequest("accepted"), {host}, {}).assignment, assigned.assignment);
	const uam::ExecutionRoutingDecision next = service.Admit(RoutingRequest("next"), {host}, {{"accepted", "a"}, {"accepted", "a"}});
	UAM_ASSERT_EQ(next.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(service.Admit(RoutingRequest("full"), {host}, {}).status, uam::ExecutionRoutingStatus::Waiting);
}

UAM_TEST(ExecutionRoutingAssignedIdleAutomaticChatWaitsForItsOwnHostCapacity)
{
	uam::ExecutionRoutingService service;
	const uam::ExecutionRoutingDecision initial = service.Admit(RoutingRequest("idle"), {RoutingHost("a")}, {});
	UAM_ASSERT(initial.assignment.automatic);
	UAM_ASSERT(service.Release("idle", true));
	uam::ExecutionRoutingRequest next_turn = RoutingRequest("idle");
	next_turn.assigned = initial.assignment;
	const uam::ExecutionRoutingDecision full = service.Admit(next_turn, {RoutingHost("a"), RoutingHost("b")}, {{"another-chat", "a"}});
	UAM_ASSERT_EQ(full.status, uam::ExecutionRoutingStatus::Waiting);
	UAM_ASSERT_EQ(full.assignment.host_id, std::string("a"));
	const uam::ExecutionRoutingDecision unrelated = service.Admit(RoutingRequest("unrelated"), {RoutingHost("a")}, {});
	UAM_ASSERT_EQ(unrelated.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT(service.Release("unrelated", true));
	const uam::ExecutionRoutingDecision available = service.Admit(next_turn, {RoutingHost("a")}, {});
	UAM_ASSERT_EQ(available.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(available.assignment, initial.assignment);
}

UAM_TEST(ExecutionRoutingOnlyOwnedRecoveryBypassesChangedOptInAndFullCapacity)
{
	uam::ExecutionRoutingService original;
	const uam::ExecutionRoutingDecision initial = original.Admit(RoutingRequest("owned"), {RoutingHost("a")}, {});
	uam::ExecutionRoutingService restored;
	UAM_ASSERT(restored.RestoreReservations({initial.assignment}));
	uam::ExecutionRoutingCandidate unconfirmed = RoutingHost("a");
	unconfirmed.provider_confirmed = false;
	unconfirmed.pool_enabled = false;
	uam::ExecutionRoutingRequest request = RoutingRequest("owned");
	request.assigned = initial.assignment;
	request.automatic_enabled = false;
	UAM_ASSERT_EQ(restored.Admit(request, {unconfirmed, RoutingHost("b")}, {{"another-chat", "a"}}).status, uam::ExecutionRoutingStatus::Waiting);
	request.recovery_of_owned_work = true;
	const uam::ExecutionRoutingDecision recovered = restored.Admit(request, {unconfirmed, RoutingHost("b")}, {{"another-chat", "a"}});
	UAM_ASSERT_EQ(recovered.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(recovered.assignment, initial.assignment);
	unconfirmed.runner_ready = false;
	UAM_ASSERT_EQ(restored.Admit(request, {unconfirmed, RoutingHost("b")}, {}).reason, uam::ExecutionRoutingReason::HostUnavailable);
	request.chat_id = "unknown";
	request.assigned = {};
	UAM_ASSERT_EQ(restored.Admit(request, {RoutingHost("b")}, {}).status, uam::ExecutionRoutingStatus::Invalid);
}
