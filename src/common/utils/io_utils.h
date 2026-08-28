#pragma once

#include "common/paths/path_utils.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/stdio.h>
#include <unistd.h>
#else
#error "io_utils.h is only supported on Windows and macOS."
#endif

namespace uam::io
{
	inline constexpr const char* kTempWritePathSuffix = ".tmp.";
	inline constexpr const char* kBackupPathSuffix = ".bak";

	inline bool TryReadFile(const std::filesystem::path& path, std::string& out)
	{
		out.clear();
		std::ifstream file(path, std::ios::in | std::ios::binary);
		if (!file)
		{
			return false;
		}

		std::ostringstream ss;
		ss << file.rdbuf();
		if (file.bad())
		{
			out.clear();
			return false;
		}
		out = ss.str();
		return true;
	}

	inline bool TryReadTextFile(const std::filesystem::path& path, std::string& out)
	{
		return TryReadFile(path, out);
	}

	inline bool TryReadTextFile(
	    const std::filesystem::path& path, std::string& out, std::size_t max_bytes)
	{
		out.clear();
		std::ifstream file(path, std::ios::in | std::ios::binary);
		if (!file) return false;

		std::array<char, 8192> buffer{};
		while (file)
		{
			file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const std::size_t count = static_cast<std::size_t>(file.gcount());
			if (out.size() > max_bytes || count > max_bytes - out.size())
			{
				out.clear();
				return false;
			}
			out.append(buffer.data(), count);
		}
		if (file.bad())
		{
			out.clear();
			return false;
		}
		return true;
	}

	inline std::string ReadTextFile(const std::filesystem::path& path)
	{
		std::string text;
		TryReadTextFile(path, text);
		return text;
	}

	template <typename LineVisitor> inline bool ForEachTextFileLine(const std::filesystem::path& path, LineVisitor visitor)
	{
		std::ifstream file(path, std::ios::in | std::ios::binary);
		if (!file)
		{
			return false;
		}

		std::string line;
		while (std::getline(file, line))
		{
			if (!visitor(line))
			{
				break;
			}
		}
		return !file.bad();
	}

	inline std::optional<std::string> ReadFirstTextFileLine(const std::filesystem::path& path)
	{
		std::optional<std::string> first_line;
		ForEachTextFileLine(path,
		                    [&first_line](const std::string& line)
		                    {
			                    first_line = line;
			                    return false;
		                    });
		return first_line;
	}

	template <typename ByteVisitor> inline bool ForEachBinaryFileByte(const std::filesystem::path& path, ByteVisitor visitor)
	{
		std::ifstream file(path, std::ios::in | std::ios::binary);
		if (!file)
		{
			return false;
		}

		char ch = '\0';
		while (file.get(ch))
		{
			if (!visitor(ch))
			{
				break;
			}
		}
		return !file.bad();
	}

	inline bool TryReadBinaryFile(const std::filesystem::path& path, std::string& out)
	{
		return TryReadFile(path, out);
	}

	inline std::filesystem::path MakeTempWritePath(const std::filesystem::path& path)
	{
		const std::uint64_t monotonic_token = static_cast<std::uint64_t>(uam::time::SteadyEpochNanosecondsNow());
		const std::uint64_t random_token = static_cast<std::uint64_t>(std::random_device{}());
		const std::uint64_t token = monotonic_token ^ random_token;
		std::filesystem::path temp_path = path;
		temp_path += kTempWritePathSuffix;
		temp_path += std::to_string(token);
		return temp_path;
	}

	inline std::filesystem::path MakeBackupPath(const std::filesystem::path& path)
	{
		std::filesystem::path backup_path = path;
		backup_path += kBackupPathSuffix;
		return backup_path;
	}

	enum class AtomicWriteStage
	{
		None,
		AfterCreate,
		DuringWrite,
		AfterFlush,
		AfterFileSync,
		BeforeReplace,
		AfterReplaceBeforeDirectorySync,
		BeforeBackupCleanup,
		Complete,
	};

	struct AtomicWriteResult
	{
		bool success = false;
		bool primary_committed = false;
		bool backup_degraded = false;
		bool interrupted = false;
		AtomicWriteStage stage = AtomicWriteStage::None;
		std::string error;
	};

	using AtomicWriteStageHook = std::function<bool(AtomicWriteStage)>;

