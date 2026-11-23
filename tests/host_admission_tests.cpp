#include "test_harness.h"
#include "common/runtime/host_admission.h"

#include <memory>

using namespace uam_test;

namespace
{
	using uam::host_admission::Decision;
	using uam::host_admission::Health;
	using uam::host_admission::HostAdmission;
	using uam::host_admission::HostLimits;
	using uam::host_admission::LiveWork;
	using uam::host_admission::LoadSample;
	using uam::host_admission::Reason;
	using uam::host_admission::Request;
	using uam::host_admission::WorkClass;

	HostLimits Host(const std::string& id, std::size_t slots, std::size_t background = 2, LoadSample load = {})
	{
		HostLimits host;
		host.host_id = id;
		host.label = id == "local" ? "this computer" : "host " + id;
		host.slot_cap = slots;
		host.background_budget = background;
		host.load = load;
		return host;
	}

	LoadSample Load(std::optional<double> cpu, std::optional<double> free_memory = 50.0)
	{
		LoadSample sample;
		sample.known = true;
		sample.cpu_percent = cpu;
		sample.free_memory_percent = free_memory;
		return sample;
	}

	Request Ask(const std::string& key, const std::string& host, WorkClass work_class, double now, bool hold = false)
	{
		Request request;
		request.key = key;
		request.host_id = host;
		request.work_class = work_class;
		request.now_s = now;
		request.hold = hold;
		return request;
	}

	std::vector<LiveWork> Busy(const std::string& host, std::size_t count, WorkClass work_class = WorkClass::Interactive, const std::string& prefix = "busy")
	{
		std::vector<LiveWork> live;
		for (std::size_t index = 0; index < count; ++index) live.push_back({prefix + std::to_string(index), host, work_class});
		return live;
	}
}

UAM_TEST(HostAdmissionExplicitTurnQueuesAtTheSlotLimitWithRunAnyway)
{
	HostAdmission admission;
	admission.UpdateHosts({Host("A", 8)}, 0.0);
	admission.SetLive(Busy("A", 8));
	const Decision held = admission.Admit(Ask("turn:explicit", "A", WorkClass::Interactive, 0.0));
	UAM_ASSERT(!held.admitted);
	UAM_ASSERT_EQ(held.reason, Reason::SlotsFull);
	UAM_ASSERT_EQ(held.message, std::string("Waiting for host A (8/8)"));
	UAM_ASSERT(held.run_anyway_offered);
	UAM_ASSERT(held.message.find('\n') == std::string::npos);
	// An agent at the same limit waits too, but is never offered Run anyway.
	const Decision agent = admission.Admit(Ask("turn:agent", "A", WorkClass::Agent, 0.0));
	UAM_ASSERT(!agent.admitted);
	UAM_ASSERT(!agent.run_anyway_offered);
	Request anyway = Ask("turn:explicit", "A", WorkClass::Interactive, 0.5);
	anyway.run_anyway = true;
	UAM_ASSERT(admission.Admit(anyway).admitted);
	UAM_ASSERT(admission.QueuedKeys("A", 0.5) == std::vector<std::string>{"turn:agent"});
	// A slot freeing goes to the waiting agent.
	admission.SetLive(Busy("A", 7));
	UAM_ASSERT(admission.Admit(Ask("turn:agent", "A", WorkClass::Agent, 1.0)).admitted);
}

