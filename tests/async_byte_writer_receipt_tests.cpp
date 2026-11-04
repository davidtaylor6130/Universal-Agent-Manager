#include "test_harness.h"
#include "common/platform/async_byte_writer.h"

#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>

using namespace uam_test;

namespace
{
	using Writer = uam::platform::AsyncByteWriter;
	using Completion = Writer::WriteCompletion;

	// The bounded wait also settles a failed assertion's writer destructor.
	struct SinkBarrier
	{
		std::mutex mutex;
		std::condition_variable changed;
		bool entered = false;
		bool released = false;

		bool EnterAndWait()
		{
			std::unique_lock lock(mutex);
			entered = true;
			changed.notify_all();
			return changed.wait_for(lock, std::chrono::seconds(5), [&] { return released; });
		}
		void AwaitEntered()
		{
			std::unique_lock lock(mutex);
			UAM_ASSERT(changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }));
		}
		void Release()
		{
			std::lock_guard lock(mutex);
			released = true;
			changed.notify_all();
		}
	};

	void AwaitCompletion(const std::shared_ptr<const Writer::EnqueueReceipt>& receipt, Completion expected)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (receipt->Completion() == Completion::Pending && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		UAM_ASSERT(receipt->Completion() == expected);
	}
}

UAM_TEST(AsyncByteWriterReceiptWaitsForExactPartialWriteSuffix)
{
	SinkBarrier suffix;
	std::string accepted;
	unsigned calls = 0;
	Writer writer([&](const char* bytes, std::size_t len, std::string&) -> std::ptrdiff_t
	{
		if (++calls == 1)
		{
			accepted.append(bytes, 2);
			return 2;
		}
		if (!suffix.EnterAndWait()) return -1;
		accepted.append(bytes, len);
		return static_cast<std::ptrdiff_t>(len);
	}, {});
	std::shared_ptr<const Writer::EnqueueReceipt> receipt;
	UAM_ASSERT(writer.EnqueueObserved("four", 4, receipt) && receipt);
	suffix.AwaitEntered();
	UAM_ASSERT(writer.OwnsReceipt(*receipt));
	UAM_ASSERT(receipt->Completion() == Completion::Pending);
	suffix.Release();
	AwaitCompletion(receipt, Completion::Complete);
	UAM_ASSERT_EQ(accepted, std::string("four"));
}

UAM_TEST(AsyncByteWriterReceiptCompletedPrefixSurvivesLaterInputFailure)
{
	SinkBarrier later;
	std::atomic<unsigned> calls{0};
	Writer writer([&](const char*, std::size_t len, std::string& error) -> std::ptrdiff_t
	{
		if (++calls == 1) return static_cast<std::ptrdiff_t>(len);
		(void)later.EnterAndWait();
		error = "controlled sink failure";
		return -1;
	}, {});
	std::shared_ptr<const Writer::EnqueueReceipt> first, second;
	UAM_ASSERT(writer.EnqueueObserved("first", 5, first));
	UAM_ASSERT(writer.EnqueueObserved("second", 6, second));
	later.AwaitEntered();
	UAM_ASSERT(first->Completion() == Completion::Complete);
	UAM_ASSERT(second->Completion() == Completion::Pending);
	// Aggregate Flush would still wait here; the exact first entry is complete.
	later.Release();
	AwaitCompletion(second, Completion::Incomplete);
	UAM_ASSERT(first->Completion() == Completion::Complete);
	UAM_ASSERT(writer.FailureOrStall());
}

UAM_TEST(AsyncByteWriterReceiptPartialErrorCannotBecomeComplete)
{
	SinkBarrier failure;
	unsigned calls = 0;
	Writer writer([&](const char*, std::size_t, std::string& error) -> std::ptrdiff_t
	{
		if (++calls == 1) return 1;
		(void)failure.EnterAndWait();
		error = "controlled partial-write failure";
		return -1;
	}, {});
	std::shared_ptr<const Writer::EnqueueReceipt> receipt;
	UAM_ASSERT(writer.EnqueueObserved("partial", 7, receipt));
	failure.AwaitEntered();
	UAM_ASSERT(receipt->Completion() == Completion::Pending);
	failure.Release();
	AwaitCompletion(receipt, Completion::Incomplete);
	writer.Stop();
	UAM_ASSERT(receipt->Completion() == Completion::Incomplete);
}

UAM_TEST(AsyncByteWriterReceiptStopAndLifetimeCannotTransferOwnership)
{
	std::shared_ptr<const Writer::EnqueueReceipt> incomplete, completed;
	{
		SinkBarrier partial;
		std::atomic<unsigned> calls{0};
		Writer writer([&](const char*, std::size_t len, std::string&) -> std::ptrdiff_t
		{
			const unsigned call = ++calls;
			if (call == 1) return static_cast<std::ptrdiff_t>(len);
			if (call == 2) return 1;
			(void)partial.EnterAndWait();
			return 0;
		}, {});
		UAM_ASSERT(writer.EnqueueObserved("done", 4, completed));
		UAM_ASSERT(writer.EnqueueObserved("pending", 7, incomplete));
		partial.AwaitEntered();
		UAM_ASSERT(completed->Completion() == Completion::Complete);
		UAM_ASSERT(incomplete->Completion() == Completion::Pending);
		partial.Release();
		writer.Stop();
		UAM_ASSERT(incomplete->Completion() == Completion::Incomplete);
	}
	UAM_ASSERT(incomplete->Completion() == Completion::Incomplete);
	UAM_ASSERT(completed->Completion() == Completion::Complete); // Historical full sink acceptance survives lifetime loss.
	Writer replacement([](const char*, std::size_t len, std::string&) { return static_cast<std::ptrdiff_t>(len); }, {});
	UAM_ASSERT(!replacement.OwnsReceipt(*incomplete));
	UAM_ASSERT(!replacement.OwnsReceipt(*completed));
	std::shared_ptr<const Writer::EnqueueReceipt> own;
	UAM_ASSERT(replacement.EnqueueObserved("new", 3, own));
	AwaitCompletion(own, Completion::Complete);
	UAM_ASSERT(replacement.OwnsReceipt(*own));
	UAM_ASSERT(incomplete->Completion() == Completion::Incomplete);
}

