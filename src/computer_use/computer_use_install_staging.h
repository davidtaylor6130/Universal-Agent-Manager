#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace uam::computer_use
{
	/// Prefix of the per-process copy built before it is swapped into `UAM Computer Use.app`.
	inline constexpr const char* kInstallStagingPrefix = ".staging-";

	/// `<root>/.staging-<pid>`.
	std::filesystem::path InstallStagingPath(const std::filesystem::path& root, long pid);

	/// Whether `root` holds any copy `SweepInstallStaging` would remove, without touching it.
	bool HasInstallStaging(const std::filesystem::path& root);

	/// Removes every `.staging-<pid>` directory in `root`. Call only while holding the install lock:
	/// copies are made only under that lock (by every UAM version), so any copy seen there was abandoned.
	/// Symlinks, files and non-matching names are left alone and never followed. Returns how many were removed.
	std::size_t SweepInstallStaging(const std::filesystem::path& root);

	/// Whether `.receipt` in `root` names these hashes and the installed app exists, so nothing needs copying.
	bool InstallReceiptMatches(const std::filesystem::path& root, const std::string& binary_hash, const std::string& plist_hash);

	/// Removes the staging directory on every exit path. After a successful swap the path no longer exists.
	class InstallStagingGuard
	{
	  public:
		explicit InstallStagingGuard(std::filesystem::path path) : m_path(std::move(path)) {}
		~InstallStagingGuard();
		InstallStagingGuard(const InstallStagingGuard&) = delete;
		InstallStagingGuard& operator=(const InstallStagingGuard&) = delete;

	  private:
		std::filesystem::path m_path;
	};
} // namespace uam::computer_use
