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
#include <cerrno>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace uam::platform
{
	// Niceness applied to Linux background workers; CFS still gives them a weighted share.
	constexpr int kBackgroundWorkerNice = 10;

	void LowerCurrentThreadToBackgroundPriority() noexcept
	{
#if defined(__APPLE__)
		// Utility at minimum relative priority stays below a utility-clamped UI thread
		// (taskpolicy -c utility) without the I/O throttling of QOS_CLASS_BACKGROUND.
		// Never raise a parent already at or below utility (for example a background-clamped host).
		const qos_class_t current = qos_class_self();
		if (current != QOS_CLASS_UNSPECIFIED && current <= QOS_CLASS_UTILITY) return;
		(void)pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, QOS_MIN_RELATIVE_PRIORITY);
#elif defined(_WIN32)
		(void)SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
		// Linux niceness is per thread (tid); never raise an already-lower thread.
		const id_t tid = static_cast<id_t>(syscall(SYS_gettid));
		errno = 0; // getpriority can legitimately return -1, so only errno marks failure.
		const int current = getpriority(PRIO_PROCESS, tid);
		if (errno != 0 || current < kBackgroundWorkerNice) (void)setpriority(PRIO_PROCESS, tid, kBackgroundWorkerNice);
#endif
	}
} // namespace uam::platform
