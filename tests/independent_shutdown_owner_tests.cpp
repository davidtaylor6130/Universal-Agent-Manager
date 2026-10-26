#include "test_harness.h"
#include "support/independent_shutdown_owner.h"
#include "common/runtime/acp/acp_process_handoff.h"
#include "common/state/app_state.h"
#include "common/provider/provider_ids.h"
#include <new>
#include <type_traits>

using namespace uam_test;
namespace sf = uam_test::shutdown_fixture;
static_assert(!std::is_copy_constructible_v<sf::IndependentOwner> && !std::is_move_constructible_v<sf::IndependentOwner>);

namespace
{
	// Entirely injected observations: no PID, fd, OS wait/signal, socket or thread.
	struct WaitFacts
	{
		sf::Origin origin = std::make_shared<const char>(0), wait_origin = origin, absent_origin = origin;
		bool terminal = true, absent = true, close = true;
		int rescues = 0, waits = 0, absences = 0, closes = 0;
	};
	class InjectedWait final : public sf::WaitObligation
	{
		std::shared_ptr<WaitFacts> facts;
	  public:
		explicit InjectedWait(std::shared_ptr<WaitFacts> value) : facts(std::move(value)) {}
		sf::Origin CreatorOrigin() const noexcept override { return facts->origin; }
		void RescueOriginal() override { ++facts->rescues; }
		sf::Observation WaitTerminal() override { ++facts->waits; return {facts->wait_origin, facts->terminal}; }
		sf::Observation ObserveAbsent() override { ++facts->absences; return {facts->absent_origin, facts->absent}; }
		bool CloseOwnedHandle() override { ++facts->closes; return facts->close; }
	};
	struct ChannelFacts
	{
		sf::Origin origin = std::make_shared<const char>(0), process;
		bool joined = true, gate = true, drain = true, throw_stop = false, foreign = false;
		unsigned writers = 0;
		int releases = 0, transfers = 0, stops = 0, closes = 0;
		std::function<void()> on_release;
	};
	class InjectedExclusive final : public sf::ExclusiveChannel
	{
		std::shared_ptr<ChannelFacts> facts;
		sf::Origin origin;
	  public:
		explicit InjectedExclusive(std::shared_ptr<ChannelFacts> value) : facts(std::move(value)), origin(facts->foreign ? std::make_shared<const char>(0) : facts->origin) {}
		sf::Origin CreatorOrigin() const noexcept override { return origin; }
		sf::Origin ProcessOrigin() const noexcept override { return facts->process; }
		void RequestStop() override
		{
			++facts->stops;
			if (facts->throw_stop) throw std::runtime_error("Injected uncertain control write");
		}
		bool DrainAndClose() override { ++facts->closes; return facts->drain; }
	};
	class InjectedOriginal final : public sf::OriginalChannel
	{
		std::shared_ptr<ChannelFacts> facts;
	  public:
		explicit InjectedOriginal(std::shared_ptr<ChannelFacts> value) : facts(std::move(value)) {}
		sf::Origin CreatorOrigin() const noexcept override { return facts->origin; }
		sf::Origin ProcessOrigin() const noexcept override { return facts->process; }
		std::unique_ptr<sf::ExclusiveChannel> QuiesceAndTransfer() override
		{
			++facts->transfers;
			if (!facts->joined || facts->writers != 0) return {};
			return std::make_unique<InjectedExclusive>(facts);
		}
		bool ReleaseStartGate() override
		{
			++facts->releases;
			if (facts->on_release) facts->on_release();
			return facts->gate;
		}
	};
	struct InjectedShutdownFixture
	{
		sf::IndependentOwner owner;
		std::shared_ptr<WaitFacts> child = std::make_shared<WaitFacts>(), watchdog = std::make_shared<WaitFacts>();
		std::shared_ptr<ChannelFacts> channel = std::make_shared<ChannelFacts>();

