#include "test_harness.h"
#include "app/execution_routing_service.h"

using namespace uam_test;

namespace
{
	using Skip = uam::ExecutionRoutingHostSkip;

	uam::ExecutionRoutingCandidate ReadyHost(const std::string& id, std::size_t slots = 1)
	{
		uam::ExecutionRoutingCandidate candidate;
		candidate.host_id = id;
		candidate.connection_identity = candidate.confirmed_connection_identity = "connection-" + id;
		candidate.provider_configuration_identity = candidate.confirmed_provider_configuration_identity = "provider-config";
		candidate.pool_enabled = candidate.provider_confirmed = candidate.runner_ready = candidate.runner_compatible = candidate.provider_supported = true;
		candidate.max_active_turns = slots;
		candidate.mappings.push_back({"local:/repo", "folder-" + id, "/mapped/repo"});
		return candidate;
	}

	uam::ExecutionRoutingRequest AutomaticRequest(const std::string& chat_id)
	{
		uam::ExecutionRoutingRequest request;
		request.chat_id = chat_id;
		request.automatic_enabled = true;
		request.source_workspace_identity = "local:/repo";
		return request;
	}

	/// The assigned computer, named "a", passed over for this reason alone.
	std::vector<uam::ExecutionRoutingSkippedHost> Only(Skip reason)
	{
		return {{"a", reason}};
	}

	std::vector<uam::ExecutionRoutingCandidate> OneOfEachProblem()
	{
		uam::ExecutionRoutingCandidate off = ReadyHost("off");
		off.pool_enabled = false;
		off.runner_ready = false;
		uam::ExecutionRoutingCandidate unreachable = ReadyHost("unreachable");
		unreachable.runner_ready = false;
		unreachable.confirmed_connection_identity = "stale";
		uam::ExecutionRoutingCandidate incompatible = ReadyHost("incompatible");
		incompatible.runner_compatible = false;
		uam::ExecutionRoutingCandidate provider = ReadyHost("provider");
		provider.provider_supported = false;
		uam::ExecutionRoutingCandidate unconfirmed = ReadyHost("unconfirmed");
		unconfirmed.confirmed_provider_configuration_identity = "older-provider-config";
		uam::ExecutionRoutingCandidate unmapped = ReadyHost("unmapped");
		unmapped.mappings = {{"local:/other", "folder-other", "/mapped/other"}};
		uam::ExecutionRoutingCandidate full = ReadyHost("full");
		uam::ExecutionRoutingCandidate no_slots = ReadyHost("no-slots", 0);
		uam::ExecutionRoutingCandidate local = ReadyHost("local", 8);
		return {off, unreachable, incompatible, provider, unconfirmed, unmapped, full, no_slots, local};
	}
}

