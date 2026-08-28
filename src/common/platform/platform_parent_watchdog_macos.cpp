#include "common/platform/platform_application_macos.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <libproc.h>
#include <signal.h>
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
		struct proc_bsdinfo child_info
		{
		};
		const auto child_matches = [&]()
		{
			return proc_pidinfo(child_pid, PROC_PIDTBSDINFO, 0, &child_info,
			                    sizeof(child_info)) == sizeof(child_info) &&
			       child_info.pbi_start_tvsec == start_seconds &&
			       child_info.pbi_start_tvusec == start_microseconds;
		};
		const bool armed = child_matches();
		const char ready = armed ? '1' : '0';
		(void)write(3, &ready, 1);
		(void)close(3);
		if (!armed) return 1;

		for (;;)
		{
			(void)usleep(100 * 1000);
			if (!child_matches()) return 0;
			if (getppid() != parent_pid)
			{
				(void)kill(-child_pid, SIGKILL);
				(void)kill(child_pid, SIGKILL);
				return 0;
			}
		}
	}
}