		InjectedShutdownFixture() { channel->process = child->origin; }
		void Capture(sf::IndependentOwner::CreationSlot& slot, bool with_watchdog = true)
		{
			std::unique_ptr<sf::WaitObligation> primary = std::make_unique<InjectedWait>(child);
			UAM_ASSERT(slot.CaptureChild(primary) && !primary);
			if (with_watchdog)
			{
				std::unique_ptr<sf::WaitObligation> watch = std::make_unique<InjectedWait>(watchdog);
				UAM_ASSERT(slot.CaptureWatchdog(watch) && !watch);
			}
			else UAM_ASSERT(slot.NoWatchdogCreated());
			std::unique_ptr<sf::OriginalChannel> original = std::make_unique<InjectedOriginal>(channel);
			UAM_ASSERT(slot.CaptureOriginalChannel(original) && !original);
		}
	};
}

UAM_TEST(IndependentShutdownOwnerRegistersBeforeStartAndRejectsLateAdoption)
{
	InjectedShutdownFixture fixture;
	auto slot = fixture.owner.ReserveBeforeCreate();
	UAM_ASSERT(!slot.ReleaseStart());
	fixture.Capture(slot);
	fixture.channel->on_release = [&] { UAM_ASSERT_EQ(fixture.owner.Pending(), std::size_t{1}); };
	UAM_ASSERT(slot.ReleaseStart());
	UAM_ASSERT(!slot.ReleaseStart());
	UAM_ASSERT_EQ(fixture.channel->releases, 1);
	UAM_ASSERT(fixture.owner.RescueAndWait());
	std::unique_ptr<sf::WaitObligation> late = std::make_unique<InjectedWait>(std::make_shared<WaitFacts>());
	UAM_ASSERT(!slot.CaptureChild(late) && late); // Rejection retains the caller's actual resource.
	UAM_ASSERT(!slot.ReleaseStart());
}

UAM_TEST(IndependentShutdownOwnerRequiresExclusiveOriginalChannelHandoff)
{
	InjectedShutdownFixture fixture;
	{
		auto slot = fixture.owner.ReserveBeforeCreate();
		fixture.Capture(slot);
		UAM_ASSERT(slot.ReleaseStart());
	}
	fixture.channel->joined = false;
	fixture.channel->writers = 1;
	UAM_ASSERT(!fixture.owner.RescueAndWait());
	UAM_ASSERT_EQ(fixture.child->rescues, 0);
	UAM_ASSERT_EQ(fixture.channel->stops, 0);
	UAM_ASSERT_EQ(fixture.owner.PendingReceipts().front().channel, sf::Stage::OriginalHandoff);
	fixture.channel->joined = true;
	UAM_ASSERT(!fixture.owner.RescueAndWait()); // Joined worker alone does not retire an active writer.
	fixture.channel->writers = 0;
	UAM_ASSERT(fixture.owner.RescueAndWait());
	UAM_ASSERT_EQ(fixture.child->rescues, 1);
	UAM_ASSERT_EQ(fixture.channel->stops, 1);
}

UAM_TEST(IndependentShutdownOwnerAbsenceCannotReplaceActualTerminalWait)
{
	InjectedShutdownFixture fixture;
	auto slot = fixture.owner.ReserveBeforeCreate();
	fixture.Capture(slot);
	fixture.child->terminal = false;
	UAM_ASSERT(!fixture.owner.RescueAndWait());
	UAM_ASSERT_EQ(fixture.child->absences, 0);
	UAM_ASSERT_EQ(fixture.child->closes, 0);
	UAM_ASSERT_EQ(fixture.owner.Pending(), std::size_t{1});
	UAM_ASSERT_EQ(fixture.owner.PendingReceipts().front().child, sf::Stage::TerminalWait);
	UAM_ASSERT(fixture.watchdog->rescues == 0 && fixture.watchdog->waits == 0 && fixture.watchdog->closes == 0);
	fixture.child->terminal = true;
	fixture.child->absent = false;
	UAM_ASSERT(!fixture.owner.RescueAndWait());
	UAM_ASSERT_EQ(fixture.child->closes, 0);
	UAM_ASSERT_EQ(fixture.watchdog->rescues, 0);
	fixture.child->absent = true;
	UAM_ASSERT(fixture.owner.RescueAndWait());
	UAM_ASSERT_EQ(fixture.child->rescues, 1);
}

