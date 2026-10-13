#include "diagnostic_log.h"

#include "common/utils/time_utils.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_lease.h"
#include <atomic>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <memory>
#include <thread>
#include <iostream>
#include <mutex>

namespace uam::diagnostics
{
namespace
{
	std::mutex g_lifecycleMutex;
	std::mutex g_queueMutex;
	std::condition_variable_any g_ready;
	std::condition_variable g_drained;
	std::deque<std::string> g_queue;
	std::size_t g_queuedBytes = 0;
	std::size_t g_droppedEntries = 0;
	bool g_accepting = false;
	bool g_writing = false;
	std::ofstream g_file;
	std::filesystem::path g_path;
	std::uintmax_t g_bytes = 0;
	std::unique_ptr<std::jthread> g_worker;
	uam::WorkspaceLeaseGate g_retentionGate;
	std::atomic<int> g_backupRetentionDays{14};
	constexpr std::size_t kMaxQueuedBytes = 512 * 1024;

	bool Open(std::ios_base::openmode mode)
	{
		// Special paths can redirect diagnostics or block desktop startup before the worker exists.
		std::error_code status_error;
		const std::filesystem::file_status status = std::filesystem::symlink_status(g_path, status_error);
		if ((status_error && status_error != std::errc::no_such_file_or_directory) ||
		    (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)) ||
		    uam::paths::IsLinkOrReparsePointNoThrow(g_path)) return false;
		g_file.clear();
		g_file.open(g_path, std::ios::binary | mode);
		if (!g_file.is_open()) return false;
		std::error_code error;
		std::filesystem::permissions(g_path,
		    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
		    std::filesystem::perm_options::replace, error);
		if (error) g_file.close();
		return !error;
	}

	bool Rotate()
	{
		g_file.close();
		g_file.clear();
		std::filesystem::path previous = g_path;
		previous += ".1";
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::symlink_status(previous, error);
		if (error && error != std::errc::no_such_file_or_directory) return false;
		if (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)) return false;
		error.clear();
		std::filesystem::remove(previous, error);
		if (error) return false;
		std::filesystem::rename(g_path, previous, error);
		if (error) return false;
		g_bytes = 0;
		return Open(std::ios::trunc);
	}

	void WriteLine(const std::string& line)
	{
		if (g_file.is_open())
		{
			if (g_bytes + line.size() <= kMaxFileBytes || Rotate())
			{
				g_file.write(line.data(), static_cast<std::streamsize>(line.size()));
				if (g_file)
				{
					g_bytes += line.size();
					return;
				}
			}
			g_file.close();
			std::cerr << "Diagnostic file logging failed; using stderr.\n";
		}
		std::cerr << line;
	}

	void ExpirePreviousLog()
	{
		const std::shared_ptr<uam::WorkspaceLeaseGate::Lease> lease = g_retentionGate.TryAcquireCleanup();
		if (!lease) return;
		const int days = g_backupRetentionDays.load();
		if (days == 0) return;
		std::filesystem::path previous = g_path;
		previous += ".1";
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::symlink_status(previous, error);
		if (error || !std::filesystem::is_regular_file(status)) return;
		const std::filesystem::file_time_type modified = std::filesystem::last_write_time(previous, error);
		if (!error && modified < std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * days))
		{
			std::filesystem::remove(previous, error);
			if (error) std::cerr << "Old diagnostic log could not be removed: " << error.message() << '\n';
		}
	}

	void Drain(std::stop_token stop)
	{
		ExpirePreviousLog();
		std::chrono::steady_clock::time_point next_expiry_check = std::chrono::steady_clock::now() + std::chrono::minutes(5);
		for (;;)
		{
			std::deque<std::string> batch;
			std::size_t dropped = 0;
			{
				std::unique_lock<std::mutex> lock(g_queueMutex);
				g_ready.wait_until(lock, stop, next_expiry_check, []() { return !g_queue.empty() || g_droppedEntries != 0; });
				if (g_queue.empty() && g_droppedEntries == 0 && stop.stop_requested()) break;
				batch.swap(g_queue);
				g_queuedBytes = 0;
				dropped = g_droppedEntries;
				g_droppedEntries = 0;
				g_writing = true;
			}
			if (std::chrono::steady_clock::now() >= next_expiry_check)
			{
				ExpirePreviousLog();
				next_expiry_check = std::chrono::steady_clock::now() + std::chrono::minutes(5);
			}
			if (dropped != 0) WriteLine(uam::time::IsoUtcTimestampNow() + " Logging dropped " + std::to_string(dropped) + " older entries because the disk queue was full.\n");
			for (const std::string& line : batch) WriteLine(line);
			if (g_file.is_open())
			{
				g_file.flush();
				if (!g_file)
				{
					g_file.close();
					std::cerr << "Diagnostic log flush failed; using stderr.\n";
				}
			}
			{
				std::lock_guard<std::mutex> lock(g_queueMutex);
				g_writing = false;
			}
			g_drained.notify_all();
		}
	}
}

