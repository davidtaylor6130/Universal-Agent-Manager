#pragma once

#include "common/platform/platform_services.h"
#include "common/platform/retained_directory_identity.h"
#include "common/paths/path_utils.h"
#include <array>
#include <mutex>
#if !defined(_WIN32)
#include <sys/file.h>
#endif

namespace uam::platform
{
	/// Production root ownership, also used by disposable fixtures. Marker contents are
	/// never rewritten: old PID diagnostics are not authority and foreign data must not be truncated.
	class AnchoredDataRootLock final : public DataRootLock
	{
	  public:
		static std::unique_ptr<DataRootLock> TryAcquire(const std::filesystem::path& root, std::string* error)
		{
			auto result = std::unique_ptr<AnchoredDataRootLock>(new AnchoredDataRootLock());
			std::error_code ec;
			const auto absolute = std::filesystem::absolute(root, ec);
			if (ec || !paths::CreateDirectoriesNoThrow(absolute, &ec) || !(result->m_named = RetainedDirectoryIdentity::Capture(absolute)))
				return Refuse(error, "Cannot retain the data root for exclusive ownership.");
			result->m_path = absolute;
#if defined(_WIN32)
			std::filesystem::path native;
			std::string failure;
			if (!io::NativeFilePath(absolute, native, failure, true))
				return Refuse(error, "Cannot address the data root safely.");
			result->m_directory = CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			if (!io::NativeFilePath(absolute / kMarker, native, failure, true))
				return Refuse(error, "Cannot address the data-root ownership marker safely.");
			result->m_file = CreateFileW(native.c_str(), GENERIC_READ, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
#else
			result->m_directory = open(absolute.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (result->m_directory >= 0)
				result->m_file = openat(result->m_directory, kMarker, O_RDONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
#endif
			if (!result->ReadIdentity(result->m_identity))
				return Refuse(error, "The data-root marker is linked, foreign or already owned.");
#if !defined(_WIN32)
			if (flock(result->m_file, LOCK_EX | LOCK_NB) != 0)
				return Refuse(error, "Another Universal Agent Manager instance is already using this data root.");
#endif
			if (!result->Current())
				return Refuse(error, "The data-root directory or ownership marker changed during acquisition.");
			return result;
		}
		~AnchoredDataRootLock() override
		{
#if defined(_WIN32)
			if (m_file != INVALID_HANDLE_VALUE) CloseHandle(m_file);
			if (m_directory != INVALID_HANDLE_VALUE) CloseHandle(m_directory);
#else
			if (m_file >= 0) close(m_file);
			if (m_directory >= 0) close(m_directory);
#endif
		}
		bool Current() const override
		{
			std::array<std::uint64_t, 2> identity{};
			if (!m_named || !m_named->Current() || !ReadIdentity(identity) || identity != m_identity)
				return false;
#if defined(_WIN32)
			BY_HANDLE_FILE_INFORMATION directory{};
			return GetFileInformationByHandle(m_directory, &directory) && (directory.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(directory.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
			struct stat directory{}, named_directory{}, marker{};
			return fstat(m_directory, &directory) == 0 && lstat(m_path.c_str(), &named_directory) == 0 && S_ISDIR(named_directory.st_mode) && directory.st_dev == named_directory.st_dev && directory.st_ino == named_directory.st_ino && fstatat(m_directory, kMarker, &marker, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(marker.st_mode) && marker.st_nlink == 1 && static_cast<std::uint64_t>(marker.st_dev) == m_identity[0] && static_cast<std::uint64_t>(marker.st_ino) == m_identity[1];
#endif
		}

	  private:
		static constexpr const char* kMarker = ".uam-data-root.lock";
		static std::unique_ptr<DataRootLock> Refuse(std::string* error, const char* reason)
		{
			if (error) *error = reason;
			return {};
		}
		bool ReadIdentity(std::array<std::uint64_t, 2>& identity) const
		{
			std::lock_guard lock(m_readMutex);
			std::array<char, 32> marker{};
			std::size_t size = 0;
#if defined(_WIN32)
			BY_HANDLE_FILE_INFORMATION info{};
			if (!GetFileInformationByHandle(m_file, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nNumberOfLinks != 1 || info.nFileSizeHigh || info.nFileSizeLow > 32)
				return false;
			identity = {info.dwVolumeSerialNumber, (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
			LARGE_INTEGER offset{};
			DWORD count = 0;
			if (!SetFilePointerEx(m_file, offset, nullptr, FILE_BEGIN) || !ReadFile(m_file, marker.data(), static_cast<DWORD>(marker.size()), &count, nullptr) || count != info.nFileSizeLow)
				return false;
			size = count;
#else
			struct stat info{};
			if (fstat(m_file, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1 || info.st_size < 0 || info.st_size > 32)
				return false;
			identity = {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino)};
			ssize_t count;
			do { count = pread(m_file, marker.data(), marker.size(), 0); } while (count < 0 && errno == EINTR);
			if (count < 0 || count != info.st_size)
				return false;
			size = static_cast<std::size_t>(count);
#endif
			const std::string_view text(marker.data(), size);
			return text.empty() || (text.size() > 1 && text.back() == '\n' && text.find_first_not_of("0123456789") == text.size() - 1);
		}
		AnchoredDataRootLock() = default;
		std::filesystem::path m_path;
		std::shared_ptr<const RetainedDirectoryIdentity> m_named;
		std::array<std::uint64_t, 2> m_identity{};
		mutable std::mutex m_readMutex;
#if defined(_WIN32)
		HANDLE m_file = INVALID_HANDLE_VALUE, m_directory = INVALID_HANDLE_VALUE;
#else
		int m_file = -1, m_directory = -1;
#endif
	};
} // namespace uam::platform
