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
#elif defined(__APPLE__) || defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <sys/stdio.h>
#else
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#include <unistd.h>
#else
#error "io_utils.h is only supported on Windows, macOS and Linux."
#endif

namespace uam::io
{
	inline constexpr const char* kTempWritePathSuffix = ".tmp.";
	inline constexpr const char* kBackupPathSuffix = ".bak";

#if defined(_WIN32)
	inline std::string AtomicWriteSystemError(unsigned long code)
	{
		return std::error_code(static_cast<int>(code), std::system_category()).message();
	}

	/// <summary>Resolve Win32 paths before adding an extended prefix; optionally protect short directory roots whose descendants can be long.</summary>
	inline bool NativeFilePath(const std::filesystem::path& path, std::filesystem::path& native_path, std::string& error, bool force_extended = false)
	{
		const std::wstring& input = path.native();
		if (input.empty() || input.find(L'\0') != std::wstring::npos)
		{
			error = "Invalid file path.";
			return false;
		}
		if (input.starts_with(L"\\\\?\\") || input.starts_with(L"\\\\.\\"))
		{
			native_path = path;
			return true;
		}
		const DWORD required = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
		if (required == 0)
		{
			error = "Failed to resolve file path: " + AtomicWriteSystemError(GetLastError());
			return false;
		}
		std::wstring absolute(required, L'\0');
		const DWORD length = GetFullPathNameW(input.c_str(), required, absolute.data(), nullptr);
		if (length == 0 || length >= required)
		{
			const DWORD code = length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
			error = "Failed to resolve file path: " + AtomicWriteSystemError(code);
			return false;
		}
		absolute.resize(length);
		if (!force_extended && length < MAX_PATH)
		{
			native_path = path;
			return true;
		}
		// GetFullPathNameW resolves relative components and separators before extended paths disable Win32 normalization.
		if (absolute.starts_with(L"\\\\"))
		{
			native_path = std::filesystem::path(L"\\\\?\\UNC\\" + absolute.substr(2));
		}
		else
		{
			native_path = std::filesystem::path(L"\\\\?\\" + absolute);
		}
		return true;
	}

#else
	inline bool NativeFilePath(const std::filesystem::path& path, std::filesystem::path& native_path, std::string&, bool = false)
	{
		native_path = path;
		return true;
	}
#endif

	/// <summary>Open all file readers through the same platform path conversion.</summary>
	inline bool OpenReadFile(const std::filesystem::path& path, std::ifstream& file)
	{
		std::filesystem::path native_path;
		std::string error;
		if (!NativeFilePath(path, native_path, error)) return false;
		file.open(native_path, std::ios::in | std::ios::binary);
		return static_cast<bool>(file);
	}

	inline bool TryReadFile(const std::filesystem::path& path, std::string& out)
	{
		out.clear();
		std::ifstream file;
		if (!OpenReadFile(path, file))
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
		std::ifstream file;
		if (!OpenReadFile(path, file)) return false;

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
		std::ifstream file;
		if (!OpenReadFile(path, file))
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
		std::ifstream file;
		if (!OpenReadFile(path, file))
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
		std::filesystem::path native_path;
		std::string normalization_error;
		if (!NativeFilePath(path, native_path, normalization_error)) return;
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::symlink_status(native_path, error);
		if (!error && status.type() != std::filesystem::file_type::not_found && !std::filesystem::is_symlink(status))
		{
			std::filesystem::remove(native_path, error);
		}
	}

	inline void CleanupAbandonedAtomicTemps(const std::filesystem::path& path)
	{
		std::filesystem::path native_path;
		std::filesystem::path native_parent;
		std::string normalization_error;
		if (path.parent_path().empty() || !NativeFilePath(path, native_path, normalization_error) ||
		    !NativeFilePath(path.parent_path(), native_parent, normalization_error, true) || !uam::paths::PathExistsNoThrow(native_path))
		{
			return;
		}

		const std::string prefix = uam::paths::Utf8PathString(path.filename()) + kTempWritePathSuffix;
		std::error_code error;
		for (std::filesystem::directory_iterator it(native_parent, std::filesystem::directory_options::skip_permission_denied, error), end;
		     !error && it != end;
		     it.increment(error))
		{
			const std::filesystem::directory_entry& entry = *it;
			const std::string name = uam::paths::Utf8PathString(entry.path().filename());
			std::filesystem::path native_entry;
			if (!NativeFilePath(entry.path(), native_entry, normalization_error)) continue;
			std::error_code status_error;
			const std::filesystem::file_status status = std::filesystem::symlink_status(native_entry, status_error);
			if (status_error || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status) || !name.starts_with(prefix))
			{
				continue;
			}

			std::error_code time_error;
			const std::filesystem::file_time_type modified = std::filesystem::last_write_time(native_entry, time_error);
			if (!time_error && std::filesystem::file_time_type::clock::now() - modified >= std::chrono::hours(24))
			{
				std::filesystem::remove(native_entry, status_error);
			}
		}
	}

