#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include "common/utils/io_utils.h"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace uam::platform
{
	/// Attribute-only identity of a retained directory. Does not grant anchored IO or lock ownership.
	/// Directory entry/mtime changes are allowed; path replacement, leaf links and reparse points are not.
	class RetainedDirectoryIdentity final
	{
	  public:
#if defined(_WIN32)
		using DirectoryHandle = HANDLE;
#else
		using DirectoryHandle = int;
#endif
		RetainedDirectoryIdentity(const RetainedDirectoryIdentity&) = delete;
		RetainedDirectoryIdentity& operator=(const RetainedDirectoryIdentity&) = delete;
		~RetainedDirectoryIdentity()
		{
			Close(m_handle);
		}

		static std::shared_ptr<const RetainedDirectoryIdentity> Capture(const std::filesystem::path& path)
		{
			const Handle opened = Open(path);
			if (opened == Invalid())
			{
				return {};
			}
			std::unique_ptr<RetainedDirectoryIdentity> result;
			try
			{
				result.reset(new RetainedDirectoryIdentity(path, opened));
			}
			catch (...)
			{
				Close(opened);
				throw;
			}
			const auto identity = Read(opened);
			if (!identity)
			{
				return {};
			}
			result->m_identity = *identity;
			if (!result->Current())
			{
				return {};
			}
			return std::shared_ptr<const RetainedDirectoryIdentity>(std::move(result));
		}

		bool Current() const
		{
			const auto retained = Read(m_handle);
			if (!retained || *retained != m_identity)
			{
				return false;
			}
			const Handle named = Open(m_path);
			if (named == Invalid())
			{
				return false;
			}
			const auto current = Read(named);
			Close(named);
			const auto after = Read(m_handle);
			return current && *current == m_identity && after && *after == m_identity;
		}
		/// Bind a separately opened IO handle to this exact retained directory, not a later name lookup.
		bool MatchesDirectory(DirectoryHandle handle) const
		{
			const auto identity = Read(handle);
			return identity && *identity == m_identity;
		}

	  private:
		using Identity = std::array<std::uint64_t, 3>;
#if defined(_WIN32)
		using Handle = DirectoryHandle;
		static Handle Invalid()
		{
			return INVALID_HANDLE_VALUE;
		}
		static Handle Open(const std::filesystem::path& path)
		{
			std::filesystem::path native;
			std::string error;
			if (!io::NativeFilePath(path, native, error, true))
				return Invalid();
			return CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		}
		static void Close(Handle handle)
		{
			if (handle != Invalid())
			{
				CloseHandle(handle);
			}
		}
		static std::optional<Identity> Read(Handle handle)
		{
			BY_HANDLE_FILE_INFORMATION info{};
			if (!GetFileInformationByHandle(handle, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
			{
				return {};
			}
			return Identity{info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
		}
#else
		using Handle = DirectoryHandle;
		static Handle Invalid()
		{
			return -1;
		}
		static Handle Open(const std::filesystem::path& path)
		{
			return open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		}
		static void Close(Handle handle)
		{
			if (handle >= 0)
			{
				close(handle);
			}
		}
		static std::optional<Identity> Read(Handle handle)
		{
			struct stat info{};
			if (fstat(handle, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_nlink == 0)
			{
				return {};
			}
			return Identity{static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino), 0};
		}
#endif
		RetainedDirectoryIdentity(std::filesystem::path path, Handle handle) : m_path(std::move(path)), m_handle(handle)
		{
		}
		std::filesystem::path m_path;
		Handle m_handle;
		Identity m_identity{};
	};
} // namespace uam::platform
