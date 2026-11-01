#include "test_harness.h"
#include "common/platform/background_thread_priority.h"

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
	UAM_ASSERT(worker_rank < ui_rank);
	// The UI thread itself must keep its priority.
	UAM_ASSERT_EQ(CurrentThreadPriorityRank(), ui_rank);
}