UAM_TEST(HostAdmissionServesWaitersFirstComeFirstServedPerHost)
{
	HostAdmission admission;
	admission.UpdateHosts({Host("A", 1), Host("B", 1)}, 0.0);
	admission.SetLive(Busy("A", 1));
	for (const char* key : {"first", "second", "third"}) UAM_ASSERT(!admission.Admit(Ask(key, "A", WorkClass::Interactive, 0.0)).admitted);
	UAM_ASSERT((admission.QueuedKeys("A", 0.0) == std::vector<std::string>{"first", "second", "third"}));
	// Another computer's line is separate.
	UAM_ASSERT(admission.Admit(Ask("elsewhere", "B", WorkClass::Interactive, 0.0)).admitted);
	admission.SetLive({});
	// The freed slot is promised to the longest waiter, even when a later one asks first.
	const Decision second = admission.Admit(Ask("second", "A", WorkClass::Interactive, 1.0));
	UAM_ASSERT(!second.admitted);
	UAM_ASSERT_EQ(second.message, std::string("Waiting for host A (1/1)"));
	UAM_ASSERT(!admission.Admit(Ask("newcomer", "A", WorkClass::Interactive, 1.0)).admitted);
	UAM_ASSERT(admission.Admit(Ask("first", "A", WorkClass::Interactive, 1.0)).admitted);
	admission.SetLive({{"first", "A", WorkClass::Interactive}});
	admission.SetLive({});
	UAM_ASSERT(!admission.Admit(Ask("third", "A", WorkClass::Interactive, 2.0)).admitted);
	UAM_ASSERT(admission.Admit(Ask("second", "A", WorkClass::Interactive, 2.0)).admitted);
	UAM_ASSERT((admission.QueuedKeys("A", 2.0) == std::vector<std::string>{"third", "newcomer"}));
}

UAM_TEST(HostAdmissionPutsUserTurnsBeforeAgentsUntilAnAgentHasAged)
{
	HostAdmission admission;
	admission.UpdateHosts({Host("A", 1)}, 0.0);
	admission.SetLive(Busy("A", 1));
	UAM_ASSERT(!admission.Admit(Ask("agent", "A", WorkClass::Agent, 0.0)).admitted);
	UAM_ASSERT(!admission.Admit(Ask("user", "A", WorkClass::Interactive, 1.0)).admitted);
	admission.SetLive({});
	// The user's turn goes first although the agent asked earlier.
	UAM_ASSERT(!admission.Admit(Ask("agent", "A", WorkClass::Agent, 2.0)).admitted);
	UAM_ASSERT(admission.Admit(Ask("user", "A", WorkClass::Interactive, 2.0)).admitted);

	// An agent that keeps waiting past the aging limit is served like a user turn, in ticket order.
	HostAdmission aged;
	aged.UpdateHosts({Host("A", 1)}, 0.0);
	aged.SetLive(Busy("A", 1));
	for (double now = 0.0; now <= HostAdmission::kAgentAgingSeconds; now += 1.0)
		UAM_ASSERT(!aged.Admit(Ask("agent", "A", WorkClass::Agent, now)).admitted);
	UAM_ASSERT(!aged.Admit(Ask("user", "A", WorkClass::Interactive, HostAdmission::kAgentAgingSeconds)).admitted);
	aged.SetLive({});
	const double later = HostAdmission::kAgentAgingSeconds + 1.0;
	UAM_ASSERT(!aged.Admit(Ask("user", "A", WorkClass::Interactive, later)).admitted);
	UAM_ASSERT(aged.Admit(Ask("agent", "A", WorkClass::Agent, later)).admitted);
}

UAM_TEST(HostAdmissionBackgroundWaitsOverTheSoftLimitAndRunsWhenLoadDrops)
{
	HostAdmission admission;
	admission.UpdateHosts({Host("local", 6, 2, Load(80.0))}, 0.0);
	UAM_ASSERT_EQ(admission.HealthOf("local"), Health::Degraded);
	const Decision busy = admission.Admit(Ask("discovery", "local", WorkClass::Background, 0.0));
	UAM_ASSERT(!busy.admitted);
	UAM_ASSERT_EQ(busy.reason, Reason::Busy);
	UAM_ASSERT_EQ(busy.message, std::string("Waiting for this computer: busy (CPU 80%)"));
	UAM_ASSERT(!busy.run_anyway_offered);
	// The soft limit does not hold user turns or agents.
	UAM_ASSERT(admission.Admit(Ask("turn", "local", WorkClass::Interactive, 0.0)).admitted);
	UAM_ASSERT(admission.Admit(Ask("agent", "local", WorkClass::Agent, 0.0)).admitted);
	// Low memory alone is over the soft limit too.
	HostAdmission memory;
	memory.UpdateHosts({Host("local", 6, 2, Load(5.0, 12.0))}, 0.0);
	UAM_ASSERT_EQ(memory.Admit(Ask("memory-worker", "local", WorkClass::Background, 0.0)).message, std::string("Waiting for this computer: busy (12% memory free)"));

	// Load drops: the host stays Degraded for its back-off, then background work starts.
	admission.UpdateHosts({Host("local", 6, 2, Load(20.0))}, 1.0);
	UAM_ASSERT_EQ(admission.HealthOf("local"), Health::Degraded);
	const Decision settling = admission.Admit(Ask("discovery", "local", WorkClass::Background, 1.0));
	UAM_ASSERT(!settling.admitted);
	UAM_ASSERT_EQ(settling.message, std::string("Waiting for this computer: busy, rechecking in 9 s"));
	admission.UpdateHosts({Host("local", 6, 2, Load(20.0))}, 1.0 + HostAdmission::kBackoffInitialSeconds);
	UAM_ASSERT_EQ(admission.HealthOf("local"), Health::Ready);
	UAM_ASSERT(admission.Admit(Ask("discovery", "local", WorkClass::Background, 11.0)).admitted);
}