	inline bool ContinueAtomicWrite(const AtomicWriteStageHook& hook, AtomicWriteStage stage, AtomicWriteResult& result)
	{
		result.stage = stage;
		if (hook && !hook(stage))
		{
			result.interrupted = true;
			result.error = "Atomic write interrupted for fault injection.";
			return false;
		}
		return true;
	}

	inline void RemoveAtomicTempNoThrow(const std::filesystem::path& path)
	{
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
		if (!error && status.type() != std::filesystem::file_type::not_found && !std::filesystem::is_symlink(status))
		{
			std::filesystem::remove(path, error);
		}
	}

	inline void CleanupAbandonedAtomicTemps(const std::filesystem::path& path)
	{
		if (!uam::paths::PathExistsNoThrow(path) || path.parent_path().empty())
		{
			return;
		}

		const std::string prefix = uam::paths::Utf8PathString(path.filename()) + kTempWritePathSuffix;
		std::error_code error;
		for (std::filesystem::directory_iterator it(path.parent_path(), std::filesystem::directory_options::skip_permission_denied, error), end;
		     !error && it != end;
		     it.increment(error))
		{
			const std::filesystem::directory_entry& entry = *it;
			const std::string name = uam::paths::Utf8PathString(entry.path().filename());
			std::error_code status_error;
			const std::filesystem::file_status status = entry.symlink_status(status_error);
			if (status_error || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status) || !name.starts_with(prefix))
			{
				continue;
			}

			std::error_code time_error;
			const auto modified = entry.last_write_time(time_error);
			if (!time_error && std::filesystem::file_time_type::clock::now() - modified >= std::chrono::hours(24))
			{
				std::filesystem::remove(entry.path(), status_error);
			}
		}
	}

#if defined(_WIN32)
	inline std::string AtomicWriteSystemError(unsigned long code)
	{
		return std::error_code(static_cast<int>(code), std::system_category()).message();
	}

	inline bool WriteAndSyncAtomicTemp(
	    const std::filesystem::path& temp_path,
	    std::string_view content,
	    const AtomicWriteStageHook& hook,
	    AtomicWriteResult& result)
	{
		HANDLE file = CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			result.error = "Failed to create temporary file: " + AtomicWriteSystemError(GetLastError());
			return false;
		}
		if (!ContinueAtomicWrite(hook, AtomicWriteStage::AfterCreate, result))
		{
			CloseHandle(file);
			return false;
		}

		std::size_t written_total = 0;
		const std::size_t fault_boundary = content.size() / 2;
		bool during_write_reached = false;
		while (written_total < content.size())
		{
			if (!during_write_reached && written_total >= fault_boundary)
			{
				during_write_reached = true;
				if (!ContinueAtomicWrite(hook, AtomicWriteStage::DuringWrite, result))
				{
					CloseHandle(file);
					return false;
				}
			}
			const std::size_t until_fault = !during_write_reached && fault_boundary > written_total ? fault_boundary - written_total : content.size() - written_total;
			const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(until_fault, 1024U * 1024U));
			DWORD written = 0;
			if (!WriteFile(file, content.data() + written_total, chunk, &written, nullptr) || written == 0)
			{
				result.error = "Failed to write temporary file: " + AtomicWriteSystemError(GetLastError());
				CloseHandle(file);
				return false;
			}
			written_total += written;
		}
		if (!during_write_reached && !ContinueAtomicWrite(hook, AtomicWriteStage::DuringWrite, result))
		{
			CloseHandle(file);
			return false;
		}
		if (!ContinueAtomicWrite(hook, AtomicWriteStage::AfterFlush, result))
		{
			CloseHandle(file);
			return false;
		}
		if (!FlushFileBuffers(file))
		{
			result.error = "Failed to sync temporary file: " + AtomicWriteSystemError(GetLastError());
			CloseHandle(file);
			return false;
		}
		CloseHandle(file);
		return ContinueAtomicWrite(hook, AtomicWriteStage::AfterFileSync, result);
	}

	inline bool ReplaceAtomicTemp(
	    const std::filesystem::path& path,
	    const std::filesystem::path& temp_path,
	    const std::filesystem::path& backup_temp_path,
	    bool destination_exists,
	    bool preserve_backup,
	    AtomicWriteResult& result)
	{
		const bool replaced = destination_exists
		    ? ReplaceFileW(path.c_str(), temp_path.c_str(), preserve_backup ? backup_temp_path.c_str() : nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)
		    : MoveFileExW(temp_path.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH);
		if (!replaced)
		{
			result.error = "Failed to atomically replace destination: " + AtomicWriteSystemError(GetLastError());
			return false;
		}
		return true;
	}