UAM_TEST(IndependentShutdownOwnerKeepsWatchdogUntilItsOwnWait)
{
	InjectedShutdownFixture fixture;
	auto slot = fixture.owner.ReserveBeforeCreate();
	fixture.Capture(slot);
	fixture.watchdog->terminal = false;
	UAM_ASSERT(!fixture.owner.RescueAndWait());
	UAM_ASSERT_EQ(fixture.child->closes, 1);
	UAM_ASSERT_EQ(fixture.watchdog->closes, 0);
	UAM_ASSERT_EQ(fixture.channel->closes, 0);
	UAM_ASSERT_EQ(fixture.owner.PendingReceipts().front().watchdog, sf::Stage::TerminalWait);
	fixture.watchdog->terminal = true;
	UAM_ASSERT(fixture.owner.RescueAndWait());
	UAM_ASSERT_EQ(fixture.child->closes, 1);
	UAM_ASSERT_EQ(fixture.watchdog->closes, 1);
}

UAM_TEST(IndependentShutdownOwnerRejectsForeignWaitAndAbsenceReceipts)
{
	for (const bool wait : {false, true})
	{
		InjectedShutdownFixture fixture;
		auto slot = fixture.owner.ReserveBeforeCreate();
		fixture.Capture(slot);
		auto& receipt = wait ? fixture.child->wait_origin : fixture.child->absent_origin;
		receipt = std::make_shared<const char>(0);
		UAM_ASSERT(!fixture.owner.RescueAndWait());
		UAM_ASSERT_EQ(fixture.child->closes, 0);
		receipt = fixture.child->origin;
		UAM_ASSERT(fixture.owner.RescueAndWait());
	}
}

UAM_TEST(IndependentShutdownOwnerSurvivesClientExceptionWithoutReplayingStop)
{
	InjectedShutdownFixture fixture;
	try
	{
		auto slot = fixture.owner.ReserveBeforeCreate();
		fixture.Capture(slot);
		UAM_ASSERT(slot.ReleaseStart());
		throw std::runtime_error("Injected client teardown failure");
	}
	catch (const std::runtime_error&) {}
	UAM_ASSERT_EQ(fixture.owner.Pending(), std::size_t{1});
	fixture.channel->throw_stop = true;
	UAM_ASSERT(!fixture.owner.RescueAndWait());
	const auto receipt = fixture.owner.PendingReceipts().front();
	UAM_ASSERT(receipt.adapter_threw && receipt.channel == sf::Stage::StopWrite);
	UAM_ASSERT(fixture.owner.RescueAndWait()); // Independent retained handles complete the rescue, no second write.
	UAM_ASSERT_EQ(fixture.channel->stops, 1);
	UAM_ASSERT_EQ(fixture.child->waits, 1);
	UAM_ASSERT_EQ(fixture.watchdog->waits, 1);
	UAM_ASSERT(fixture.owner.RescueAndWait());
	UAM_ASSERT_EQ(fixture.channel->stops, 1);
}

UAM_TEST(IndependentShutdownOwnerRetainsUnknownChannelAndPartialCreation)
{
	InjectedShutdownFixture fixture;
	auto slot = fixture.owner.ReserveBeforeCreate();
	fixture.Capture(slot);
	fixture.channel->foreign = true;
	UAM_ASSERT(!fixture.owner.RescueAndWait());
	UAM_ASSERT_EQ(fixture.child->rescues, 0);
	UAM_ASSERT_EQ(fixture.channel->stops, 0);
	fixture.channel->foreign = false;
	UAM_ASSERT(!fixture.owner.RescueAndWait()); // Later mutable DATA cannot replace the retained foreign handoff.
	UAM_ASSERT_EQ(fixture.owner.Pending(), std::size_t{1});

	sf::IndependentOwner partial;
	auto pending = partial.ReserveBeforeCreate();
	std::unique_ptr<sf::WaitObligation> child = std::make_unique<InjectedWait>(std::make_shared<WaitFacts>());
	UAM_ASSERT(pending.CaptureChild(child));
	UAM_ASSERT(!pending.ReleaseStart());
	UAM_ASSERT(!partial.RescueAndWait()); // Missing watchdog declaration remains an explicit open obligation.
	UAM_ASSERT_EQ(partial.Pending(), std::size_t{1});
}

