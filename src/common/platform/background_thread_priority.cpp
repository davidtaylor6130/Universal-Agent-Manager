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

	BarrierRaisableWorkerPriority::~BarrierRaisableWorkerPriority()
	{
#if defined(_WIN32)
		if (m_thread != 0) (void)CloseHandle(reinterpret_cast<HANDLE>(m_thread));
#endif
	}

	void BarrierRaisableWorkerPriority::EnterWorker() noexcept
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		m_entered = true;
#if defined(__APPLE__)
		m_thread = reinterpret_cast<std::uintptr_t>(pthread_self());
		m_original_priority = static_cast<std::intptr_t>(qos_class_self());
#elif defined(_WIN32)
		m_thread = reinterpret_cast<std::uintptr_t>(OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId()));
		m_original_priority = GetThreadPriority(GetCurrentThread());
#else
		m_thread = static_cast<std::uintptr_t>(syscall(SYS_gettid));
		errno = 0;
		const int original = getpriority(PRIO_PROCESS, static_cast<id_t>(m_thread));
		m_original_priority = errno == 0 ? original : 0;
#endif
		if (m_raised) return;
		LowerCurrentThreadToBackgroundPriority();
		m_lowered = true;
	}

	void BarrierRaisableWorkerPriority::LeaveWorker() noexcept
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		m_left = true;
#if defined(__APPLE__)
		if (m_raise_token != nullptr) (void)pthread_override_qos_class_end_np(static_cast<pthread_override_t>(m_raise_token));
		m_raise_token = nullptr;
#elif defined(_WIN32)
		if (m_thread != 0) (void)CloseHandle(reinterpret_cast<HANDLE>(m_thread));
		m_thread = 0;
#endif
	}

	void BarrierRaisableWorkerPriority::RaiseForBarrier() noexcept
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		if (m_raised) return;
		m_raised = true;
		// Not started yet: EnterWorker sees m_raised and never lowers. Already left: nothing to raise.
		if (!m_entered || m_left || !m_lowered) return;
#if defined(__APPLE__)
		qos_class_t original = static_cast<qos_class_t>(m_original_priority);
		if (original == QOS_CLASS_UNSPECIFIED) original = QOS_CLASS_DEFAULT;
		m_raise_token = pthread_override_qos_class_start_np(reinterpret_cast<pthread_t>(m_thread), original, 0);
#elif defined(_WIN32)
		if (m_thread != 0) (void)SetThreadPriority(reinterpret_cast<HANDLE>(m_thread), static_cast<int>(m_original_priority));
#else
		(void)setpriority(PRIO_PROCESS, static_cast<id_t>(m_thread), static_cast<int>(m_original_priority));
#endif
	}

	bool BarrierRaisableWorkerPriority::Finished() const noexcept
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		return m_left;
	}

	bool BarrierRaisableWorkerPriority::RunningLowered() const noexcept
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		return m_entered && !m_left && m_lowered && !m_raised;
	}
} // namespace uam::platform
