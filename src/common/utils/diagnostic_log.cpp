#include "diagnostic_log.h"

#include "common/utils/time_utils.h"

#include <fstream>
#include <iostream>
#include <mutex>

namespace uam::diagnostics
{
namespace
{
	std::mutex g_mutex;
	std::ofstream g_file;
	std::filesystem::path g_path;
	std::uintmax_t g_bytes = 0;

	bool Open(std::ios_base::openmode mode)
	{
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
		std::filesystem::remove(previous, error);
		if (error) return false;
		std::filesystem::rename(g_path, previous, error);
		if (error) return false;
		g_bytes = 0;
		return Open(std::ios::trunc);
	}
}

bool StartFileLog(const std::filesystem::path& path, std::string& error)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	error.clear();
	if (g_file.is_open())
	{
		error = "Diagnostic file logging is already active.";
		return false;
	}
	g_path = path;
	if (Open(std::ios::app))
	{
		g_file.seekp(0, std::ios::end);
		const std::streampos end = g_file.tellp();
		if (end >= 0 && static_cast<std::uintmax_t>(end) <= kMaxFileBytes)
		{
			g_bytes = static_cast<std::uintmax_t>(end);
			return true;
		}
	}
	g_file.close();
	error = "Diagnostic log could not be opened or exceeds its size limit; using stderr.";
	return false;
}

void StopFileLog()
{
	std::lock_guard<std::mutex> lock(g_mutex);
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
	// ponytail: flushes are synchronous; use a bounded queue if disk stalls affect the UI.
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file.is_open())
	{
		if (g_bytes + line.size() <= kMaxFileBytes || Rotate())
		{
			g_file.write(line.data(), static_cast<std::streamsize>(line.size()));
			g_file.flush();
			if (g_file)
			{
				g_bytes += line.size();
				return;
			}
		}
		// ponytail: failed file writes use stderr until restart; add backoff if retry is needed.
		g_file.close();
		std::cerr << "Diagnostic file logging failed; using stderr.\n";
	}
	std::cerr << line;
}

} // namespace uam::diagnostics