UAM_TEST(IndependentShutdownOwnerRescuesPartialCreationBeforeAnyChannelExists)
{
	sf::IndependentOwner owner;
	auto slot = owner.ReserveBeforeCreate();
	auto facts = std::make_shared<WaitFacts>();
	std::unique_ptr<sf::WaitObligation> child = std::make_unique<InjectedWait>(facts);
	UAM_ASSERT(slot.NoWatchdogCreated());
	UAM_ASSERT(slot.CaptureChild(child));
	UAM_ASSERT(!slot.ReleaseStart());
	UAM_ASSERT(owner.RescueAndWait());
	UAM_ASSERT(facts->rescues == 1 && facts->waits == 1 && facts->absences == 1 && facts->closes == 1);
}

UAM_TEST(IndependentShutdownOwnerEmptyReservationCannotBeReusedAfterCleanup)
{
	sf::IndependentOwner owner;
	auto slot = owner.ReserveBeforeCreate();
	UAM_ASSERT(owner.RescueAndWait());
	std::unique_ptr<sf::WaitObligation> late = std::make_unique<InjectedWait>(std::make_shared<WaitFacts>());
	UAM_ASSERT(!slot.CaptureChild(late) && late);
	UAM_ASSERT(!slot.NoWatchdogCreated() && !slot.ReleaseStart());
}

UAM_TEST(IndependentShutdownOwnerRetainsFailedHandleAndChannelClosure)
{
	InjectedShutdownFixture fixture;
	auto slot = fixture.owner.ReserveBeforeCreate();
	fixture.Capture(slot);
	fixture.child->close = false;
	UAM_ASSERT(!fixture.owner.RescueAndWait());
	UAM_ASSERT_EQ(fixture.owner.PendingReceipts().front().child, sf::Stage::HandleClose);
	fixture.child->close = true;
	fixture.channel->drain = false;
	UAM_ASSERT(!fixture.owner.RescueAndWait());
	UAM_ASSERT_EQ(fixture.owner.PendingReceipts().front().channel, sf::Stage::ChannelDrain);
	fixture.channel->drain = true;
	UAM_ASSERT(fixture.owner.RescueAndWait());
	UAM_ASSERT(fixture.child->waits == 1 && fixture.watchdog->waits == 1 && fixture.channel->stops == 1);
}

UAM_TEST(IndependentShutdownOwnerCannotReuseChildAsWatchdogOrOverfillRegistry)
{
	sf::IndependentOwner owner;
	auto slot = owner.ReserveBeforeCreate();
	auto facts = std::make_shared<WaitFacts>();
	std::unique_ptr<sf::WaitObligation> child = std::make_unique<InjectedWait>(facts);
	std::unique_ptr<sf::WaitObligation> duplicate = std::make_unique<InjectedWait>(facts);
	UAM_ASSERT(slot.CaptureChild(child));
	UAM_ASSERT(!slot.CaptureWatchdog(duplicate) && duplicate);
	UAM_ASSERT(slot.NoWatchdogCreated());
	for (int i = 1; i != 16; ++i) { auto reserved = owner.ReserveBeforeCreate(); }
	bool refused = false;
	try { auto excessive = owner.ReserveBeforeCreate(); }
	catch (const std::length_error&) { refused = true; }
	UAM_ASSERT(refused && owner.Pending() == 16);
	UAM_ASSERT(owner.RescueAndWait());
}

UAM_TEST(IndependentShutdownOwnerExpiryCannotReleaseSurvivingClientGate)
{
	InjectedShutdownFixture fixture;
	std::optional<sf::IndependentOwner::CreationSlot> slot;
	{
		sf::IndependentOwner outer;
		slot.emplace(outer.ReserveBeforeCreate());
		fixture.Capture(*slot);
	}
	UAM_ASSERT(!slot->ReleaseStart());
	UAM_ASSERT_EQ(fixture.channel->releases, 0);
	std::unique_ptr<sf::WaitObligation> late = std::make_unique<InjectedWait>(std::make_shared<WaitFacts>());
	UAM_ASSERT(!slot->CaptureChild(late) && late);
}