UAM_TEST(AsyncByteWriterReceiptRejectsEmptyAndOversizedObservations)
{
	Writer writer([](const char*, std::size_t len, std::string&) { return static_cast<std::ptrdiff_t>(len); }, {});
	std::shared_ptr<const Writer::EnqueueReceipt> receipt;
	std::string error;
	UAM_ASSERT(writer.Enqueue(nullptr, 0)); // Existing empty-input semantics remain.
	UAM_ASSERT(!writer.EnqueueObserved(nullptr, 1, receipt, &error) && !receipt);
	UAM_ASSERT(!writer.EnqueueObserved("", 0, receipt, &error) && !receipt);
	UAM_ASSERT(!writer.EnqueueObserved("x", uam::platform::kAsyncInputMaxQueuedBytes + 1, receipt, &error) && !receipt);
	UAM_ASSERT(writer.FailureOrStall()); // Existing queue-bound rejection still fails the writer.
}

UAM_TEST(AsyncByteWriterReceiptInvalidSinkCountCannotBecomeComplete)
{
	Writer writer([](const char*, std::size_t len, std::string&) { return static_cast<std::ptrdiff_t>(len + 1); }, {});
	std::shared_ptr<const Writer::EnqueueReceipt> receipt;
	UAM_ASSERT(writer.EnqueueObserved("input", 5, receipt));
	AwaitCompletion(receipt, Completion::Incomplete);
	UAM_ASSERT(writer.FailureOrStall());
}

UAM_TEST(AsyncByteWriterReceiptStopDuringSinkWriteRemainsIncomplete)
{
	SinkBarrier in_flight;
	Writer writer([&](const char*, std::size_t len, std::string&) -> std::ptrdiff_t
	{
		if (!in_flight.EnterAndWait()) return -1;
		return static_cast<std::ptrdiff_t>(len);
	}, {});
	std::shared_ptr<const Writer::EnqueueReceipt> receipt;
	UAM_ASSERT(writer.EnqueueObserved("in-flight", 9, receipt));
	in_flight.AwaitEntered();
	std::jthread stopping([&] { writer.Stop(); });
	AwaitCompletion(receipt, Completion::Incomplete); // Stop won before completion could be published.
	in_flight.Release();
	stopping.join();
	UAM_ASSERT(receipt->Completion() == Completion::Incomplete);
}

UAM_TEST(AsyncByteWriterReceiptConcurrentReadsRetainCompletedPrefixDuringStop)
{
	SinkBarrier first_write, later_write;
	std::atomic<unsigned> calls{0};
	Writer writer([&](const char*, std::size_t len, std::string& error) -> std::ptrdiff_t
	{
		if (++calls == 1)
		{
			if (!first_write.EnterAndWait()) return -1;
			return static_cast<std::ptrdiff_t>(len);
		}
		(void)later_write.EnterAndWait();
		error = "controlled later failure during Stop";
		return -1;
	}, {});
	std::shared_ptr<const Writer::EnqueueReceipt> first, later;
	UAM_ASSERT(writer.EnqueueObserved("prefix", 6, first));
	UAM_ASSERT(writer.EnqueueObserved("later", 5, later));
	first_write.AwaitEntered();
	std::promise<Completion> before_completion;
	std::atomic<bool> sample_complete{false}, stop_observed{false}, bad{false};
	std::atomic<unsigned> completed_samples{0};
	std::jthread observer([&](std::stop_token stop)
	{
		before_completion.set_value(first->Completion());
		while (!sample_complete.load(std::memory_order_acquire) && !stop.stop_requested()) std::this_thread::yield();
		while (!stop_observed.load(std::memory_order_acquire) && !stop.stop_requested())
		{
			if (first->Completion() != Completion::Complete) bad.store(true);
			++completed_samples;
			std::this_thread::yield();
		}
		for (unsigned index = 0; index < 512 && !stop.stop_requested(); ++index)
		{
			if (first->Completion() != Completion::Complete) bad.store(true);
			++completed_samples;
		}
	});
	UAM_ASSERT(before_completion.get_future().get() == Completion::Pending);
	first_write.Release();
	later_write.AwaitEntered(); // The worker published prefix Complete before entering this sink.
	UAM_ASSERT(first->Completion() == Completion::Complete);
	sample_complete.store(true, std::memory_order_release);
	std::jthread stopping([&] { writer.Stop(); });
	AwaitCompletion(later, Completion::Incomplete); // Stop has finalized the pending entry.
	stop_observed.store(true, std::memory_order_release);
	later_write.Release();
	stopping.join();
	observer.join();
	UAM_ASSERT(!bad.load() && completed_samples.load() >= 512);
	UAM_ASSERT(first->Completion() == Completion::Complete);
	UAM_ASSERT(later->Completion() == Completion::Incomplete);
}
