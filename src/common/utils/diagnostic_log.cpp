#include "diagnostic_log.h"

#include "common/utils/time_utils.h"
#include "common/paths/path_utils.h"
#include "common/paths/workspace_lease.h"
#include "common/secrets/secret_redactor.h"
#include <atomic>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <memory>
#include <thread>
#include <iostream>
#include <iterator>
#include <mutex>
#include <utility>
#include <vector>

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
	// Storage audit channel. Path, pending lines and the compaction request are guarded by g_queueMutex;
	// the file itself belongs to the worker (and to StopFileLog after the worker joins).
	constexpr std::size_t kMaxAuditQueuedEntries = 4096;
	std::deque<std::string> g_auditQueue;
	std::size_t g_auditDropped = 0;
	std::filesystem::path g_auditPath;
	int g_auditRetentionDays = kAuditRetentionDays;
	bool g_auditCompactPending = false;
	std::ofstream g_auditFile;
	std::uintmax_t g_auditBytes = 0;
	// After a failed compaction the size trigger waits this long, so a locked file is not reread for every line.
	std::chrono::steady_clock::time_point g_auditCompactRetryAt{};

	bool OpenRegular(const std::filesystem::path& path, std::ofstream& file, std::ios_base::openmode mode)
	{
		// Special paths can redirect diagnostics or block desktop startup before the worker exists.
		std::error_code status_error;
		const std::filesystem::file_status status = std::filesystem::symlink_status(path, status_error);
		if ((status_error && status_error != std::errc::no_such_file_or_directory) ||
		    (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)) ||
		    uam::paths::IsLinkOrReparsePointNoThrow(path)) return false;
		file.clear();
		file.open(path, std::ios::binary | mode);
		if (!file.is_open()) return false;
		std::error_code error;
		std::filesystem::permissions(path,
		    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
		    std::filesystem::perm_options::replace, error);
		if (error) file.close();
		return !error;
	}

	bool Open(std::ios_base::openmode mode)
	{
		return OpenRegular(g_path, g_file, mode);
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

	std::string FormatLine(std::string_view message)
	{
		constexpr std::string_view truncation = " [truncated]";
		// Scrub before truncation and control-byte folding, either of which could split or alter a registered secret.
		std::string scrubbed; if (uam::secrets::Active()) message = scrubbed = uam::secrets::Scrub(message);
		std::string line = uam::time::IsoUtcTimestampNow() + " ";
		const std::size_t body_limit = kMaxEntryBytes - line.size() - truncation.size() - 1;
		line.append(message.substr(0, body_limit));
		for (char& ch : line)
		{
			if (static_cast<unsigned char>(ch) < 32 || ch == 127) ch = ' ';
		}
		if (message.size() > body_limit) line.append(truncation);
		line.push_back('\n');
		return line;
	}

	void AuditUnavailable(const char* what)
	{
		g_auditFile.close();
		std::cerr << "Storage audit log " << what << "; audit lines remain in the diagnostic log only.\n";
	}

	// A held or locked file (antivirus, an editor) must not stop audit lines: keep appending, retry compaction later.
	void ReopenAfterFailedCompaction(const std::filesystem::path& path)
	{
		std::error_code error;
		if (!OpenRegular(path, g_auditFile, std::ios::app)) { AuditUnavailable("could not be compacted or reopened"); return; }
		const std::uintmax_t size = std::filesystem::file_size(path, error);
		g_auditBytes = error ? 0 : size;
		g_auditCompactRetryAt = std::chrono::steady_clock::now() + std::chrono::hours(1);
		std::cerr << "Storage audit log could not be compacted; appending and retrying in an hour.\n";
	}

	// Rewrites the audit file without expired or excess lines, then reopens it for appending.
	void CompactAudit(const std::filesystem::path& path, int retention_days, std::size_t max_bytes)
	{
		g_auditFile.close();
		if (path.empty()) return;
		std::string text;
		bool truncated_read = false;
		{
			std::error_code error;
			const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
			if (!error && std::filesystem::is_regular_file(status) && !uam::paths::IsLinkOrReparsePointNoThrow(path))
			{
				std::ifstream input(path, std::ios::binary);
				input.seekg(0, std::ios::end);
				const std::streamoff size = input.tellg();
				// Read at most twice the cap: the oldest bytes of an overgrown file would be dropped anyway.
				const std::streamoff limit = static_cast<std::streamoff>(2 * kMaxAuditFileBytes);
				const std::streamoff start = size > limit ? size - limit : 0;
				input.seekg(start);
				text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
				truncated_read = start > 0;
				if (truncated_read)
				{
					const std::size_t newline = text.find('\n');
					text.erase(0, newline == std::string::npos ? text.size() : newline + 1);
				}
			}
			else if (error && error != std::errc::no_such_file_or_directory) { AuditUnavailable("could not be inspected"); return; }
			else if (!error && std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)) { AuditUnavailable("is not a regular file"); return; }
		}
		const std::string kept = CompactAuditText(text, std::chrono::system_clock::now(), retention_days, max_bytes);
		if (truncated_read || kept.size() != text.size())
		{
			std::filesystem::path temp = path;
			temp += ".tmp";
			std::ofstream output;
			if (!OpenRegular(temp, output, std::ios::trunc)) { ReopenAfterFailedCompaction(path); return; }
			output.write(kept.data(), static_cast<std::streamsize>(kept.size()));
			output.close();
			std::error_code error;
			if (output.fail()) error = std::make_error_code(std::errc::io_error);
			else std::filesystem::rename(temp, path, error);
			if (error)
			{
				std::filesystem::remove(temp, error);
				ReopenAfterFailedCompaction(path);
				return;
			}
		}
		if (!OpenRegular(path, g_auditFile, std::ios::app)) { AuditUnavailable("could not be opened"); return; }
		g_auditBytes = kept.size();
	}

	void WriteAuditLines(const std::deque<std::string>& lines, const std::filesystem::path& path, int retention_days)
	{
		for (const std::string& line : lines)
		{
			if (!g_auditFile.is_open()) return;
			// Compact to three quarters of the cap so a busy day does not rewrite the file for every line.
			if (g_auditBytes + line.size() > kMaxAuditFileBytes && std::chrono::steady_clock::now() >= g_auditCompactRetryAt)
				CompactAudit(path, retention_days, kMaxAuditFileBytes * 3 / 4);
			if (!g_auditFile.is_open()) return;
			g_auditFile.write(line.data(), static_cast<std::streamsize>(line.size()));
			if (!g_auditFile) { AuditUnavailable("write failed"); return; }
			g_auditBytes += line.size();
		}
		g_auditFile.flush();
		if (!g_auditFile) AuditUnavailable("flush failed");
	}

	void Drain(std::stop_token stop)
	{
		ExpirePreviousLog();
		std::chrono::steady_clock::time_point next_expiry_check = std::chrono::steady_clock::now() + std::chrono::minutes(5);
		std::chrono::steady_clock::time_point next_audit_compaction = std::chrono::steady_clock::time_point::max();
		for (;;)
		{
			std::deque<std::string> batch;
			std::deque<std::string> audit_batch;
			std::filesystem::path audit_path;
			int audit_retention_days = kAuditRetentionDays;
			bool compact_audit = false;
			std::size_t dropped = 0;
			{
				std::unique_lock<std::mutex> lock(g_queueMutex);
				g_ready.wait_until(lock, stop, next_expiry_check, []() { return !g_queue.empty() || !g_auditQueue.empty() || g_droppedEntries != 0 || g_auditCompactPending; });
				if (g_queue.empty() && g_auditQueue.empty() && g_droppedEntries == 0 && !g_auditCompactPending && stop.stop_requested()) break;
				batch.swap(g_queue);
				audit_batch.swap(g_auditQueue);
				audit_path = g_auditPath;
				audit_retention_days = g_auditRetentionDays;
				compact_audit = std::exchange(g_auditCompactPending, false);
				if (const std::size_t audit_dropped = std::exchange(g_auditDropped, 0); audit_dropped != 0)
					audit_batch.push_back(FormatLine("Audit log dropped " + std::to_string(audit_dropped) + " entries because its queue was full; they were also queued to uam.log."));
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
			// Age expiry runs when the audit log opens and then daily, so a long-running app still forgets old lines.
			if (!audit_path.empty() && (compact_audit || std::chrono::steady_clock::now() >= next_audit_compaction))
			{
				CompactAudit(audit_path, audit_retention_days, kMaxAuditFileBytes);
				next_audit_compaction = std::chrono::steady_clock::now() + std::chrono::hours(24);
			}
			if (!audit_batch.empty() && !audit_path.empty()) WriteAuditLines(audit_batch, audit_path, audit_retention_days);
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
	g_drained.wait(lock, []() { return g_queue.empty() && g_auditQueue.empty() && !g_writing && g_droppedEntries == 0 && !g_auditCompactPending; });
}

void StopFileLog()
{
	std::lock_guard<std::mutex> lock(g_lifecycleMutex);
	{
		std::lock_guard<std::mutex> queue_lock(g_queueMutex);
		// The closing marker lets a reader tell an uninterrupted audit history from a crash or kill.
		if (g_accepting && !g_auditPath.empty()) g_auditQueue.push_back(FormatLine("Audit log closed"));
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
	g_auditFile.close();
	g_auditFile.clear();
	g_auditBytes = 0;
	g_auditCompactRetryAt = {};
	std::lock_guard<std::mutex> queue_lock(g_queueMutex);
	g_auditPath.clear();
	g_auditQueue.clear();
	g_auditCompactPending = false;
}

void Write(std::string_view message)
{
	std::string line = FormatLine(message);
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

bool StartAuditLog(const std::filesystem::path& path, std::string& error, int retention_days)
{
	std::lock_guard<std::mutex> lock(g_lifecycleMutex);
	error.clear();
	if (retention_days < 1 || retention_days > 3650) { error = "Storage audit retention is invalid."; return false; }
	if (!g_worker) { error = "Storage audit log needs an active diagnostic file log."; return false; }
	std::error_code status_error;
	const std::filesystem::file_status status = std::filesystem::symlink_status(path, status_error);
	if (path.filename().empty() || (status_error && status_error != std::errc::no_such_file_or_directory) ||
	    (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)) || uam::paths::IsLinkOrReparsePointNoThrow(path))
	{
		error = "Storage audit log path is not a regular file; audit lines remain in the diagnostic log only.";
		return false;
	}
	{
		std::lock_guard<std::mutex> queue_lock(g_queueMutex);
		if (!g_auditPath.empty()) { error = "Storage audit logging is already active."; return false; }
		g_auditPath = path;
		g_auditRetentionDays = retention_days;
		g_auditCompactPending = true;
		g_auditQueue.push_back(FormatLine("Audit log opened: retention_days=" + std::to_string(retention_days)));
		g_ready.notify_one();
	}
	return true;
}

void WriteAudit(std::string_view message)
{
	std::string line = FormatLine(message);
	{
		std::lock_guard<std::mutex> lock(g_queueMutex);
		if (g_accepting && !g_auditPath.empty())
		{
			// Bounded only against a stalled worker; ordinary audit volume is a few lines per user action.
			if (g_auditQueue.size() < kMaxAuditQueuedEntries) g_auditQueue.push_back(line);
			else ++g_auditDropped; // Still written to uam.log below; only the audit copy is lost.
		}
	}
	Write(message);
}

std::string CompactAuditText(std::string_view text, std::chrono::system_clock::time_point now, int retention_days, std::size_t max_bytes)
{
	// Lines start with the fixed-width ISO UTC stamp, so string order is time order.
	const std::time_t cutoff_time = std::chrono::system_clock::to_time_t(now - std::chrono::hours(24) * retention_days);
	const std::string cutoff = uam::time::detail::FormatTimestamp(uam::time::detail::UtcTimeSnapshot(cutoff_time), uam::time::kIsoUtcTimestampFormat);
	std::vector<std::string_view> kept;
	std::size_t kept_bytes = 0;
	std::size_t begin = 0;
	while (begin < text.size())
	{
		std::size_t end = text.find('\n', begin);
		end = end == std::string_view::npos ? text.size() : end + 1;
		const std::string_view line = text.substr(begin, end - begin);
		begin = end;
		const bool stamped = line.size() > cutoff.size() && line[cutoff.size()] == ' ' && line[4] == '-' && line[10] == 'T';
		// An unstamped fragment (a torn final write) has no age to keep it by.
		if (!stamped || line.substr(0, cutoff.size()) < cutoff || line.back() != '\n') continue;
		kept.push_back(line);
		kept_bytes += line.size();
	}
	std::size_t first = 0;
	while (first < kept.size() && kept_bytes > max_bytes) kept_bytes -= kept[first++].size();
	std::string result;
	result.reserve(kept_bytes);
	for (std::size_t index = first; index < kept.size(); ++index) result.append(kept[index]);
	return result;
}

} // namespace uam::diagnostics