UAM_TEST(ShutdownProcessFieldHandoffRestoresOriginalOnPreWorkerException)
{
	uam::platform::StdioProcessPlatformFields source, destination;
#if defined(_WIN32)
	// Closed native handles, with only inert process-info DATA as the witness.
	source.process_info.dwProcessId = 23;
#else
	// All negative sentinels: no actual process or open descriptor is involved.
	source.child_pid = -7;
	source.watchdog_pid = -8;
	source.stdin_write_fd = -2;
#endif
	bool threw = false;
	try
	{
		uam::acp_detail::ScopedStdioProcessTransfer handoff(source, destination);
		throw std::runtime_error("Injected failure before worker construction completes");
	}
	catch (const std::runtime_error&) { threw = true; }
	UAM_ASSERT(threw);
#if defined(_WIN32)
	UAM_ASSERT(source.process_info.dwProcessId == 23 && destination.process_info.dwProcessId == 0);
	UAM_ASSERT(source.process_info.hProcess == INVALID_HANDLE_VALUE && destination.process_info.hProcess == INVALID_HANDLE_VALUE);
#else
	UAM_ASSERT(source.child_pid == -7 && source.watchdog_pid == -8 && source.stdin_write_fd == -2);
	UAM_ASSERT(destination.child_pid == -1 && destination.watchdog_pid == -1 && destination.stdin_write_fd == -1);
#endif
}

UAM_TEST(ShutdownProcessFieldHandoffCommitRetainsExclusiveDestination)
{
	uam::platform::StdioProcessPlatformFields source, destination;
#if defined(_WIN32)
	source.process_info.dwProcessId = 23;
#else
	source.child_pid = -7;
#endif
	{
		uam::acp_detail::ScopedStdioProcessTransfer handoff(source, destination);
		handoff.Commit();
	}
#if defined(_WIN32)
	UAM_ASSERT(source.process_info.dwProcessId == 0 && destination.process_info.dwProcessId == 23);
#else
	UAM_ASSERT(source.child_pid == -1 && destination.child_pid == -7);
#endif
}

namespace
{
	struct PendingAllocationFacts
	{
		bool fail = false;
		int allocations = 0;
	};

	template <typename T> struct PendingAllocation
	{
		using value_type = T;
		std::shared_ptr<PendingAllocationFacts> facts;
		explicit PendingAllocation(std::shared_ptr<PendingAllocationFacts> value) : facts(std::move(value))
		{
		}
		template <typename U> PendingAllocation(const PendingAllocation<U>& other) : facts(other.facts)
		{
		}
		T* allocate(std::size_t count)
		{
			++facts->allocations;
			if (facts->fail)
				throw std::bad_alloc();
			return std::allocator<T>{}.allocate(count);
		}
		void deallocate(T* value, std::size_t count) noexcept
		{
			std::allocator<T>{}.deallocate(value, count);
		}
		template <typename U> bool operator==(const PendingAllocation<U>& other) const noexcept
		{
			return facts == other.facts;
		}
	};

	void SeedInertProcessFields(uam::platform::StdioProcessPlatformFields& fields)
	{
		// A null alias retains only an ownership control block: no writer object,
		// worker, callable, process, descriptor or native handle is created.
		auto writer_owner = std::make_shared<const char>(0);
		fields.stdin_writer = std::shared_ptr<uam::platform::AsyncByteWriter>(writer_owner, nullptr);
#if defined(_WIN32)
		fields.stdin_write = fields.stdout_read = fields.stderr_read = nullptr;
		fields.process_info.dwProcessId = 23;
		fields.process_info.dwThreadId = 29;
		fields.job_object = INVALID_HANDLE_VALUE;
#else
		fields.stdin_write_fd = -11;
		fields.stdout_read_fd = -12;
		fields.stderr_read_fd = -13;
		fields.child_pid = -7;
		fields.watchdog_pid = -8;
		fields.reaped_exit_code = 17;
#if defined(__linux__)
		fields.deferred_reap_complete = std::make_shared<std::atomic<bool>>(false);
#endif
#endif
	}

	bool SameProcessFields(const uam::platform::StdioProcessPlatformFields& left,
	                       const uam::platform::StdioProcessPlatformFields& right)
	{
		const bool same_writer = left.stdin_writer == right.stdin_writer &&
		    !left.stdin_writer.owner_before(right.stdin_writer) && !right.stdin_writer.owner_before(left.stdin_writer);
#if defined(_WIN32)
		return same_writer && left.stdin_write == right.stdin_write && left.stdout_read == right.stdout_read &&
		    left.stderr_read == right.stderr_read && left.job_object == right.job_object &&
		    left.process_info.hProcess == right.process_info.hProcess && left.process_info.hThread == right.process_info.hThread &&
		    left.process_info.dwProcessId == right.process_info.dwProcessId && left.process_info.dwThreadId == right.process_info.dwThreadId;
#else
		return same_writer && left.stdin_write_fd == right.stdin_write_fd && left.stdout_read_fd == right.stdout_read_fd &&
		    left.stderr_read_fd == right.stderr_read_fd && left.child_pid == right.child_pid &&
		    left.watchdog_pid == right.watchdog_pid && left.reaped_exit_code == right.reaped_exit_code
#if defined(__linux__)
		    && left.deferred_reap_complete == right.deferred_reap_complete
#endif
		    ;
#endif
	}