UAM_TEST(HostAdmissionHardLimitHoldsAgentsAndOffersRunAnywayToUserTurns)
{
	HostAdmission admission;
	admission.UpdateHosts({Host("A", 8, 2, Load(95.0))}, 0.0);
	// CPU must stay over the hard limit for ten seconds; until then it is only over the soft limit.
	UAM_ASSERT_EQ(admission.HealthOf("A"), Health::Degraded);
	UAM_ASSERT(admission.Admit(Ask("agent-early", "A", WorkClass::Agent, 0.0)).admitted);
	admission.UpdateHosts({Host("A", 8, 2, Load(95.0))}, 9.0);
	UAM_ASSERT_EQ(admission.HealthOf("A"), Health::Degraded);
	admission.UpdateHosts({Host("A", 8, 2, Load(95.0))}, 10.0);
	UAM_ASSERT_EQ(admission.HealthOf("A"), Health::Overloaded);
	const Decision agent = admission.Admit(Ask("agent", "A", WorkClass::Agent, 10.0));
	UAM_ASSERT(!agent.admitted);
	UAM_ASSERT_EQ(agent.reason, Reason::Overloaded);
	UAM_ASSERT(!agent.run_anyway_offered);
	UAM_ASSERT_EQ(admission.Admit(Ask("background", "A", WorkClass::Background, 10.0)).reason, Reason::Overloaded);
	const Decision user = admission.Admit(Ask("user", "A", WorkClass::Interactive, 10.0));
	UAM_ASSERT(!user.admitted);
	UAM_ASSERT(user.run_anyway_offered);
	UAM_ASSERT_EQ(user.message, std::string("Waiting for host A: overloaded (CPU 95%)"));
	Request anyway = Ask("user", "A", WorkClass::Interactive, 10.0);
	anyway.run_anyway = true;
	UAM_ASSERT(admission.Admit(anyway).admitted);
	// A dip below the hard limit restarts the ten seconds.
	HostAdmission dip;
	dip.UpdateHosts({Host("A", 8, 2, Load(95.0))}, 0.0);
	dip.UpdateHosts({Host("A", 8, 2, Load(85.0))}, 5.0);
	dip.UpdateHosts({Host("A", 8, 2, Load(95.0))}, 6.0);
	dip.UpdateHosts({Host("A", 8, 2, Load(95.0))}, 12.0);
	UAM_ASSERT_EQ(dip.HealthOf("A"), Health::Degraded);
	// Free memory under the hard limit is overloaded at once.
	HostAdmission memory;
	memory.UpdateHosts({Host("A", 8, 2, Load(10.0, 6.0))}, 0.0);
	UAM_ASSERT_EQ(memory.HealthOf("A"), Health::Overloaded);
}