#else
	inline std::string AtomicWriteSystemError(int code)
	{
		return std::error_code(code, std::generic_category()).message();
	}

	inline bool WriteAndSyncAtomicTemp(
	    const std::filesystem::path& temp_path,
	    std::string_view content,
	    const AtomicWriteStageHook& hook,
	    AtomicWriteResult& result)
	{
		const int fd = open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
		if (fd < 0)
		{
			result.error = "Failed to create temporary file: " + AtomicWriteSystemError(errno);
			return false;
		}
		if (!ContinueAtomicWrite(hook, AtomicWriteStage::AfterCreate, result))
		{
			close(fd);
			return false;
		}

		std::size_t written_total = 0;
		const std::size_t fault_boundary = content.size() / 2;
		bool during_write_reached = false;
		while (written_total < content.size())
		{
			if (!during_write_reached && written_total >= fault_boundary)
			{
				during_write_reached = true;
				if (!ContinueAtomicWrite(hook, AtomicWriteStage::DuringWrite, result))
				{
					close(fd);
					return false;
				}
			}
			const std::size_t until_fault = !during_write_reached && fault_boundary > written_total ? fault_boundary - written_total : content.size() - written_total;
			const ssize_t written = write(fd, content.data() + written_total, std::min<std::size_t>(until_fault, 1024U * 1024U));
			if (written > 0)
			{
				written_total += static_cast<std::size_t>(written);
				continue;
			}
			if (written < 0 && errno == EINTR)
			{
				continue;
			}
			result.error = "Failed to write temporary file: " + AtomicWriteSystemError(errno);
			close(fd);
			return false;
		}
		if (!during_write_reached && !ContinueAtomicWrite(hook, AtomicWriteStage::DuringWrite, result))
		{
			close(fd);
			return false;
		}
		if (!ContinueAtomicWrite(hook, AtomicWriteStage::AfterFlush, result))
		{
			close(fd);
			return false;
		}
		if (fcntl(fd, F_FULLFSYNC) != 0 && fsync(fd) != 0)
		{
			result.error = "Failed to sync temporary file: " + AtomicWriteSystemError(errno);
			close(fd);
			return false;
		}
		if (close(fd) != 0)
		{
			result.error = "Failed to close temporary file: " + AtomicWriteSystemError(errno);
			return false;
		}
		return ContinueAtomicWrite(hook, AtomicWriteStage::AfterFileSync, result);
	}

	inline bool SyncAtomicWriteDirectory(const std::filesystem::path& parent, AtomicWriteResult& result)
	{
		const int fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0)
		{
			result.error = "Failed to open destination directory for sync: " + AtomicWriteSystemError(errno);
			return false;
		}
		const bool synced = fsync(fd) == 0;
		const int sync_error = errno;
		close(fd);
		if (!synced)
		{
			result.error = "Failed to sync destination directory: " + AtomicWriteSystemError(sync_error);
		}
		return synced;
	}
