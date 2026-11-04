#include "test_harness.h"
#include "common/platform/background_thread_priority.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
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

UAM_TEST(DurableSaveAndStopWorkersKeepDefaultPriority)
{
	// Force/shutdown/exit barriers block the UI on chat-save and terminal-media workers, and stop
	// workers serve a user stop, so none of them may be lowered (#647).
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