UAM_TEST(HostAdmissionOverloadBackOffDoublesWhenTheHostReturnsSoon)
{
	HostAdmission admission;
	admission.UpdateHosts({Host("A", 4, 2, Load(10.0, 5.0))}, 0.0);
	admission.UpdateHosts({Host("A", 4, 2, Load(10.0, 50.0))}, 1.0);
	UAM_ASSERT_EQ(admission.HealthOf("A"), Health::Overloaded);
	admission.UpdateHosts({Host("A", 4, 2, Load(10.0, 50.0))}, 10.5);
	UAM_ASSERT_EQ(admission.HealthOf("A"), Health::Ready);
	// Overloaded again within five minutes: the back-off doubles to twenty seconds.
	admission.UpdateHosts({Host("A", 4, 2, Load(10.0, 5.0))}, 20.0);
	admission.UpdateHosts({Host("A", 4, 2, Load(10.0, 50.0))}, 21.0);
	admission.UpdateHosts({Host("A", 4, 2, Load(10.0, 50.0))}, 39.0);
	UAM_ASSERT_EQ(admission.HealthOf("A"), Health::Overloaded);
	const std::vector<uam::host_admission::HostUsage> usage = admission.Usage(39.0);
	UAM_ASSERT_EQ(usage.size(), std::size_t{1});
	UAM_ASSERT(usage.front().backoff_remaining_s > 0.5 && usage.front().backoff_remaining_s < 1.5);
	admission.UpdateHosts({Host("A", 4, 2, Load(10.0, 50.0))}, 40.5);
	UAM_ASSERT_EQ(admission.HealthOf("A"), Health::Ready);
}

UAM_TEST(HostAdmissionUnknownLoadUsesSlotCountsOnly)
{
	HostAdmission admission;
	admission.UpdateHosts({Host("A", 2, 1)}, 0.0);
	UAM_ASSERT_EQ(admission.HealthOf("A"), Health::Ready);
	UAM_ASSERT(admission.Admit(Ask("discovery", "A", WorkClass::Background, 0.0, true)).admitted);
	// The background budget still applies with unknown load, and unknown load never frees a slot.
	const Decision budget = admission.Admit(Ask("version-check", "A", WorkClass::Background, 0.0));
	UAM_ASSERT_EQ(budget.message, std::string("Waiting for host A (background 1/1)"));
	admission.SetLive(Busy("A", 2));
	UAM_ASSERT_EQ(admission.Admit(Ask("turn", "A", WorkClass::Interactive, 0.0)).reason, Reason::SlotsFull);
	// A known reading then an unknown one: unknown does not prolong or clear the held state by itself.
	LoadSample unknown;
	UAM_ASSERT(!unknown.known);
	admission.UpdateHosts({Host("A", 2, 1, Load(80.0))}, 1.0);
	admission.UpdateHosts({Host("A", 2, 1, unknown)}, 2.0);
	UAM_ASSERT_EQ(admission.HealthOf("A"), Health::Degraded);
	admission.UpdateHosts({Host("A", 2, 1, unknown)}, 12.0);
	UAM_ASSERT_EQ(admission.HealthOf("A"), Health::Ready);
}

UAM_TEST(HostAdmissionBackgroundBudgetIsSeparateFromSlotsAndFirstInFirstOut)
{
	auto admission = std::make_shared<HostAdmission>();
	admission->UpdateHosts({Host("A", 1, 2)}, 0.0);
	admission->SetLive(Busy("A", 1));
	{
		UAM_ASSERT(admission->Admit(Ask("bg-1", "A", WorkClass::Background, 0.0, true)).admitted);
		uam::host_admission::HeldLease first(admission, "bg-1");
		UAM_ASSERT(admission->Admit(Ask("bg-2", "A", WorkClass::Background, 0.0, true)).admitted);
		uam::host_admission::HeldLease second(admission, "bg-2");
		UAM_ASSERT_EQ(admission->Admit(Ask("bg-3", "A", WorkClass::Background, 0.0)).reason, Reason::BackgroundBudgetFull);
		UAM_ASSERT_EQ(admission->Admit(Ask("bg-4", "A", WorkClass::Background, 0.0)).reason, Reason::BackgroundBudgetFull);
		const std::vector<uam::host_admission::HostUsage> usage = admission->Usage(0.0);
		UAM_ASSERT_EQ(usage.front().interactive, std::size_t{1});
		UAM_ASSERT_EQ(usage.front().background, std::size_t{2});
		UAM_ASSERT_EQ(usage.front().waiting, std::size_t{2});
		first.Reset();
		// One slot freed: the earlier waiter takes it, the later one keeps waiting.
		UAM_ASSERT(!admission->Admit(Ask("bg-4", "A", WorkClass::Background, 1.0)).admitted);
		UAM_ASSERT(admission->Admit(Ask("bg-3", "A", WorkClass::Background, 1.0, true)).admitted);
		UAM_ASSERT(admission->Release("bg-3"));
	}
	// Leases going out of scope release their places.
	UAM_ASSERT_EQ(admission->Usage(2.0).front().background, std::size_t{0});
	UAM_ASSERT(admission->Admit(Ask("bg-4", "A", WorkClass::Background, 2.0)).admitted);
}