#endif

	inline AtomicWriteResult AtomicWriteFileDetailed(
	    const std::filesystem::path& path,
	    std::string_view content,
	    const AtomicWriteStageHook& hook = {},
	    bool preserve_backup = false)
	{
		static std::mutex write_mutex;
		std::lock_guard<std::mutex> lock(write_mutex);
		AtomicWriteResult result;
		if (path.empty() || path.filename().empty())
		{
			result.error = "Destination path is empty.";
			return result;
		}

		const std::filesystem::path parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
		std::error_code ec;
		if (!uam::paths::CreateDirectoriesNoThrow(parent, &ec))
		{
			result.error = "Failed to create destination directory: " + ec.message();
			return result;
		}
		const std::filesystem::file_status destination_status = std::filesystem::symlink_status(path, ec);
		if (ec && ec != std::errc::no_such_file_or_directory)
		{
			result.error = "Failed to inspect destination: " + ec.message();
			return result;
		}
		const bool destination_exists = !ec && destination_status.type() != std::filesystem::file_type::not_found;
		if (destination_exists && (std::filesystem::is_symlink(destination_status) || !std::filesystem::is_regular_file(destination_status)))
		{
			result.error = "Destination must be a regular file, not a link or directory.";
			return result;
		}

		CleanupAbandonedAtomicTemps(path);
		CleanupAbandonedAtomicTemps(MakeBackupPath(path));

		const std::filesystem::path temp_path = MakeTempWritePath(path);
		const std::filesystem::path backup_path = MakeBackupPath(path);
		const std::filesystem::path backup_temp_path = MakeTempWritePath(backup_path);
		if (!WriteAndSyncAtomicTemp(temp_path, content, hook, result))
		{
			if (!result.interrupted)
			{
				RemoveAtomicTempNoThrow(temp_path);
			}
			return result;
		}
		if (!ContinueAtomicWrite(hook, AtomicWriteStage::BeforeReplace, result))
		{
			return result;
		}

#if defined(_WIN32)
		if (!ReplaceAtomicTemp(path, temp_path, backup_temp_path, destination_exists, preserve_backup, result))
		{
			RemoveAtomicTempNoThrow(temp_path);
			return result;
		}
		result.primary_committed = true;
#else
		if (destination_exists)
		{
			if (renameatx_np(AT_FDCWD, temp_path.c_str(), AT_FDCWD, path.c_str(), RENAME_SWAP) != 0)
			{
				result.error = "Failed to atomically swap destination: " + AtomicWriteSystemError(errno);
				RemoveAtomicTempNoThrow(temp_path);
				return result;
			}
		}
		else if (rename(temp_path.c_str(), path.c_str()) != 0)
		{
			result.error = "Failed to atomically install destination: " + AtomicWriteSystemError(errno);
			RemoveAtomicTempNoThrow(temp_path);
			return result;
		}
#endif
		if (!ContinueAtomicWrite(hook, AtomicWriteStage::AfterReplaceBeforeDirectorySync, result))
		{
			return result;
		}
#if defined(__APPLE__)
		if (!SyncAtomicWriteDirectory(parent, result))
		{
			return result;
		}
		result.primary_committed = true;
#endif
		if (!ContinueAtomicWrite(hook, AtomicWriteStage::BeforeBackupCleanup, result))
		{
			if (result.primary_committed)
			{
				result.success = true;
				result.backup_degraded = preserve_backup && destination_exists;
			}
			return result;
		}

		if (destination_exists)
		{
			if (!preserve_backup)
			{
#if defined(__APPLE__)
				RemoveAtomicTempNoThrow(temp_path);
#endif
				result.success = true;
				result.primary_committed = true;
				result.stage = AtomicWriteStage::Complete;
				return result;
			}
#if defined(_WIN32)
			if (!MoveFileExW(backup_temp_path.c_str(), backup_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				result.error = "Destination was saved, but its recovery backup could not be installed: " + AtomicWriteSystemError(GetLastError());
				result.success = true;
				result.backup_degraded = true;
				return result;
			}
#else
			if (rename(temp_path.c_str(), backup_path.c_str()) != 0)
			{
				result.error = "Destination was saved, but its recovery backup could not be installed: " + AtomicWriteSystemError(errno);
				result.success = true;
				result.backup_degraded = true;
				return result;
			}
			if (!SyncAtomicWriteDirectory(parent, result))
			{
				result.success = true;
				result.backup_degraded = true;
				return result;
			}
#endif
		}

		result.success = true;
		result.primary_committed = true;
		result.stage = AtomicWriteStage::Complete;
		return result;
	}

	inline bool AtomicWriteFile(const std::filesystem::path& path, std::string_view content)
	{
		return AtomicWriteFileDetailed(path, content).success;
	}

	inline bool AtomicWriteFileWithBackup(const std::filesystem::path& path, std::string_view content)
	{
		return AtomicWriteFileDetailed(path, content, {}, true).success;
	}

	inline bool WriteTextFile(const std::filesystem::path& path, std::string_view content)
	{
		return AtomicWriteFile(path, content);
	}

	inline bool WriteBinaryFile(const std::filesystem::path& path, std::string_view content)
	{
		return AtomicWriteFile(path, content);
	}

	inline bool WriteTextFileWithBackup(const std::filesystem::path& path, std::string_view content)
	{
		return AtomicWriteFileWithBackup(path, content);
	}

	inline bool WriteBinaryFileWithBackup(const std::filesystem::path& path, std::string_view content)
	{
		return AtomicWriteFileWithBackup(path, content);
	}
} // namespace uam::io
