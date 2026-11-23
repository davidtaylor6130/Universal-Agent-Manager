#include "test_harness.h"
#include "common/platform/background_thread_priority.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <string>

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace uam_test;

namespace
{
	// Returns a comparable rank where a larger value means more CPU priority.
	int CurrentThreadPriorityRank()
	{
#if defined(__APPLE__)
		qos_class_t qos = QOS_CLASS_UNSPECIFIED;
		int relative = 0;
		(void)pthread_get_qos_class_np(pthread_self(), &qos, &relative);
		if (qos == QOS_CLASS_UNSPECIFIED) qos = QOS_CLASS_DEFAULT;
		return static_cast<int>(qos) * 100 + relative;
#elif defined(_WIN32)
		return GetThreadPriority(GetCurrentThread());
#else
		return -getpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)));
#endif
	}
}

UAM_TEST(BackgroundWorkerThreadRunsBelowSpawningUiThread)
{
	const int ui_rank = CurrentThreadPriorityRank();
	int worker_rank = ui_rank;
	uam::platform::StartBackgroundThread([&worker_rank]() { worker_rank = CurrentThreadPriorityRank(); }).join();
#if defined(_WIN32)
	UAM_ASSERT(worker_rank < ui_rank);
#elif defined(__APPLE__)
	// A parent already at or below utility is never raised.
	UAM_ASSERT(worker_rank < ui_rank || ui_rank <= static_cast<int>(QOS_CLASS_UTILITY) * 100);
#else
	// A host already at nice >= 10 is never raised, so only require the background floor.
	UAM_ASSERT(worker_rank <= ui_rank);
	UAM_ASSERT(worker_rank <= -10);
#endif
	// The UI thread itself must keep its priority.
	UAM_ASSERT_EQ(CurrentThreadPriorityRank(), ui_rank);
}

namespace
{
	std::string ReadSource(const char* relative)
	{
		std::ifstream input(std::filesystem::path(UAM_TEST_SOURCE_DIR) / relative);
		std::stringstream text;
		text << input.rdbuf();
		return text.str();
	}

	// Returns the body of the lambda whose capture list starts at or after `from` (brace matched).
	std::string LambdaBodyAfter(const std::string& text, std::size_t from)
	{
		std::size_t open = text.find('{', from);
		if (open == std::string::npos) return {};
		int depth = 0;
		for (std::size_t at = open; at < text.size(); ++at)
		{
			if (text[at] == '{') ++depth;
			else if (text[at] == '}' && --depth == 0) return text.substr(open, at - open + 1);
		}
		return {};
	}

	// Body of the worker lambda that contains `at`, found from the nearest preceding thread spawn.
	std::string EnclosingWorker(const std::string& text, std::size_t at)
	{
		const std::size_t jthread = text.rfind("std::jthread>(", at);
		const std::size_t thread = text.rfind("std::thread(", at);
		std::size_t spawn = jthread == std::string::npos ? thread : thread == std::string::npos ? jthread : std::max(jthread, thread);
		return spawn == std::string::npos ? std::string{} : LambdaBodyAfter(text, spawn);
	}

	constexpr const char* kLower = "LowerCurrentThreadToBackgroundPriority";
}