	void CheckPendingPublication(bool restart, bool allocation_failure)
	{
		for (const char* provider : uam::provider_ids::kAllCliProviderIds)
		{
			uam::AcpSessionState session;
			session.provider_id = provider;
			session.running = true;
			session.transport_ownership_error = "original error";
			session.shutdown_chat_incarnation = std::make_shared<const std::uint8_t>(0);
			SeedInertProcessFields(session);
			const uam::platform::StdioProcessPlatformFields original = session;
			const auto chat_owners = session.shutdown_chat_incarnation.use_count();
			const auto runtime_owners = session.permission_review_incarnation.use_count();
			auto facts = std::make_shared<PendingAllocationFacts>();
			facts->fail = allocation_failure;
			using Entry = std::unique_ptr<uam::PendingAcpRemoteStop>;
			std::vector<Entry, PendingAllocation<Entry>> published{PendingAllocation<Entry>(facts)};
			auto pending = std::make_unique<uam::PendingAcpRemoteStop>();
			pending->chat_id = "original-chat";
			pending->projection_owner = uam::AcpStopProjectionOwner{
			    std::filesystem::path("original-root"), session.shutdown_chat_incarnation, session.permission_review_incarnation, &session};
			pending->restart_after_stop = restart;
			pending->recoverable_turn = true;
			pending->deadline_time_s = 42.0;
			bool threw = false;
			try
			{
				uam::acp_detail::PublishStdioProcessTransfer(session, std::move(pending), published);
				session.running = false;
				session.remote_stop_pending = true;
				session.transport_ownership_error.clear();
			}
			catch (const std::bad_alloc&)
			{
				threw = true;
			}
			UAM_ASSERT_EQ(threw, allocation_failure);
			UAM_ASSERT_EQ(facts->allocations, 1);
			if (allocation_failure)
			{
				UAM_ASSERT(published.empty() && session.running && !session.remote_stop_pending);
				UAM_ASSERT(SameProcessFields(session, original));
				UAM_ASSERT_EQ(session.transport_ownership_error, std::string("original error"));
				UAM_ASSERT_EQ(session.shutdown_chat_incarnation.use_count(), chat_owners);
				UAM_ASSERT_EQ(session.permission_review_incarnation.use_count(), runtime_owners);
			}
			else
			{
				UAM_ASSERT(published.size() == 1 && !session.running && session.remote_stop_pending);
				UAM_ASSERT(SameProcessFields(*published.front(), original));
				UAM_ASSERT(SameProcessFields(session, uam::platform::StdioProcessPlatformFields{}));
				UAM_ASSERT(session.transport_ownership_error.empty());
				const auto& owned = *published.front();
				UAM_ASSERT_EQ(owned.chat_id, std::string("original-chat"));
				UAM_ASSERT(owned.restart_after_stop == restart && owned.recoverable_turn && owned.deadline_time_s == 42.0);
				UAM_ASSERT(owned.projection_owner && owned.projection_owner->runtime == &session &&
				           owned.projection_owner->data_root == std::filesystem::path("original-root") &&
				           owned.projection_owner->chat_incarnation == session.shutdown_chat_incarnation &&
				           owned.projection_owner->runtime_incarnation == session.permission_review_incarnation);
			}
		}
	}
}

UAM_TEST(PendingRemoteStopPublicationPreservesOriginalOnAllocationFailure)
{
	CheckPendingPublication(false, true);
}

UAM_TEST(PendingRemoteRestartPublicationPreservesOriginalOnAllocationFailure)
{
	CheckPendingPublication(true, true);
}

UAM_TEST(PendingRemoteStopPublicationCommitsOriginalFieldsAndFence)
{
	CheckPendingPublication(false, false);
	CheckPendingPublication(true, false);
}
