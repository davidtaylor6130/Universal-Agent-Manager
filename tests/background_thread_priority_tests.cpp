#include "test_harness.h"
#include "common/platform/background_thread_priority.h"

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
#if defined(__APPLE__) || defined(_WIN32)
	UAM_ASSERT(worker_rank < ui_rank);
#else
	// A host already at nice >= 10 is never raised, so only require the background floor.
	UAM_ASSERT(worker_rank <= ui_rank);
	UAM_ASSERT(worker_rank <= -10);
#endif
	// The UI thread itself must keep its priority.
	UAM_ASSERT_EQ(CurrentThreadPriorityRank(), ui_rank);
}

UAM_TEST(DurableSaveAndStopWorkersKeepDefaultPriority)
{
	// Force/shutdown barriers block the UI on the save publication, baseline, prepare and queue
	// futures, and stop workers serve a user stop, so none of them may be lowered (#647).
	// ponytail: source scan, replace with a priority probe if these workers gain an injection seam.
	const std::filesystem::path root = std::filesystem::path(__FILE__).parent_path().parent_path();
	for (const char* relative : {"src/common/runtime/acp/acp_session_runtime.cpp", "src/app/chat_lifecycle_service.cpp"})
	{
		std::ifstream input(root / relative);
		UAM_ASSERT(input.good());
		std::stringstream text;
		text << input.rdbuf();
		UAM_ASSERT(text.str().find("StartBackgroundThread(") == std::string::npos);
		const std::size_t stop = text.str().find("StopStdioProcessObserved(service, *owned");
		if (stop == std::string::npos) continue;
		const std::size_t lowered = text.str().rfind("LowerCurrentThreadToBackgroundPriority", stop);
		UAM_ASSERT(lowered == std::string::npos || stop - lowered > 400);
	}
}