UAM_TEST(ChatSaveWorkersRiseForBarriersAndStopWorkersKeepDefaultPriority)
{
	// Chat-save workers run lowered so they cannot starve the UI poll (#526), but force, shutdown and
	// synchronous-save barriers block the UI on them, so every such barrier raises them first (#647).
	// Terminal-media and stop workers are waited on by exit settlement and user stops and stay unlowered.
	// ponytail: source scan, replace with a priority probe if these workers gain an injection seam.
	const std::string runtime = ReadSource("src/common/runtime/acp/acp_session_runtime.cpp");
	const std::string lifecycle = ReadSource("src/app/chat_lifecycle_service.cpp");
	const std::string orchestration = ReadSource("src/app/runtime_orchestration_services.cpp");
	UAM_ASSERT(!runtime.empty() && !lifecycle.empty() && !orchestration.empty());

	UAM_ASSERT(runtime.find("StartBackgroundThread(") == std::string::npos);
	UAM_ASSERT(lifecycle.find("StartBackgroundThread(") == std::string::npos);
	UAM_ASSERT(lifecycle.find(kLower) == std::string::npos);

	// Only agent preparation and cold-history preparation may lower themselves in the runtime.
	int lowered = 0;
	for (std::size_t at = runtime.find(kLower); at != std::string::npos; at = runtime.find(kLower, at + 1))
	{
		const std::string worker = EnclosingWorker(runtime, at);
		UAM_ASSERT(worker.find("CaptureSelectedPrimary") != std::string::npos || worker.find("LoadColdHistoryPreparation") != std::string::npos);
		++lowered;
	}
	UAM_ASSERT_EQ(lowered, 2);

	// Every chat-save worker starts through the barrier-raisable helper; none is a plain detached thread.
	for (const char* worker : {"reader", "writer", "worker", "publisher", "queue_writer", "baseline"})
		UAM_ASSERT(runtime.find(std::string("std::thread(std::move(") + worker + "))") == std::string::npos);
	int save_workers = 0;
	for (std::size_t at = runtime.find("StartChatSaveWorker(*task, std::move("); at != std::string::npos;
	     at = runtime.find("StartChatSaveWorker(*task, std::move(", at + 1)) ++save_workers;
	UAM_ASSERT_EQ(save_workers, 7);
	UAM_ASSERT(runtime.find("platform::StartBarrierRaisableBackgroundThread(priority") != std::string::npos);
	// The barriers: synchronous saves and the force path (through the queued-admission barrier), and the
	// forced empty-held and owned publications, raise before they wait.
	const auto body_of = [&runtime](const char* signature)
	{
		const std::size_t at = runtime.find(signature);
		return at == std::string::npos ? std::string{} : LambdaBodyAfter(runtime, at);
	};
	UAM_ASSERT(body_of("void FinishPendingQueuedAdmissionSave(AppState& app").find("RaiseChatSaveWorkersForBarrier(*task);") != std::string::npos);
	UAM_ASSERT(body_of("static bool PollEmptyHeldSend(").find("if (force) RaiseChatSaveWorkersForBarrier(*task);") != std::string::npos);
	UAM_ASSERT(body_of("static bool PollOwnedChatPublication(").find("if (force) RaiseChatSaveWorkersForBarrier(*task);") != std::string::npos);
	const std::string flush = body_of("bool FlushPendingChatSaves(AppState& app, bool force)");
	const std::size_t force_block = flush.find("if (force)\n");
	UAM_ASSERT(force_block != std::string::npos);
	UAM_ASSERT(flush.find("acp_detail::FinishPendingQueuedAdmissionSave(app, chat_id);", force_block) < flush.find("task->published.get()", force_block));

	int stops = 0;
	for (std::size_t at = runtime.find("StopStdioProcessObserved(service, *owned"); at != std::string::npos;
	     at = runtime.find("StopStdioProcessObserved(service, *owned", at + 1), ++stops)
	{
		const std::string worker = EnclosingWorker(runtime, at);
		UAM_ASSERT(!worker.empty() && worker.find(kLower) == std::string::npos);
	}
	UAM_ASSERT(stops >= 2);

	for (const char* phase : {"TerminalMediaPhase::Preparing, std::memory_order_release", "TerminalMediaPhase::Finishing, std::memory_order_release"})
	{
		const std::size_t at = orchestration.find(phase);
		UAM_ASSERT(at != std::string::npos);
		const std::string worker = LambdaBodyAfter(orchestration, orchestration.find("std::thread(", at));
		UAM_ASSERT(!worker.empty() && worker.find(kLower) == std::string::npos);
	}
}

UAM_TEST(BarrierRaisableWorkerRunsLoweredUntilABarrierRaisesIt)
{
	const int ui_rank = CurrentThreadPriorityRank();
	const auto priority = std::make_shared<uam::platform::BarrierRaisableWorkerPriority>();
	std::promise<int> lowered_rank;
	std::promise<void> raised;
	std::promise<int> raised_rank;
	std::thread worker = uam::platform::StartBarrierRaisableBackgroundThread(priority, [&]()
	{
		lowered_rank.set_value(CurrentThreadPriorityRank());
		raised.get_future().wait();
		raised_rank.set_value(CurrentThreadPriorityRank());
	});
	const int lowered = lowered_rank.get_future().get();
	UAM_ASSERT(priority->RunningLowered());
#if defined(_WIN32)
	UAM_ASSERT(lowered < ui_rank);
#elif defined(__APPLE__)
	UAM_ASSERT(lowered < ui_rank || ui_rank <= static_cast<int>(QOS_CLASS_UTILITY) * 100);
#else
	UAM_ASSERT(lowered <= ui_rank && lowered <= -10);
#endif
	priority->RaiseForBarrier();
	priority->RaiseForBarrier(); // Idempotent.
	UAM_ASSERT(!priority->RunningLowered());
	raised.set_value();
	const int after = raised_rank.get_future().get();
	worker.join();
	UAM_ASSERT(priority->Finished());
#if defined(_WIN32)
	// Windows raises the worker's own thread priority back to where it started.
	UAM_ASSERT_EQ(after, ui_rank);
#else
	// macOS raises through a QoS override, which the requested class does not show; Linux needs privilege.
	(void)after;
#endif
	priority->RaiseForBarrier(); // After the worker left: a no-op.
	UAM_ASSERT(priority->Finished());
}

UAM_TEST(BarrierRaisedBeforeStartKeepsWorkerAtDefaultPriority)
{
	const int ui_rank = CurrentThreadPriorityRank();
	const auto priority = std::make_shared<uam::platform::BarrierRaisableWorkerPriority>();
	priority->RaiseForBarrier();
	int worker_rank = 0;
	uam::platform::StartBarrierRaisableBackgroundThread(priority, [&worker_rank]() { worker_rank = CurrentThreadPriorityRank(); }).join();
	UAM_ASSERT_EQ(worker_rank, ui_rank);
	UAM_ASSERT(priority->Finished() && !priority->RunningLowered());
}
