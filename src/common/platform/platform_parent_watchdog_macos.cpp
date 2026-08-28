#include "common/platform/platform_application_macos.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <libproc.h>
#include <signal.h>
#include <sys/event.h>
#include <unistd.h>

namespace uam::platform
{
	namespace
	{
		bool ParseWatchdogInteger(const char* text, long long& value)
		{
			if (text == nullptr || *text == '\0') return false;
			char* end = nullptr;
			errno = 0;
			value = std::strtoll(text, &end, 10);
			return errno == 0 && end != text && *end == '\0' && value >= 0;
		}
	}

	std::optional<int> RunMacParentDeathWatchdogIfRequested(const int argc, char* argv[])
	{
		if (argc < 2 || std::strcmp(argv[1], kMacParentDeathWatchdogArgument) != 0)
			return std::nullopt;
		long long parent_value = 0;
		long long child_value = 0;
		long long start_seconds = 0;
		long long start_microseconds = 0;
		if (argc != 6 || !ParseWatchdogInteger(argv[2], parent_value) ||
		    !ParseWatchdogInteger(argv[3], child_value) ||
		    !ParseWatchdogInteger(argv[4], start_seconds) ||
		    !ParseWatchdogInteger(argv[5], start_microseconds) || parent_value <= 0 ||
		    child_value <= 0)
			return 2;

		const pid_t parent_pid = static_cast<pid_t>(parent_value);
		const pid_t child_pid = static_cast<pid_t>(child_value);
		const int queue = kqueue();
		struct kevent change
		{
		};
		EV_SET(&change, child_pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
		const bool armed = queue >= 0 && kevent(queue, &change, 1, nullptr, 0, nullptr) == 0;
		const char ready = armed ? '1' : '0';
		(void)write(3, &ready, 1);
		(void)close(3);
		if (!armed) return 1;

		for (;;)
		{
			struct kevent event
			{
			};
			const struct timespec timeout = {0, 100 * 1000 * 1000};
			const int event_count = kevent(queue, nullptr, 0, &event, 1, &timeout);
			if (event_count > 0) return 0;
			if (event_count < 0 && errno != EINTR)
			{
				(void)kill(-child_pid, SIGKILL);
				(void)kill(child_pid, SIGKILL);
				return 1;
			}
			if (getppid() != parent_pid)
			{
				struct proc_bsdinfo current_child_info
				{
				};
				const bool same_process = proc_pidinfo(
				    child_pid, PROC_PIDTBSDINFO, 0, &current_child_info,
				    sizeof(current_child_info)) == sizeof(current_child_info) &&
				    current_child_info.pbi_start_tvsec == start_seconds &&
				    current_child_info.pbi_start_tvusec == start_microseconds;
				if (same_process)
				{
					(void)kill(-child_pid, SIGKILL);
					(void)kill(child_pid, SIGKILL);
				}
				return 0;
			}
		}
	}
}
