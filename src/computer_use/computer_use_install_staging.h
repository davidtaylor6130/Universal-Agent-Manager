#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace uam::computer_use
{
	/// Prefix of the per-process copy built before it is swapped into `UAM Computer Use.app`.
	inline constexpr const char* kInstallStagingPrefix = ".staging-";

	/// Most staging copies tolerated in the install folder; beyond it no new copy is made.
	inline constexpr std::size_t kMaxInstallStagingCopies = 2;

	/// `<root>/.staging-<pid>`.
	std::filesystem::path InstallStagingPath(const std::filesystem::path& root, long pid);

	/// Whether a process with this id still exists.
	bool InstallProcessAlive(long pid);

	/// Removes `.staging-<pid>` directories in `root` whose process is gone (or is `self_pid`).
	/// Call only while holding the install lock. Symlinks and non-matching names are left alone and
	/// never followed. Returns how many staging directories remain afterwards.
	std::size_t SweepStaleInstallStaging(const std::filesystem::path& root, long self_pid);

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
