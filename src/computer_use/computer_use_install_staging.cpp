#include "computer_use/computer_use_install_staging.h"

#include <algorithm>
#include <fstream>
#include <string_view>
#include <system_error>

namespace uam::computer_use
{
	std::filesystem::path InstallStagingPath(const std::filesystem::path& root, long pid)
	{
		return root / (kInstallStagingPrefix + std::to_string(pid));
	}

	namespace
	{
		/// A real `.staging-<digits>` directory. symlink_status never follows a link; a link or file named like staging is not ours.
		bool IsStagingCopy(const std::filesystem::directory_entry& entry)
		{
			const std::string name = entry.path().filename().string();
			if (!name.starts_with(kInstallStagingPrefix)) return false;
			const std::string_view digits = std::string_view(name).substr(std::string_view(kInstallStagingPrefix).size());
			std::error_code error;
			return !digits.empty() && std::ranges::all_of(digits, [](char ch) { return ch >= '0' && ch <= '9'; }) &&
			    std::filesystem::is_directory(entry.symlink_status(error));
		}
	} // namespace

	bool HasInstallStaging(const std::filesystem::path& root)
	{
		std::error_code error;
		for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
			if (IsStagingCopy(*it)) return true;
		return false;
	}

	std::size_t SweepInstallStaging(const std::filesystem::path& root)
	{
		std::size_t removed = 0;
		std::error_code error;
		for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
		{
			if (!IsStagingCopy(*it)) continue;
			std::error_code remove_error;
			// remove_all deletes links inside the copy without following them.
			std::filesystem::remove_all(it->path(), remove_error);
			if (!remove_error) ++removed;
		}
		return removed;
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
