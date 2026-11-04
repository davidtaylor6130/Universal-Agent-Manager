#pragma once

#include "common/platform/retained_directory_identity.h"
#include "common/utils/io_utils.h"
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#if !defined(_WIN32)
#include <sys/file.h>
#endif

namespace uam::platform
{
	/// Independent MCP writers share this lease; stopped migration owns it exclusively.
	/// The reserved empty marker is not profile data, PID evidence or execution authority.
	class DataFolderWriterLease final
	{
	  public:
		enum class Access
		{
			Shared,
			Exclusive
		};
		static constexpr const char* kFileName = ".uam-data-writers.lock";
		static std::shared_ptr<DataFolderWriterLease> TryAcquire(const std::filesystem::path& root, Access access, std::string& error)
		{
			auto result = std::shared_ptr<DataFolderWriterLease>(new DataFolderWriterLease());
			result->m_root = RetainedDirectoryIdentity::Capture(root);
			result->m_path = root / kFileName;
			if (!result->m_root)
			{
				error = "The profile directory cannot be owned for writer coordination.";
				return {};
			}
#if defined(_WIN32)
			std::filesystem::path native;
			if (!io::NativeFilePath(result->m_path, native, error, true))
				return {};
			result->m_handle = CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			if (result->m_handle == INVALID_HANDLE_VALUE || !ReadIdentity(result->m_handle, result->m_identity))
			{
				error = "The profile writer marker is unavailable, linked or foreign.";
				return {};
			}
			OVERLAPPED offset{};
			if (!LockFileEx(result->m_handle, LOCKFILE_FAIL_IMMEDIATELY | (access == Access::Exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0), 0, 1, 0, &offset))
			{
				error = "A profile writer or stopped transfer still owns this folder.";
				return {};
			}
#else
			result->m_handle = open(result->m_path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
			if (result->m_handle < 0 || !ReadIdentity(result->m_handle, result->m_identity))
			{
				error = "The profile writer marker is unavailable, linked or foreign.";
				return {};
			}
			if (flock(result->m_handle, (access == Access::Exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0)
			{
				error = "A profile writer or stopped transfer still owns this folder.";
				return {};
			}
#endif
			if (!result->Current())
			{
				error = "Profile writer ownership changed during acquisition.";
				return {};
			}
			return result;
		}
		~DataFolderWriterLease()
		{
#if defined(_WIN32)
			if (m_handle != INVALID_HANDLE_VALUE)
				CloseHandle(m_handle);
#else
			if (m_handle >= 0)
				close(m_handle);
#endif
		}
		bool Current() const
		{
			std::array<std::uint64_t, 3> observed{};
			if (!m_root || !m_root->Current() || !ReadIdentity(m_handle, observed) || observed != m_identity)
				return false;
#if !defined(_WIN32)
			struct stat named{};
			return lstat(m_path.c_str(), &named) == 0 && S_ISREG(named.st_mode) && named.st_nlink == 1 && named.st_size == 0 && static_cast<std::uint64_t>(named.st_dev) == m_identity[0] && static_cast<std::uint64_t>(named.st_ino) == m_identity[1];
#else
			// The retained file handle denies delete sharing; its name cannot be replaced.
			return true;
#endif
		}

	  private:
#if defined(_WIN32)
		using Handle = HANDLE;
		Handle m_handle = INVALID_HANDLE_VALUE;
		static bool ReadIdentity(Handle handle, std::array<std::uint64_t, 3>& identity)
		{
			BY_HANDLE_FILE_INFORMATION info{};
			if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) || info.nNumberOfLinks != 1 || info.nFileSizeHigh || info.nFileSizeLow)
				return false;
			identity = {info.dwVolumeSerialNumber, (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow, 0};
			return true;
		}
#else
		using Handle = int;
		Handle m_handle = -1;
		static bool ReadIdentity(Handle handle, std::array<std::uint64_t, 3>& identity)
		{
			struct stat info{};
			if (fstat(handle, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size != 0)
				return false;
			identity = {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino), 0};
			return true;
		}
#endif
		DataFolderWriterLease() = default;
		std::filesystem::path m_path;
		std::shared_ptr<const RetainedDirectoryIdentity> m_root;
		std::array<std::uint64_t, 3> m_identity{};
	};
} // namespace uam::platform
