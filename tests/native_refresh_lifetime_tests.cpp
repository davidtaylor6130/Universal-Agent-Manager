#include "test_harness.h"
#include "common/runtime/native_refresh_lifetime.h"
#include <future>

namespace
{
	struct RetirementProbe
	{
		std::shared_ptr<std::promise<std::thread::id>> entered;
		std::shared_future<void> release;
		std::shared_ptr<std::promise<void>> finished;
		int observed = 0;
		int* receipt = nullptr;
		~RetirementProbe() noexcept
		{
			if (entered) entered->set_value(std::this_thread::get_id());
			if (release.valid()) release.wait();
			if (receipt) *receipt = observed;
			if (finished) finished->set_value();
		}
	};
	bool Await(std::future<void>& future) { return future.wait_for(std::chrono::seconds(3)) == std::future_status::ready; }
	bool AwaitRetirement()
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (uam::native_history::detail::refresh_owners.load() != 0 && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		return uam::native_history::detail::refresh_owners.load() == 0;
	}
}

UAM_TEST(NativeRefreshLifetimeDisposesDiscardedTaskWithoutBlockingReleasingThread)
{
	auto work = uam::native_history::TryCreateWorkerOwnedNativeRefresh<RetirementProbe>();
	UAM_ASSERT(work);
	auto entered = std::make_shared<std::promise<std::thread::id>>(); auto entering = entered->get_future();
	auto finished = std::make_shared<std::promise<void>>(); auto finishing = finished->get_future();
	std::promise<void> release; work->release = release.get_future().share();
	work->entered = entered; work->finished = finished;
	int observed = 0; work->observed = 42; work->receipt = &observed;
	// Simulate an accepted callback task discarded without executing completion.
	std::function<void()> queued_task = [work] {}; work.reset();
	std::promise<void> returned; auto returning = returned.get_future();
	std::thread releasing([task = std::move(queued_task), &returned]() mutable { task = {}; returned.set_value(); });
	// A moved-from std::function is valid but may retain a small target.
	// Explicitly release the source so the queued task is the last owner.
	queued_task = {};
	const auto releasing_id = releasing.get_id();
	const bool returned_before_cleanup = Await(returning);
	const bool cleanup_started = entering.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
	release.set_value(); releasing.join();
	const bool cleanup_finished = Await(finishing);
	const bool permit_retired = AwaitRetirement();
	UAM_ASSERT(permit_retired);
	UAM_ASSERT(returned_before_cleanup && cleanup_started && cleanup_finished);
	UAM_ASSERT(entering.get() != releasing_id); UAM_ASSERT_EQ(observed, 42);
}

UAM_TEST(NativeRefreshLifetimeBoundsOutstandingWorkUntilCleanupFinishes)
{
	auto first = uam::native_history::TryCreateWorkerOwnedNativeRefresh<RetirementProbe>();
	auto second = uam::native_history::TryCreateWorkerOwnedNativeRefresh<RetirementProbe>();
	UAM_ASSERT(first && second);
	UAM_ASSERT(!uam::native_history::TryCreateWorkerOwnedNativeRefresh<RetirementProbe>());
	auto finished = std::make_shared<std::promise<void>>(); auto finishing = finished->get_future();
	std::promise<void> release; first->release = release.get_future().share(); first->finished = finished;
	first.reset();
	UAM_ASSERT(!uam::native_history::TryCreateWorkerOwnedNativeRefresh<RetirementProbe>());
	release.set_value(); UAM_ASSERT(Await(finishing));
	std::shared_ptr<RetirementProbe> replacement;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!replacement && std::chrono::steady_clock::now() < deadline)
	{
		replacement = uam::native_history::TryCreateWorkerOwnedNativeRefresh<RetirementProbe>();
		if (!replacement) std::this_thread::yield();
	}
	UAM_ASSERT(replacement);
	second.reset(); replacement.reset();
	UAM_ASSERT(AwaitRetirement());
}