UAM_TEST(HostAdmissionUnreachableHostHoldsAgentAndBackgroundWork)
{
	HostAdmission admission;
	HostLimits down = Host("A", 4);
	down.reachable = false;
	admission.UpdateHosts({down}, 0.0);
	UAM_ASSERT_EQ(admission.HealthOf("A"), Health::Unreachable);
	UAM_ASSERT_EQ(admission.Admit(Ask("bg", "A", WorkClass::Background, 0.0)).message, std::string("Waiting for host A to be reachable"));
	UAM_ASSERT_EQ(admission.Admit(Ask("agent", "A", WorkClass::Agent, 0.0)).reason, Reason::Unreachable);
	// A user's own turn on an explicit computer keeps its existing connection attempt and error.
	UAM_ASSERT(admission.Admit(Ask("turn", "A", WorkClass::Interactive, 0.0)).admitted);
	admission.UpdateHosts({Host("A", 4)}, 1.0);
	UAM_ASSERT_EQ(admission.HealthOf("A"), Health::Ready);
	UAM_ASSERT(admission.Admit(Ask("agent", "A", WorkClass::Agent, 1.0)).admitted);
}

UAM_TEST(HostAdmissionWaiterThatStopsAskingLosesItsPlace)
{
	HostAdmission admission;
	admission.UpdateHosts({Host("A", 1)}, 0.0);
	admission.SetLive(Busy("A", 1));
	UAM_ASSERT(!admission.Admit(Ask("gone", "A", WorkClass::Interactive, 0.0)).admitted);
	admission.SetLive({});
	// Silent for longer than the stale limit, it no longer holds the slot.
	UAM_ASSERT(admission.Admit(Ask("present", "A", WorkClass::Interactive, HostAdmission::kQueueStaleSeconds + 1.0)).admitted);
	admission.SetLive({{"present", "A", WorkClass::Interactive}});
	UAM_ASSERT(!admission.Admit(Ask("later", "A", WorkClass::Interactive, HostAdmission::kQueueStaleSeconds + 1.0)).admitted);
	UAM_ASSERT_EQ(admission.QueuedKeys("A", HostAdmission::kQueueStaleSeconds + 1.0), std::vector<std::string>{"later"});
	// After the forget limit both tickets are gone; asking again would join the back of the line.
	admission.SetLive({});
	const double back = HostAdmission::kQueueForgetSeconds + 10.0;
	UAM_ASSERT(admission.Admit(Ask("fresh", "A", WorkClass::Interactive, back)).admitted);
	UAM_ASSERT(!admission.Withdraw("later"));
	UAM_ASSERT(!admission.Withdraw("gone"));
}

UAM_TEST(HostAdmissionLocalLimitAndPeekLeaveTheLineUnchanged)
{
	HostAdmission admission;
	admission.UpdateHosts({Host("local", 6)}, 0.0);
	admission.SetLive(Busy("local", 6));
	const Decision peek = admission.Peek(Ask("turn", "local", WorkClass::Interactive, 0.0));
	UAM_ASSERT(!peek.admitted);
	UAM_ASSERT_EQ(peek.message, std::string("Waiting for this computer (6/6)"));
	UAM_ASSERT(admission.QueuedKeys("local", 0.0).empty());
	admission.SetLive(Busy("local", 5));
	UAM_ASSERT(admission.Peek(Ask("turn", "local", WorkClass::Interactive, 0.0)).admitted);
	UAM_ASSERT(admission.QueuedKeys("local", 0.0).empty());
}