bool ConfigureBackupRetention(int days)
{
	if (days < 0 || days > 3650) return false;
	const std::shared_ptr<uam::WorkspaceLeaseGate::Lease> lease = g_retentionGate.TryAcquireMutation();
	if (!lease) return false;
	g_backupRetentionDays.store(days);
	return true;
}

bool StartFileLog(const std::filesystem::path& path, std::string& error, int backup_retention_days)
{
	std::lock_guard<std::mutex> lock(g_lifecycleMutex);
	error.clear();
	if (g_worker || g_file.is_open())
	{
		error = "Diagnostic file logging is already active.";
		return false;
	}
	if (!ConfigureBackupRetention(backup_retention_days)) { error = "Diagnostic backup retention is invalid or busy."; return false; }
	g_path = path;
	if (Open(std::ios::app))
	{
		g_file.seekp(0, std::ios::end);
		const std::streampos end = g_file.tellp();
		if (end >= 0)
		{
			g_bytes = static_cast<std::uintmax_t>(end);
			if (g_bytes > kMaxFileBytes && !Rotate())
			{
				error = "Oversized diagnostic log could not be rotated; using stderr.";
				return false;
			}
			try
			{
				g_worker = std::make_unique<std::jthread>(Drain);
				std::lock_guard<std::mutex> queue_lock(g_queueMutex);
				g_accepting = true;
				return true;
			}
			catch (const std::system_error& exception)
			{
				error = "Diagnostic logging worker could not start: " + std::string(exception.what());
				g_file.close();
				return false;
			}
		}
	}
	g_file.close();
	error = "Diagnostic log could not be opened or exceeds its size limit; using stderr.";
	return false;
}

void FlushFileLog()
{
	std::unique_lock<std::mutex> lock(g_queueMutex);
	g_drained.wait(lock, []() { return g_queue.empty() && !g_writing && g_droppedEntries == 0; });
}

void StopFileLog()
{
	std::lock_guard<std::mutex> lock(g_lifecycleMutex);
	{
		std::lock_guard<std::mutex> queue_lock(g_queueMutex);
		g_accepting = false;
	}
	if (g_worker)
	{
		g_worker->request_stop();
		g_ready.notify_all();
		g_worker.reset();
	}
	g_file.close();
	g_file.clear();
	g_path.clear();
	g_bytes = 0;
}

void Write(std::string_view message)
{
	constexpr std::string_view truncation = " [truncated]";
	std::string line = uam::time::IsoUtcTimestampNow() + " ";
	const std::size_t body_limit = kMaxEntryBytes - line.size() - truncation.size() - 1;
	line.append(message.substr(0, body_limit));
	for (char& ch : line)
	{
		if (static_cast<unsigned char>(ch) < 32 || ch == 127) ch = ' ';
	}
	if (message.size() > body_limit) line.append(truncation);
	line.push_back('\n');
	{
		std::lock_guard<std::mutex> lock(g_queueMutex);
		if (g_accepting)
		{
			while (g_queuedBytes + line.size() > kMaxQueuedBytes && !g_queue.empty())
			{
				g_queuedBytes -= g_queue.front().size();
				g_queue.pop_front();
				++g_droppedEntries;
			}
			g_queuedBytes += line.size();
			g_queue.push_back(std::move(line));
			g_ready.notify_one();
			return;
		}
	}
	std::cerr << line;
}

} // namespace uam::diagnostics
