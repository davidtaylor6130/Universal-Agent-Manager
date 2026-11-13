#include "computer_use/computer_use_install_staging.h"

#include <charconv>
#include <fstream>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#endif

namespace uam::computer_use
{
	std::filesystem::path InstallStagingPath(const std::filesystem::path& root, long pid)
	{
		return root / (kInstallStagingPrefix + std::to_string(pid));
	}

	bool InstallProcessAlive(long pid)
	{
		if (pid <= 0) return false;
#if defined(_WIN32)
		const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
		if (process == nullptr) return GetLastError() != ERROR_INVALID_PARAMETER;
		const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
		CloseHandle(process);
		return running;
#else
		return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
	}

	std::size_t SweepStaleInstallStaging(const std::filesystem::path& root, long self_pid)
	{
		std::size_t remaining = 0;
		std::error_code error;
		for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
		{
			const std::string name = it->path().filename().string();
			if (!name.starts_with(kInstallStagingPrefix)) continue;
			const std::string_view digits = std::string_view(name).substr(std::string_view(kInstallStagingPrefix).size());
			long pid = 0;
			const auto [last, parse_error] = std::from_chars(digits.data(), digits.data() + digits.size(), pid);
			std::error_code status_error;
			// symlink_status never follows a link; a link or foreign file named like staging is not ours.
			if (parse_error != std::errc{} || last != digits.data() + digits.size() || !std::filesystem::is_directory(it->symlink_status(status_error)))
				continue;
			if (pid != self_pid && InstallProcessAlive(pid))
			{
				++remaining;
				continue;
			}
			std::error_code remove_error;
			// remove_all deletes links inside the copy without following them.
			std::filesystem::remove_all(it->path(), remove_error);
			if (remove_error) ++remaining;
		}
		return remaining;
	}

	bool InstallReceiptMatches(const std::filesystem::path& root, const std::string& binary_hash, const std::string& plist_hash)
	{
		std::ifstream input(root / ".receipt");
		std::string receipt_binary, receipt_plist;
		if (input) input >> receipt_binary >> receipt_plist;
		std::error_code error;
		return !binary_hash.empty() && receipt_binary == binary_hash && receipt_plist == plist_hash &&
		    std::filesystem::is_directory(root / "UAM Computer Use.app", error);
	}

	InstallStagingGuard::~InstallStagingGuard()
	{
		std::error_code error;
		std::filesystem::remove_all(m_path, error);
	}
} // namespace uam::computer_use