UAM_TEST(HostAdmissionLoadSampleFromFiguresNeverReadsUnknownAsIdle)
{
	const LoadSample unix_like = uam::host_admission::LoadSampleFromFigures(8u, 6.0, std::nullopt, 4ull << 30, 16ull << 30);
	UAM_ASSERT(unix_like.known);
	UAM_ASSERT(unix_like.cpu_percent && *unix_like.cpu_percent > 74.9 && *unix_like.cpu_percent < 75.1);
	UAM_ASSERT(unix_like.free_memory_percent && *unix_like.free_memory_percent > 24.9 && *unix_like.free_memory_percent < 25.1);
	const LoadSample windows = uam::host_admission::LoadSampleFromFigures(8u, std::nullopt, 42.0, std::nullopt, std::nullopt);
	UAM_ASSERT(windows.known && windows.cpu_percent && *windows.cpu_percent == 42.0 && !windows.free_memory_percent);
	const LoadSample saturated = uam::host_admission::LoadSampleFromFigures(2u, 9.0, std::nullopt, std::nullopt, std::nullopt);
	UAM_ASSERT(saturated.cpu_percent && *saturated.cpu_percent == 100.0);
	UAM_ASSERT(!uam::host_admission::LoadSampleFromFigures(std::nullopt, 3.0, std::nullopt, std::nullopt, 0ull).known);
	UAM_ASSERT(!uam::host_admission::LoadSampleFromFigures(std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt).known);
}

UAM_TEST(HostAdmissionOverloadedFallsBackToDegradedWhileStillBusy)
{
	HostAdmission admission;
	// One hard reading (low memory), then the host stays over the soft limit only.
	admission.UpdateHosts({Host("A", 4, 2, Load(80.0, 5.0))}, 0.0);
	UAM_ASSERT_EQ(admission.HealthOf("A"), Health::Overloaded);
	for (double now = 1.0; now < HostAdmission::kBackoffInitialSeconds; now += 1.0)
	{
		admission.UpdateHosts({Host("A", 4, 2, Load(80.0, 50.0))}, now);
		UAM_ASSERT_EQ(admission.HealthOf("A"), Health::Overloaded);
	}
	for (double now = HostAdmission::kBackoffInitialSeconds; now < 600.0; now += 1.0)
		admission.UpdateHosts({Host("A", 4, 2, Load(80.0, 50.0))}, now);
	// Busy but not overloaded: agents run again, background work still waits.
	UAM_ASSERT_EQ(admission.HealthOf("A"), Health::Degraded);
	UAM_ASSERT(admission.Admit(Ask("agent", "A", WorkClass::Agent, 600.0)).admitted);
	UAM_ASSERT_EQ(admission.Admit(Ask("bg", "A", WorkClass::Background, 600.0)).reason, Reason::Busy);
}

UAM_TEST(HostAdmissionStartReservationHoldsASlotUntilItBecomesLiveOrExpires)
{
	HostAdmission admission;
	admission.UpdateHosts({Host("A", 1)}, 0.0);
	Request start = Ask("turn:cold", "A", WorkClass::Interactive, 0.0, true);
	start.hold_seconds = 30.0;
	UAM_ASSERT(admission.Admit(start).admitted);
	// The starting process holds the slot before its turn is live.
	UAM_ASSERT_EQ(admission.Admit(Ask("turn:other", "A", WorkClass::Interactive, 1.0)).reason, Reason::SlotsFull);
	// Live under the same key it is counted once; the reservation itself is not counted twice.
	admission.SetLive({{"turn:cold", "A", WorkClass::Interactive}});
	UAM_ASSERT_EQ(admission.Usage(2.0).front().interactive, std::size_t{1});
	// A start that never became live gives the slot back after its reservation expires.
	admission.SetLive({});
	UAM_ASSERT(!admission.Admit(Ask("turn:other", "A", WorkClass::Interactive, 29.0)).admitted);
	UAM_ASSERT(admission.Admit(Ask("turn:other", "A", WorkClass::Interactive, 30.5)).admitted);
}
