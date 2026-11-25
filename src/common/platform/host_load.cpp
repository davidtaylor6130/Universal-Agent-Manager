#include "common/platform/host_load.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <mutex>
#include <thread>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <unistd.h>
#else
#include <stdlib.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <string>
#endif

namespace uam::platform
{
	namespace
	{
#if defined(_WIN32)
		std::uint64_t FileTimeValue(const FILETIME& time)
		{
			return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
		}

		struct CpuTimes
		{
			std::uint64_t idle = 0;
			std::uint64_t total = 0;
		};

		bool ReadCpuTimes(CpuTimes& times)
		{
			FILETIME idle{};
			FILETIME kernel{};
			FILETIME user{};
			if (!GetSystemTimes(&idle, &kernel, &user)) return false;
			// Kernel time already includes idle time.
			times.idle = FileTimeValue(idle);
			times.total = FileTimeValue(kernel) + FileTimeValue(user);
			return true;
		}

		/// CPU use between this reading and the previous one. The first reading, or one after a long gap, measures a
		/// short window of its own, so a figure is never a since-boot average.
		std::optional<double> ReadCpuPercent()
		{
			static std::mutex mutex;
			static CpuTimes previous;
			static std::chrono::steady_clock::time_point previous_at;
			std::scoped_lock lock(mutex);
			const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
			if (previous.total == 0 || now - previous_at > std::chrono::minutes(1))
			{
				if (!ReadCpuTimes(previous)) return std::nullopt;
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
			}
			CpuTimes current;
			if (!ReadCpuTimes(current)) return std::nullopt;
			const bool ordered = current.total >= previous.total && current.idle >= previous.idle;
			const std::uint64_t total = ordered ? current.total - previous.total : 0;
			const std::uint64_t idle = ordered ? current.idle - previous.idle : 0;
			previous = current;
			previous_at = std::chrono::steady_clock::now();
			if (total == 0 || idle > total) return std::nullopt;
			return 100.0 * static_cast<double>(total - idle) / static_cast<double>(total);
		}
#elif !defined(__APPLE__)
		/// Reads MemTotal and MemAvailable (kB) from /proc/meminfo.
		void ReadLinuxMemory(HostLoad& load)
		{
			std::ifstream meminfo("/proc/meminfo");
			std::string line;
			while (std::getline(meminfo, line))
			{
				std::istringstream fields(line);
				std::string key;
				unsigned long long kilobytes = 0;
				if (!(fields >> key >> kilobytes)) continue;
				if (key == "MemTotal:") load.memory_total_bytes = static_cast<std::uint64_t>(kilobytes) * 1024ull;
				else if (key == "MemAvailable:") load.memory_available_bytes = static_cast<std::uint64_t>(kilobytes) * 1024ull;
			}
		}
#endif

#if !defined(_WIN32)
		void ReadPosixProcessorsAndLoad(HostLoad& load)
		{
			const long online = sysconf(_SC_NPROCESSORS_ONLN);
			if (online > 0) load.cores = static_cast<std::uint32_t>(online);
			double averages[1] = {0.0};
			if (getloadavg(averages, 1) == 1 && averages[0] >= 0.0) load.load_average_1m = averages[0];
		}
#endif
	}

	HostLoad ReadCurrentHostLoad() noexcept
	{
		HostLoad load;
		try
		{
#if defined(_WIN32)
			const DWORD processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
			if (processors > 0) load.cores = static_cast<std::uint32_t>(processors);
			load.cpu_percent = ReadCpuPercent();
			MEMORYSTATUSEX memory{};
			memory.dwLength = sizeof(memory);
			if (GlobalMemoryStatusEx(&memory))
			{
				load.memory_total_bytes = static_cast<std::uint64_t>(memory.ullTotalPhys);
				load.memory_available_bytes = static_cast<std::uint64_t>(memory.ullAvailPhys);
			}
#elif defined(__APPLE__)
			ReadPosixProcessorsAndLoad(load);
			std::uint64_t total = 0;
			std::size_t size = sizeof(total);
			if (sysctlbyname("hw.memsize", &total, &size, nullptr, 0) == 0 && total > 0) load.memory_total_bytes = total;
			mach_port_t host = mach_host_self();
			vm_statistics64_data_t statistics{};
			mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
			vm_size_t page_size = 0;
			if (host_page_size(host, &page_size) == KERN_SUCCESS &&
			    host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&statistics), &count) == KERN_SUCCESS && total > 0)
			{
				// Activity Monitor's "memory used": app memory (anonymous pages that cannot be purged), wired and compressed.
				const std::uint64_t internal = statistics.internal_page_count;
				const std::uint64_t purgeable = statistics.purgeable_count;
				const std::uint64_t used_pages = (internal > purgeable ? internal - purgeable : 0) +
				    static_cast<std::uint64_t>(statistics.wire_count) + static_cast<std::uint64_t>(statistics.compressor_page_count);
				const std::uint64_t used = used_pages * static_cast<std::uint64_t>(page_size);
				load.memory_available_bytes = used < total ? total - used : 0;
			}
			(void)mach_port_deallocate(mach_task_self(), host);
#else
			ReadPosixProcessorsAndLoad(load);
			ReadLinuxMemory(load);
#endif
		}
		catch (...)
		{
		}
		return load;
	}
}