UAM_TEST(ExecutionRoutingWaitNamesWhyEachRemoteComputerWasPassedOver)
{
	uam::ExecutionRoutingService service;
	const uam::ExecutionRoutingDecision waiting = service.Admit(AutomaticRequest("chat"), OneOfEachProblem(), {{"busy", "full"}});
	UAM_ASSERT_EQ(waiting.status, uam::ExecutionRoutingStatus::Waiting);
	UAM_ASSERT_EQ(waiting.reason, uam::ExecutionRoutingReason::NoEligibleHost);
	// This computer is never an automatic destination, so it is not listed as passed over.
	const std::vector<uam::ExecutionRoutingSkippedHost> expected{{"off", Skip::Off}, {"unreachable", Skip::Unreachable},
	    {"incompatible", Skip::Unreachable}, {"provider", Skip::ProviderUnavailable}, {"unconfirmed", Skip::Unconfirmed},
	    {"unmapped", Skip::NoMapping}, {"full", Skip::Full}, {"no-slots", Skip::Full}};
	UAM_ASSERT(waiting.skipped == expected);

	// The diagnostics do not change the choice: a free computer is still assigned and nothing is listed.
	std::vector<uam::ExecutionRoutingCandidate> with_free = OneOfEachProblem();
	with_free.push_back(ReadyHost("free", 2));
	const uam::ExecutionRoutingDecision assigned = service.Admit(AutomaticRequest("chat"), with_free, {{"busy", "full"}});
	UAM_ASSERT_EQ(assigned.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT_EQ(assigned.assignment.host_id, std::string("free"));
	UAM_ASSERT(assigned.skipped.empty());
}

UAM_TEST(ExecutionRoutingAssignedChatWaitNamesOnlyItsOwnComputer)
{
	uam::ExecutionRoutingService service;
	const uam::ExecutionRoutingDecision initial = service.Admit(AutomaticRequest("pinned"), {ReadyHost("a")}, {});
	UAM_ASSERT(initial.assignment.automatic);
	UAM_ASSERT(service.Release("pinned", true));
	uam::ExecutionRoutingRequest next_turn = AutomaticRequest("pinned");
	next_turn.assigned = initial.assignment;

	const uam::ExecutionRoutingDecision full = service.Admit(next_turn, {ReadyHost("a"), ReadyHost("b", 4)}, {{"another-chat", "a"}});
	UAM_ASSERT_EQ(full.status, uam::ExecutionRoutingStatus::Waiting);
	UAM_ASSERT(full.skipped == Only(Skip::Full));

	uam::ExecutionRoutingCandidate unreachable = ReadyHost("a");
	unreachable.runner_ready = false;
	UAM_ASSERT(service.Admit(next_turn, {unreachable}, {}).skipped == Only(Skip::Unreachable));
	UAM_ASSERT(service.Admit(next_turn, {ReadyHost("b")}, {}).skipped == Only(Skip::Unreachable));
	uam::ExecutionRoutingCandidate provider = ReadyHost("a");
	provider.provider_supported = false;
	UAM_ASSERT(service.Admit(next_turn, {provider}, {}).skipped == Only(Skip::ProviderUnavailable));
	uam::ExecutionRoutingCandidate unconfirmed = ReadyHost("a");
	unconfirmed.confirmed_provider_configuration_identity = "older-provider-config";
	UAM_ASSERT(service.Admit(next_turn, {unconfirmed}, {}).skipped == Only(Skip::Unconfirmed));
	uam::ExecutionRoutingCandidate turned_off = ReadyHost("a");
	turned_off.pool_enabled = false;
	UAM_ASSERT(service.Admit(next_turn, {turned_off}, {}).skipped == Only(Skip::Off));
	uam::ExecutionRoutingRequest routing_off = next_turn;
	routing_off.automatic_enabled = false;
	UAM_ASSERT(service.Admit(routing_off, {ReadyHost("a")}, {}).skipped == Only(Skip::Off));

	const uam::ExecutionRoutingDecision available = service.Admit(next_turn, {ReadyHost("a")}, {});
	UAM_ASSERT_EQ(available.status, uam::ExecutionRoutingStatus::Assigned);
	UAM_ASSERT(available.skipped.empty());
}

UAM_TEST(ExecutionRoutingOccupiedCountsMatchAdmissionIncludingReservations)
{
	uam::ExecutionRoutingService service;
	UAM_ASSERT_EQ(service.Admit(AutomaticRequest("reserved"), {ReadyHost("a", 4)}, {}).status, uam::ExecutionRoutingStatus::Assigned);
	// A chat reported live and also reserved counts once; an unrelated live chat adds one.
	const std::unordered_map<std::string, std::size_t> counts = service.OccupiedCounts({{"reserved", "a"}, {"live", "a"}, {"elsewhere", "b"}, {"", "a"}});
	UAM_ASSERT_EQ(counts.at("a"), std::size_t{2});
	UAM_ASSERT_EQ(counts.at("b"), std::size_t{1});
	UAM_ASSERT(service.Release("reserved", true));
	UAM_ASSERT(!service.OccupiedCounts({}).contains("a"));
}
