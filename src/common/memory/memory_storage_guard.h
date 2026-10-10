#pragma once

#include "common/utils/io_utils.h"
#include <mutex>
#include <optional>
#include <string>

namespace uam::memory
{
	inline std::mutex& StorageMutex()
	{
		static std::mutex mutex;
		return mutex;
	}

	/// <summary>Identifies saved memory bytes without reading their bodies, including replacement and restored modification times.</summary>
	inline std::optional<std::string> StorageRevision(const std::filesystem::path& path)
	{
#if defined(_WIN32)
		std::filesystem::path native;
		std::string error;
		if (!io::NativeFilePath(path, native, error)) return std::nullopt;
		HANDLE file = CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES,
		    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		    FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			const DWORD code = GetLastError();
			return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ?
			    std::optional<std::string>("missing") : std::nullopt;
		}
		BY_HANDLE_FILE_INFORMATION identity{};
		FILE_BASIC_INFO basic{};
		const bool ok = GetFileInformationByHandle(file, &identity) &&
		    GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic));
		CloseHandle(file);
		if (!ok || (identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return std::nullopt;
		return std::to_string(identity.dwVolumeSerialNumber) + ":" + std::to_string(identity.nFileIndexHigh) + ":" +
		    std::to_string(identity.nFileIndexLow) + ":" + std::to_string(identity.nFileSizeHigh) + ":" +
		    std::to_string(identity.nFileSizeLow) + ":" + std::to_string(basic.LastWriteTime.QuadPart) + ":" +
		    std::to_string(basic.ChangeTime.QuadPart);
#else
		struct stat status{};
		if (lstat(path.c_str(), &status) != 0)
			return errno == ENOENT ? std::optional<std::string>("missing") : std::nullopt;
		if (!S_ISREG(status.st_mode)) return std::nullopt;
#if defined(__APPLE__)
		const timespec modified = status.st_mtimespec;
		const timespec changed = status.st_ctimespec;
#else
		const timespec modified = status.st_mtim;
		const timespec changed = status.st_ctim;
#endif
		return std::to_string(status.st_dev) + ":" + std::to_string(status.st_ino) + ":" +
		    std::to_string(status.st_size) + ":" + std::to_string(modified.tv_sec) + ":" +
		    std::to_string(modified.tv_nsec) + ":" + std::to_string(changed.tv_sec) + ":" +
		    std::to_string(changed.tv_nsec);
#endif
	}
}