#if defined(_WIN32)

	inline bool WriteAndSyncAtomicTemp(
	    const std::filesystem::path& temp_path,
	    std::string_view content,
	    const AtomicWriteStageHook& hook,
	    AtomicWriteResult& result)
	{
		std::filesystem::path native_temp_path;
		if (!NativeFilePath(temp_path, native_temp_path, result.error)) return false;
		HANDLE file = CreateFileW(native_temp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
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
		std::filesystem::path native_path;
		std::filesystem::path native_temp_path;
		std::filesystem::path native_backup_temp_path;
		if (!NativeFilePath(path, native_path, result.error) ||
		    !NativeFilePath(temp_path, native_temp_path, result.error) ||
		    (preserve_backup && !NativeFilePath(backup_temp_path, native_backup_temp_path, result.error))) return false;
		const bool replaced = destination_exists
		    ? ReplaceFileW(native_path.c_str(), native_temp_path.c_str(), preserve_backup ? native_backup_temp_path.c_str() : nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)
		    : MoveFileExW(native_temp_path.c_str(), native_path.c_str(), MOVEFILE_WRITE_THROUGH);
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
#if defined(__APPLE__)
		const bool synced = fcntl(fd, F_FULLFSYNC) == 0 || fsync(fd) == 0;
#else
		const bool synced = fsync(fd) == 0;
#endif
		if (!synced)
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

	/// <summary>Publishes an already synced temporary file with the normal atomic backup contract.</summary>
	using AtomicTempProducer = std::function<bool(const std::function<bool(std::string_view)>&)>;

	/// <summary>Produces a complete private temporary file in bounded chunks and syncs it before publication.</summary>
	inline bool WriteAndSyncAtomicTemp(const std::filesystem::path& temp_path,
	    const AtomicTempProducer& produce, const AtomicWriteStageHook& hook, AtomicWriteResult& result)
	{
		bool file_closed = false;
#if defined(_WIN32)
		std::filesystem::path native_path;
		if (!NativeFilePath(temp_path, native_path, result.error)) return false;
		HANDLE file = CreateFileW(native_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
		if (file == INVALID_HANDLE_VALUE) { result.error = "Failed to create temporary file: " + AtomicWriteSystemError(GetLastError()); return false; }
		const auto close_file = [&]()
		{
			if (file_closed) return true;
			file_closed = true;
			const bool closed = CloseHandle(file) != 0;
			if (!closed && result.error.empty()) result.error = "Failed to close temporary file: " + AtomicWriteSystemError(GetLastError());
			return closed;
		};
#else
		const int file = open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
		if (file < 0) { result.error = "Failed to create temporary file: " + AtomicWriteSystemError(errno); return false; }
		const auto close_file = [&]()
		{
			if (file_closed) return true;
			file_closed = true;
			const bool closed = close(file) == 0;
			if (!closed && result.error.empty()) result.error = "Failed to close temporary file: " + AtomicWriteSystemError(errno);
			return closed;
		};
#endif
		try
		{
			if (!ContinueAtomicWrite(hook, AtomicWriteStage::AfterCreate, result)) { close_file(); return false; }
			bool writing_started = false;
			const std::function<bool(std::string_view)> append = [&](std::string_view chunk)
			{
				if (!writing_started)
				{
					writing_started = true;
					if (!ContinueAtomicWrite(hook, AtomicWriteStage::DuringWrite, result)) return false;
				}
				std::size_t offset = 0;
				while (offset < chunk.size())
				{
					const std::size_t count = std::min<std::size_t>(chunk.size() - offset, 1024U * 1024U);
#if defined(_WIN32)
					DWORD written = 0;
					if (!WriteFile(file, chunk.data() + offset, static_cast<DWORD>(count), &written, nullptr) || written == 0)
					{
						result.error = "Failed to write temporary file: " + AtomicWriteSystemError(GetLastError()); return false;
					}
#else
					const ssize_t written = write(file, chunk.data() + offset, count);
					if (written < 0 && errno == EINTR) continue;
					if (written <= 0) { result.error = "Failed to write temporary file: " + AtomicWriteSystemError(errno); return false; }
#endif
					offset += static_cast<std::size_t>(written);
				}
				return true;
			};
			if (!produce(append) || (!writing_started && !ContinueAtomicWrite(hook, AtomicWriteStage::DuringWrite, result)) ||
			    !ContinueAtomicWrite(hook, AtomicWriteStage::AfterFlush, result)) { close_file(); return false; }
#if defined(_WIN32)
			const bool synced = FlushFileBuffers(file) != 0;
			if (!synced) result.error = "Failed to sync temporary file: " + AtomicWriteSystemError(GetLastError());
#else
#if defined(__APPLE__)
			const bool synced = fcntl(file, F_FULLFSYNC) == 0 || fsync(file) == 0;
#else
			const bool synced = fsync(file) == 0;
#endif
			if (!synced) result.error = "Failed to sync temporary file: " + AtomicWriteSystemError(errno);
#endif
			const bool closed = close_file();
			return synced && closed && ContinueAtomicWrite(hook, AtomicWriteStage::AfterFileSync, result);
		}
		catch (...) { close_file(); throw; }
	}

	inline AtomicWriteResult CommitSyncedAtomicTemp(const std::filesystem::path& path,
	    const std::filesystem::path& temp_path, bool preserve_backup = false,
	    const AtomicWriteStageHook& hook = {}, bool sync_directory = true)
	{
		AtomicWriteResult result;
		const std::filesystem::path parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
		std::error_code ec;
		std::filesystem::path native_path;
		if (!NativeFilePath(path, native_path, result.error)) return result;
		const std::filesystem::file_status status = std::filesystem::symlink_status(native_path, ec);
		if (ec && ec != std::errc::no_such_file_or_directory) { result.error = "Could not inspect destination: " + ec.message(); return result; }
		const bool destination_exists = !ec && status.type() != std::filesystem::file_type::not_found;
		if (destination_exists && !std::filesystem::is_regular_file(status)) { result.error = "Destination must be a regular file."; return result; }
		const std::filesystem::path backup_path = MakeBackupPath(path);
		const std::filesystem::path backup_temp_path = MakeTempWritePath(backup_path);
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
#if defined(__APPLE__)
			const int swapped = renameatx_np(AT_FDCWD, temp_path.c_str(), AT_FDCWD, path.c_str(), RENAME_SWAP);
#else
			const long swapped = syscall(SYS_renameat2, AT_FDCWD, temp_path.c_str(), AT_FDCWD, path.c_str(), RENAME_EXCHANGE);
#endif
			if (swapped != 0)
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
#if !defined(_WIN32)
		if (sync_directory && !SyncAtomicWriteDirectory(parent, result))
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
#if !defined(_WIN32)
				RemoveAtomicTempNoThrow(temp_path);
#endif
				result.success = true;
				result.primary_committed = true;
				result.stage = AtomicWriteStage::Complete;
				return result;
			}
#if defined(_WIN32)
			std::filesystem::path native_backup_temp_path;
			std::filesystem::path native_backup_path;
			if (!NativeFilePath(backup_temp_path, native_backup_temp_path, result.error) ||
			    !NativeFilePath(backup_path, native_backup_path, result.error))
			{
				result.success = true;
				result.backup_degraded = true;
				return result;
			}
			if (!MoveFileExW(native_backup_temp_path.c_str(), native_backup_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
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
			if (sync_directory && !SyncAtomicWriteDirectory(parent, result))
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
		std::filesystem::path native_parent;
		std::filesystem::path native_path;
		// Directory creation reaches its legacy limit before a child file requires an extended path.
		if (!NativeFilePath(parent, native_parent, result.error, true) || !NativeFilePath(path, native_path, result.error)) return result;
		if (!uam::paths::CreateDirectoriesNoThrow(native_parent, &ec))
		{
			result.error = "Failed to create destination directory: " + ec.message();
			return result;
		}
		const std::filesystem::file_status destination_status = std::filesystem::symlink_status(native_path, ec);
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
		return CommitSyncedAtomicTemp(path, temp_path, preserve_backup, hook);
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
