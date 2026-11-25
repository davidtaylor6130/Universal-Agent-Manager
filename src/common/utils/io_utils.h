#pragma once

#include "common/paths/path_utils.h"
#include "common/platform/published_file_change_witness.h"
#include "common/utils/time_utils.h"

#include <algorithm>
#include <array>
#include <atomic>
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
#include <thread>
#include <unordered_map>
#include <vector>

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

	// Test-only delay before each durable temporary-file write on the calling thread only, so a case can
	// prove its own thread (for example the UI poll) performs no synchronous durable write.
	inline thread_local int t_durable_write_delay_ms_for_tests = 0;
	inline void DelayDurableWriteForTests()
	{
		if (t_durable_write_delay_ms_for_tests > 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(t_durable_write_delay_ms_for_tests));
	}

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

	/// <summary>Reads a whole file of at most max_bytes; a larger file is refused by its size before any body is read.</summary>
	inline bool TryReadTextFile(
	    const std::filesystem::path& path, std::string& out, std::size_t max_bytes)
	{
		out.clear();
		std::filesystem::path native_path;
		std::string path_error;
		std::error_code size_error;
		if (!NativeFilePath(path, native_path, path_error)) return false;
		const std::uintmax_t size = std::filesystem::file_size(native_path, size_error);
		if (!size_error && size > max_bytes) return false;
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

	/// Outcome of a read that must not mistake an I/O failure for an absent or empty file.
	enum class ReadFileStatus
	{
		Read,
		Missing,
		Error,
	};

	/// Test-only: a read of this exact path reports ReadFileStatus::Error on this thread.
	inline thread_local std::filesystem::path t_read_failure_path_for_tests;

	/// Reads a whole file, reporting Missing only when nothing exists at the path. A path that exists
	/// but cannot be opened, is not a regular file or hits a stream error reports Error, never "".
	/// A short read without a stream error is not detected here; callers validate completeness.
	inline ReadFileStatus TryReadTextFileStatus(const std::filesystem::path& path, std::string& out)
	{
		out.clear();
		if (!t_read_failure_path_for_tests.empty() && path == t_read_failure_path_for_tests) return ReadFileStatus::Error;
		std::filesystem::path native_path;
		std::string error;
		if (!NativeFilePath(path, native_path, error)) return ReadFileStatus::Error;
		std::error_code ec;
		const std::filesystem::file_status status = std::filesystem::status(native_path, ec);
		if (ec)
		{
			if (ec == std::errc::no_such_file_or_directory) return ReadFileStatus::Missing;
			return ReadFileStatus::Error;
		}
		if (status.type() == std::filesystem::file_type::not_found) return ReadFileStatus::Missing;
		if (!std::filesystem::is_regular_file(status)) return ReadFileStatus::Error;
		return TryReadFile(path, out) ? ReadFileStatus::Read : ReadFileStatus::Error;
	}

	enum class BoundedReadStatus
	{
		Read,
		Unreadable,
		TooLarge,
	};

	struct BoundedReadResult
	{
		BoundedReadStatus status = BoundedReadStatus::Unreadable;
		// Observed on-disk size; zero when the size could not be read.
		std::uintmax_t source_bytes = 0;
	};

	/// <summary>Reads a whole file only when it fits max_bytes, checking its size before allocating any body.</summary>
	/// <remarks>The bounded reader still enforces the ceiling if the file grows after the size check. The file is never changed.</remarks>
	inline BoundedReadResult TryReadFileWithinLimit(const std::filesystem::path& path, std::string& out, std::size_t max_bytes)
	{
		out.clear();
		BoundedReadResult result;
		std::filesystem::path native_path;
		std::string path_error;
		std::error_code size_error;
		if (!NativeFilePath(path, native_path, path_error)) return result;
		result.source_bytes = std::filesystem::file_size(native_path, size_error);
		if (size_error)
		{
			result.source_bytes = 0;
			return result;
		}
		if (result.source_bytes > max_bytes)
		{
			result.status = BoundedReadStatus::TooLarge;
			return result;
		}
		// One allocation of the observed size; the bounded reader keeps capacity across its clear().
		out.reserve(static_cast<std::size_t>(result.source_bytes));
		if (TryReadTextFile(path, out, max_bytes))
		{
			result.status = BoundedReadStatus::Read;
			return result;
		}
		std::string().swap(out);
		// The file grew past the ceiling during the read, or the read itself failed.
		const std::uintmax_t grown = std::filesystem::file_size(native_path, size_error);
		if (!size_error && grown > max_bytes)
		{
			result.source_bytes = grown;
			result.status = BoundedReadStatus::TooLarge;
		}
		return result;
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
		/// POSIX only: the destination's volume refused an exchange rename, so the predecessor was kept by a
		/// copy before a plain rename replaced the destination (see ReplaceExistingDestination).
		bool replace_fallback = false;
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

	/// <summary>Directory entries the abandoned-temporary sweeps have looked at; lets a test bound the work per write.</summary>
	inline std::atomic<std::uint64_t>& AtomicTempSweepEntriesExamined()
	{
		static std::atomic<std::uint64_t> examined{0};
		return examined;
	}

	/// <summary>Removes one atomic temporary abandoned for at least 24 hours; returns whether it is gone.</summary>
	inline bool RemoveAbandonedAtomicTempNoThrow(const std::filesystem::path& temp_path)
	{
		std::filesystem::path native_entry;
		std::string normalization_error;
		if (!NativeFilePath(temp_path, native_entry, normalization_error)) return false;
		std::error_code status_error;
		const std::filesystem::file_status status = std::filesystem::symlink_status(native_entry, status_error);
		if (status.type() == std::filesystem::file_type::not_found) return true;
		if (status_error) return false;
		if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) return false;
		std::error_code time_error;
		const std::filesystem::file_time_type modified = std::filesystem::last_write_time(native_entry, time_error);
		if (time_error || std::filesystem::file_time_type::clock::now() - modified < std::chrono::hours(24)) return false;
		return std::filesystem::remove(native_entry, status_error) || !status_error;
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
			++AtomicTempSweepEntriesExamined();
			// Name first: a chats directory holds thousands of entries and a stat each is the whole cost.
			if (!uam::paths::Utf8PathString(entry.path().filename()).starts_with(prefix)) continue;
			RemoveAbandonedAtomicTempNoThrow(entry.path());
		}
	}

	/// <summary>Guards only the throttled sweep's cached directory listings, never directory reads, removals or writes.</summary>
	inline std::mutex& AtomicTempListingsMutex()
	{
		static std::mutex* listings_mutex = new std::mutex(); // Leaked: a detached writer may still sweep during exit.
		return *listings_mutex;
	}

	/// <summary>Removes the abandoned temporaries of one destination, reading its directory at most once per interval.</summary>
	/// The caller holds that destination's durable writer lock, so no write to this destination (or its backup) is in
	/// flight. The cached listings have their own small mutex, held only to read or update the cache: directory reads,
	/// status checks and removals run without it, so writers to unrelated destinations never wait on each other here.
	/// Only temporaries at least 24 hours old are removed, so one that appears after the directory was listed is
	/// simply left for a later listing.
	inline void CleanupAbandonedAtomicTempsThrottled(const std::filesystem::path& path)
	{
		struct Listing
		{
			std::chrono::steady_clock::time_point read_at;
			std::vector<std::string> temp_names;
		};
		static auto& listings = *new std::unordered_map<std::string, Listing>(); // Leaked, like its mutex.
		std::filesystem::path native_parent;
		std::string normalization_error;
		if (path.parent_path().empty() || !NativeFilePath(path.parent_path(), native_parent, normalization_error, true)) return;
		const std::string key = uam::paths::Utf8PathString(native_parent);
		const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		bool refresh = false;
		{
			std::lock_guard<std::mutex> lock(AtomicTempListingsMutex());
			const auto [found, inserted] = listings.try_emplace(key);
			Listing& listing = found->second;
			// Claim the refresh before listing, so concurrent writers to this directory do not all read it. A writer
			// that arrives while the first listing is being read sees no names and leaves its temporaries for a later write.
			refresh = inserted || now - listing.read_at >= std::chrono::minutes(10);
			if (refresh) listing.read_at = now;
			else if (listing.temp_names.empty()) return;
		}
		if (refresh)
		{
			std::vector<std::string> temp_names;
			std::error_code error;
			for (std::filesystem::directory_iterator it(native_parent, std::filesystem::directory_options::skip_permission_denied, error), end;
			     !error && it != end;
			     it.increment(error))
			{
				++AtomicTempSweepEntriesExamined();
				std::string name = uam::paths::Utf8PathString(it->path().filename());
				if (name.find(kTempWritePathSuffix) != std::string::npos) temp_names.push_back(std::move(name));
			}
			std::lock_guard<std::mutex> lock(AtomicTempListingsMutex());
			listings[key].temp_names = std::move(temp_names);
			if (listings[key].temp_names.empty()) return;
		}
		// Same rule as the unthrottled sweep: an existing destination and an existing backup each own their prefix.
		std::vector<std::string> prefixes;
		for (const std::filesystem::path& owner : {path, MakeBackupPath(path)})
		{
			std::filesystem::path native_owner;
			if (!NativeFilePath(owner, native_owner, normalization_error) || !uam::paths::PathExistsNoThrow(native_owner)) continue;
			prefixes.push_back(uam::paths::Utf8PathString(owner.filename()) + kTempWritePathSuffix);
		}
		if (prefixes.empty()) return;
		const auto owned = [&prefixes](const std::string& name)
		{
			return std::ranges::any_of(prefixes, [&name](const std::string& prefix) { return name.starts_with(prefix); });
		};
		std::vector<std::string> candidates;
		{
			std::lock_guard<std::mutex> lock(AtomicTempListingsMutex());
			for (const std::string& name : listings[key].temp_names)
				if (owned(name)) candidates.push_back(name);
		}
		if (candidates.empty()) return;
		std::vector<std::string> removed;
		for (std::string& name : candidates)
			if (RemoveAbandonedAtomicTempNoThrow(path.parent_path() / uam::paths::PathFromUtf8(name))) removed.push_back(std::move(name));
		if (removed.empty()) return;
		std::lock_guard<std::mutex> lock(AtomicTempListingsMutex());
		std::erase_if(listings[key].temp_names, [&removed](const std::string& name) { return std::ranges::find(removed, name) != removed.end(); });
	}

#if defined(_WIN32)

	inline bool WriteAndSyncAtomicTemp(
	    const std::filesystem::path& temp_path,
	    std::string_view content,
	    const AtomicWriteStageHook& hook,
	    AtomicWriteResult& result, bool sync_to_disk = true)
	{
		if (sync_to_disk) DelayDurableWriteForTests();
		std::filesystem::path native_temp_path;
		if (!NativeFilePath(temp_path, native_temp_path, result.error)) return false;
		HANDLE file = CreateFileW(native_temp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | (sync_to_disk ? FILE_FLAG_WRITE_THROUGH : 0), nullptr);
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
		if (sync_to_disk && !FlushFileBuffers(file))
		{
			result.error = "Failed to sync temporary file: " + AtomicWriteSystemError(GetLastError());
			CloseHandle(file);
			return false;
		}
		CloseHandle(file);
		return !sync_to_disk || ContinueAtomicWrite(hook, AtomicWriteStage::AfterFileSync, result);
	}

	inline bool ReplaceAtomicTemp(
	    const std::filesystem::path& path,
	    const std::filesystem::path& temp_path,
	    const std::filesystem::path& backup_temp_path,
	    bool destination_exists,
	    bool preserve_backup,
	    AtomicWriteResult& result, bool sync_to_disk = true)
	{
		std::filesystem::path native_path;
		std::filesystem::path native_temp_path;
		std::filesystem::path native_backup_temp_path;
		if (!NativeFilePath(path, native_path, result.error) ||
		    !NativeFilePath(temp_path, native_temp_path, result.error) ||
		    (preserve_backup && !NativeFilePath(backup_temp_path, native_backup_temp_path, result.error))) return false;
		const bool replaced = destination_exists
		    ? ReplaceFileW(native_path.c_str(), native_temp_path.c_str(), preserve_backup ? native_backup_temp_path.c_str() : nullptr, sync_to_disk ? REPLACEFILE_WRITE_THROUGH : 0, nullptr, nullptr)
		    : MoveFileExW(native_temp_path.c_str(), native_path.c_str(), sync_to_disk ? MOVEFILE_WRITE_THROUGH : 0);
		if (!replaced)
		{
			result.error = "Failed to atomically replace destination: " + AtomicWriteSystemError(GetLastError());
			return false;
		}
		return true;
	}

	/// Installs a recovery backup over its predecessor. A retained attribute-only witness may still
	/// hold that predecessor open (FILE_SHARE_DELETE); legacy rename-replace refuses an open target
	/// with ERROR_ACCESS_DENIED, so retry with POSIX semantics, which unlinks it while handles stay valid.
	inline bool InstallBackupReplacing(const std::filesystem::path& native_source, const std::filesystem::path& native_target)
	{
		if (MoveFileExW(native_source.c_str(), native_target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
		const DWORD legacy_error = GetLastError();
		if (legacy_error != ERROR_ACCESS_DENIED) return false;
		const HANDLE source = CreateFileW(native_source.c_str(), DELETE | SYNCHRONIZE,
		    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_WRITE_THROUGH, nullptr);
		if (source == INVALID_HANDLE_VALUE) { SetLastError(legacy_error); return false; }
		const std::wstring& target = native_target.native();
		std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) + target.size() * sizeof(wchar_t));
		auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
		rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
		rename->FileNameLength = static_cast<DWORD>(target.size() * sizeof(wchar_t));
		std::copy(target.begin(), target.end(), rename->FileName);
		const bool renamed = SetFileInformationByHandle(source, FileRenameInfoEx, rename, static_cast<DWORD>(buffer.size())) != FALSE;
		CloseHandle(source);
		if (!renamed) SetLastError(legacy_error);
		return renamed;
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
	    AtomicWriteResult& result, bool sync_to_disk = true)
	{
		if (sync_to_disk) DelayDurableWriteForTests();
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
		const bool synced = !sync_to_disk || fcntl(fd, F_FULLFSYNC) == 0 || fsync(fd) == 0;
#else
		const bool synced = !sync_to_disk || fsync(fd) == 0;
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
		return !sync_to_disk || ContinueAtomicWrite(hook, AtomicWriteStage::AfterFileSync, result);
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

	/// <summary>Errors with which a volume refuses an exchange rename it does not implement (SMB, exFAT, NFS, some FUSE).</summary>
	/// <remarks>Same set as provider_setup.h PublishExclusive, plus ENOTTY, which some network clients return.</remarks>
	inline bool RenameSwapUnsupportedError(int error)
	{
		return error == ENOTSUP || error == EOPNOTSUPP || error == EINVAL || error == ENOSYS || error == ENOTTY;
	}

	// Test-only faults on the calling thread while set: the exchange rename, the fallback's installing rename, the
	// rename restoring a dropped destination and the rename moving the kept predecessor to the candidate name fail with
	// these errnos instead of making the system call.
	inline thread_local int t_rename_swap_errno_for_tests = 0;
	inline thread_local int t_replace_install_errno_for_tests = 0;
	inline thread_local int t_replace_restore_errno_for_tests = 0;
	inline thread_local int t_replace_reshape_errno_for_tests = 0;
	// Test-only, with an install errno: first remove the destination (a non-atomic network client), first complete
	// the rename (a client reporting an error after the server finished it), or first put an unrelated file there.
	inline thread_local bool t_replace_install_drops_destination_for_tests = false;
	inline thread_local bool t_replace_install_lands_before_error_for_tests = false;
	inline thread_local bool t_replace_install_substitutes_destination_for_tests = false;

	/// <summary>Volumes (by st_dev) whose exchange rename was refused in this process, so later replacements skip the attempt.</summary>
	/// <remarks>Only refusals are cached. A supported volume pays one lstat of the destination per replacement for the lookup.</remarks>
	struct RenameSwapCapabilityCache
	{
		std::mutex mutex;
		std::vector<dev_t> unsupported;
	};
	inline RenameSwapCapabilityCache& RenameSwapCapabilities()
	{
		static RenameSwapCapabilityCache cache;
		return cache;
	}
	inline bool RenameSwapKnownUnsupported(dev_t device)
	{
		auto& cache = RenameSwapCapabilities();
		std::lock_guard lock(cache.mutex);
		return std::find(cache.unsupported.begin(), cache.unsupported.end(), device) != cache.unsupported.end();
	}
	inline void RecordRenameSwapUnsupported(dev_t device)
	{
		auto& cache = RenameSwapCapabilities();
		std::lock_guard lock(cache.mutex);
		if (std::find(cache.unsupported.begin(), cache.unsupported.end(), device) == cache.unsupported.end()) cache.unsupported.push_back(device);
	}
	inline void ResetRenameSwapCapabilityCacheForTests()
	{
		auto& cache = RenameSwapCapabilities();
		std::lock_guard lock(cache.mutex);
		cache.unsupported.clear();
	}

	/// <summary>Copies an existing regular file to a new private sibling, keeping its mode where the volume allows.</summary>
	/// <remarks>A copy rather than a hard link: a link would change the predecessor's ctime and, after a crash, leave the
	/// destination with a second link that publication witnesses and data-folder inventory refuse. The copy gets a new
	/// modification time, so an abandoned one is swept no sooner than 24 hours after the save.</remarks>
	inline bool CopyPredecessorForReplace(const std::filesystem::path& source, const std::filesystem::path& target, bool sync_to_disk, int& error_out)
	{
		const int input = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (input < 0) { error_out = errno; return false; }
		struct stat info{};
		const bool inspected = fstat(input, &info) == 0;
		if (!inspected || !S_ISREG(info.st_mode))
		{
			error_out = inspected ? EINVAL : errno;
			close(input);
			return false;
		}
		const int output = open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
		if (output < 0) { error_out = errno; close(input); return false; }
		bool copied = true;
		std::array<char, 64 * 1024> buffer{};
		while (copied)
		{
			const ssize_t count = read(input, buffer.data(), buffer.size());
			if (count < 0 && errno == EINTR) continue;
			if (count < 0) { error_out = errno; copied = false; break; }
			if (count == 0) break;
			ssize_t offset = 0;
			while (offset < count)
			{
				const ssize_t written = write(output, buffer.data() + offset, static_cast<std::size_t>(count - offset));
				if (written < 0 && errno == EINTR) continue;
				if (written <= 0) { error_out = written < 0 ? errno : EIO; copied = false; break; }
				offset += written;
			}
		}
		// Mode is fidelity, not durability: exFAT and some shares refuse it, which is not a failure.
		if (copied) (void)fchmod(output, info.st_mode & 07777);
#if defined(__APPLE__)
		if (copied && sync_to_disk && fcntl(output, F_FULLFSYNC) != 0 && fsync(output) != 0) { error_out = errno; copied = false; }
#else
		if (copied && sync_to_disk && fsync(output) != 0) { error_out = errno; copied = false; }
#endif
		if (close(output) != 0 && copied) { error_out = errno; copied = false; }
		close(input);
		if (!copied) unlink(target.c_str());
		return copied;
	}

	enum class ReplaceExistingOutcome
	{
		Replaced,
		FailedUnchanged, // The destination still holds its previous file; the candidate may be discarded.
		FailedHeld       // The destination is missing or cannot be confirmed: the previous file is kept outside any sweep.
	};

	/// <summary>Replaces an existing destination with candidate, leaving the previous file at predecessor (normally the
	/// candidate path, exactly where an exchange rename leaves it).</summary>
	/// <remarks>Prefers the atomic exchange (macOS RENAME_SWAP, Linux RENAME_EXCHANGE). When the destination's volume
	/// refuses it, the volume is cached as unsupported and the replace falls back to: copy the predecessor to holder
	/// (synced when sync_directory), sync the directory, rename(candidate, destination), then move holder to the
	/// candidate name. The caller syncs the directory afterwards as on the exchange path. The fallback is only as atomic
	/// as the volume's rename-over-existing (an SMB client may delete then rename). After a failed install the
	/// destination's identity, size and modification time decide the outcome: the previous file removes the copy, the
	/// candidate counts as replaced, a missing name is restored from the copy, and anything ambiguous is held.
	/// holder_is_evidence marks a caller-reserved evidence path that keeps a held predecessor; otherwise a held copy
	/// becomes the ".bak" companion when the name is missing and none exists, else "<name>.held.<token>", which no
	/// abandoned-temporary sweep matches. See docs/engineering/durable-writes.md.</remarks>
	inline ReplaceExistingOutcome ReplaceExistingDestination(const std::filesystem::path& candidate, const std::filesystem::path& destination,
	    const std::filesystem::path& holder, bool holder_is_evidence, bool sync_directory, std::filesystem::path& predecessor, AtomicWriteResult& result)
	{
		predecessor = candidate;
		struct stat destination_info{};
		const bool device_known = lstat(destination.c_str(), &destination_info) == 0;
		if (!device_known || !RenameSwapKnownUnsupported(destination_info.st_dev))
		{
			int swap_error = t_rename_swap_errno_for_tests;
			if (swap_error == 0)
			{
#if defined(__APPLE__)
				if (renameatx_np(AT_FDCWD, candidate.c_str(), AT_FDCWD, destination.c_str(), RENAME_SWAP) == 0) return ReplaceExistingOutcome::Replaced;
#else
				if (syscall(SYS_renameat2, AT_FDCWD, candidate.c_str(), AT_FDCWD, destination.c_str(), RENAME_EXCHANGE) == 0) return ReplaceExistingOutcome::Replaced;
#endif
				swap_error = errno;
			}
			if (!RenameSwapUnsupportedError(swap_error))
			{
				result.error = "Failed to atomically swap destination: " + AtomicWriteSystemError(swap_error);
				return ReplaceExistingOutcome::FailedUnchanged;
			}
			if (device_known) RecordRenameSwapUnsupported(destination_info.st_dev);
		}
		result.replace_fallback = true;
		const std::filesystem::path parent = destination.parent_path().empty() ? std::filesystem::path(".") : destination.parent_path();

		// 1. Keep the predecessor before the destination name changes.
		int keep_error = 0;
		if (!device_known || !CopyPredecessorForReplace(destination, holder, sync_directory, keep_error))
		{
			result.error = "Failed to keep the destination's previous file before replacing it: " + AtomicWriteSystemError(device_known ? keep_error : ENOENT);
			return ReplaceExistingOutcome::FailedUnchanged;
		}
		if (sync_directory && !SyncAtomicWriteDirectory(parent, result))
		{
			unlink(holder.c_str());
			return ReplaceExistingOutcome::FailedUnchanged;
		}
		struct stat candidate_info{};
		const bool candidate_known = lstat(candidate.c_str(), &candidate_info) == 0;

		// 2. Install the candidate. POSIX rename replaces atomically where the volume implements it.
		int install_error = t_replace_install_errno_for_tests;
		if (install_error != 0 && t_replace_install_drops_destination_for_tests) unlink(destination.c_str());
		if (install_error != 0 && t_replace_install_lands_before_error_for_tests) (void)rename(candidate.c_str(), destination.c_str());
		if (install_error != 0 && t_replace_install_substitutes_destination_for_tests)
		{
			const std::filesystem::path other = MakeTempWritePath(destination);
			const int fd = open(other.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
			if (fd >= 0) { (void)!write(fd, "unrelated", 9); close(fd); (void)rename(other.c_str(), destination.c_str()); }
		}
		if (install_error == 0 && rename(candidate.c_str(), destination.c_str()) != 0) install_error = errno;
		if (install_error != 0)
		{
			struct stat remaining{};
			const bool present = lstat(destination.c_str(), &remaining) == 0;
			// Identity alone is not enough: smbfs without server file IDs and exFAT derive inode numbers from the name
			// or directory slot, so a renamed file can keep the old number. Size and modification time must agree too.
			const auto same_file = [&](const struct stat& info)
			{
#if defined(__APPLE__)
				const bool same_time = info.st_mtimespec.tv_sec == remaining.st_mtimespec.tv_sec && info.st_mtimespec.tv_nsec == remaining.st_mtimespec.tv_nsec;
#else
				const bool same_time = info.st_mtim.tv_sec == remaining.st_mtim.tv_sec && info.st_mtim.tv_nsec == remaining.st_mtim.tv_nsec;
#endif
				return info.st_dev == remaining.st_dev && info.st_ino == remaining.st_ino && info.st_size == remaining.st_size && same_time;
			};
			// Moves a held copy to a name no abandoned-temporary sweep matches and says where it is.
			const auto hold_copy = [&](const std::string& cause)
			{
				std::string kept = holder.filename().string();
				if (!holder_is_evidence)
				{
					const std::filesystem::path held = std::filesystem::path(destination) += ".held." + MakeTempWritePath(std::filesystem::path("x")).extension().string().substr(1);
					if (rename(holder.c_str(), held.c_str()) == 0)
					{
						kept = held.filename().string();
						if (sync_directory) (void)SyncAtomicWriteDirectory(parent, result);
					}
				}
				result.error = cause + "; its previous file is kept beside it as " + kept + ": " + AtomicWriteSystemError(install_error);
				return ReplaceExistingOutcome::FailedHeld;
			};
			if (present && candidate_known && same_file(candidate_info))
			{
				// The client reported an error after the rename landed: this is a completed replacement.
			}
			else if (present && same_file(destination_info) && !(candidate_known && candidate_info.st_ino == remaining.st_ino && candidate_info.st_dev == remaining.st_dev))
			{
				unlink(holder.c_str());
				result.error = "Failed to replace destination: " + AtomicWriteSystemError(install_error);
				return ReplaceExistingOutcome::FailedUnchanged;
			}
			else if (present)
			{
				return hold_copy("Failed to replace destination, which cannot be confirmed unchanged");
			}
			else
			{
				// A non-atomic client removed the old name before failing: put the kept predecessor back.
				int restore_error = t_replace_restore_errno_for_tests;
				if (restore_error == 0 && rename(holder.c_str(), destination.c_str()) != 0) restore_error = errno;
				if (restore_error == 0)
				{
					if (sync_directory) (void)SyncAtomicWriteDirectory(parent, result);
					result.error = "Failed to replace destination; its previous file was restored: " + AtomicWriteSystemError(install_error);
					return ReplaceExistingOutcome::FailedUnchanged;
				}
				// Recovery readers look for the ".bak" companion. Never overwrite an existing one, and leave a caller's
				// reserved evidence path where its owner reports it.
				if (!holder_is_evidence)
				{
					const std::filesystem::path backup = MakeBackupPath(destination);
					struct stat existing{};
					if (lstat(backup.c_str(), &existing) != 0 && errno == ENOENT && rename(holder.c_str(), backup.c_str()) == 0)
					{
						if (sync_directory) (void)SyncAtomicWriteDirectory(parent, result);
						result.error = "Failed to replace destination; its previous file is kept as its recovery backup: " + AtomicWriteSystemError(install_error);
						return ReplaceExistingOutcome::FailedHeld;
					}
				}
				return hold_copy("Failed to replace destination, which is missing");
			}
		}

		// 3. Match the exchange's shape so callers find the predecessor where they expect it.
		int reshape_error = t_replace_reshape_errno_for_tests;
		if (reshape_error == 0 && rename(holder.c_str(), candidate.c_str()) != 0) reshape_error = errno;
		if (reshape_error != 0) predecessor = holder;
		return ReplaceExistingOutcome::Replaced;
	}
#endif

	/// <summary>Publishes an already synced temporary file with the normal atomic backup contract.</summary>
	using AtomicTempProducer = std::function<bool(const std::function<bool(std::string_view)>&)>;

	/// <summary>Produces a complete private temporary file in bounded chunks and syncs it before publication.</summary>
	inline bool WriteAndSyncAtomicTemp(const std::filesystem::path& temp_path,
	    const AtomicTempProducer& produce, const AtomicWriteStageHook& hook, AtomicWriteResult& result)
	{
		DelayDurableWriteForTests();
		bool file_closed = false;
#if defined(_WIN32)
		std::filesystem::path native_path;
		if (!NativeFilePath(temp_path, native_path, result.error)) return false;
		// Producers append small chunks; write-through would make each one a synchronous disk write.
		// FlushFileBuffers below is the durability point, matching the POSIX fsync contract.
		HANDLE file = CreateFileW(native_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
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

	/// Opt-in actual-writer origin only; default writer body/sharing contract above is unchanged.
	inline bool WriteAndSyncAtomicTempWithOrigin(const std::filesystem::path& temp_path,
	    const AtomicTempProducer& produce, const AtomicWriteStageHook& hook, AtomicWriteResult& result,
	    std::shared_ptr<const uam::platform::AtomicTempWriterOrigin>& origin_out)
	{
		DelayDurableWriteForTests();
		origin_out.reset();
		bool file_closed = false;
#if defined(_WIN32)
		std::filesystem::path native_path;
		if (!NativeFilePath(temp_path, native_path, result.error)) return false;
		HANDLE file = CreateFileW(native_path.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr); // Synced by FlushFileBuffers, as above.
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
			// Mint from the ACTUAL exclusive writer while it remains alive, never from a path reopen.
			auto origin = synced ? uam::platform::AtomicTempWriterOrigin::Retain(file) : nullptr;
			if (synced && !origin && result.error.empty()) result.error = "Could not retain actual atomic writer identity.";
			const bool closed = close_file();
			if (!synced || !origin || !closed) return false;
			if (!ContinueAtomicWrite(hook, AtomicWriteStage::AfterFileSync, result)) return false;
			origin_out = std::move(origin);
			return true;
		}
		catch (...) { close_file(); throw; }
	}

	inline AtomicWriteResult CommitSyncedAtomicTemp(const std::filesystem::path& path,
	    const std::filesystem::path& temp_path, bool preserve_backup = false,
	    const AtomicWriteStageHook& hook = {}, bool sync_directory = true, bool sync_replace = true)
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
		if (!ReplaceAtomicTemp(path, temp_path, backup_temp_path, destination_exists, preserve_backup, result, sync_replace))
		{
			RemoveAtomicTempNoThrow(temp_path);
			return result;
		}
		result.primary_committed = true;
#else
		// After an exchange (or its fallback) the destination's previous file is here, not at temp_path.
		std::filesystem::path predecessor_path = temp_path;
		if (destination_exists)
		{
			const ReplaceExistingOutcome replaced = ReplaceExistingDestination(temp_path, path, MakeTempWritePath(path), false, sync_directory, predecessor_path, result);
			if (replaced != ReplaceExistingOutcome::Replaced)
			{
				if (replaced == ReplaceExistingOutcome::FailedUnchanged) RemoveAtomicTempNoThrow(temp_path);
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
				RemoveAtomicTempNoThrow(predecessor_path);
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
			if (!InstallBackupReplacing(native_backup_temp_path, native_backup_path))
			{
				result.error = "Destination was saved, but its recovery backup could not be installed: " + AtomicWriteSystemError(GetLastError());
				result.success = true;
				result.backup_degraded = true;
				return result;
			}
#else
			if (rename(predecessor_path.c_str(), backup_path.c_str()) != 0)
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
	// Opt-in private evidence handoff; the six-argument legacy path above is unchanged.
	inline AtomicWriteResult CommitSyncedAtomicTemp(
	    const std::filesystem::path& path, const std::filesystem::path& temp_path, bool preserve_backup,
	    const AtomicWriteStageHook& hook, bool sync_directory, bool sync_replace,
	    const std::filesystem::path& private_backup_temp_path)
	{
		AtomicWriteResult result;
		namespace fs = std::filesystem;
		std::error_code ec;
		const auto normalized = [&](const fs::path& value)
		{
			if (value.empty()) return fs::path{};
			const auto absolute = fs::absolute(value, ec);
			return ec ? fs::path{} : absolute.lexically_normal();
		};
		const fs::path destination = normalized(path);
		if (destination.empty()) { result.error = "Invalid conditional destination."; return result; }
		const fs::path candidate = normalized(temp_path);
		if (candidate.empty()) { result.error = "Invalid conditional candidate."; return result; }
		const fs::path evidence = normalized(private_backup_temp_path);
		if (evidence.empty()) { result.error = "Invalid private predecessor path."; return result; }
		const fs::path backup = MakeBackupPath(destination);
		const auto same = [](const fs::path& left, const fs::path& right)
		{
#if defined(_WIN32)
			return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
			return left == right;
#endif
		};
		if (!same(destination.parent_path(), candidate.parent_path()) || !same(destination.parent_path(), evidence.parent_path()) ||
		    same(destination, candidate) || same(destination, evidence) || same(candidate, evidence) || same(backup, evidence) || same(backup, candidate))
		{
			result.error = "Conditional candidate and private evidence must be distinct siblings.";
			return result;
		}
		fs::path native_path, native_candidate, native_evidence, native_parent;
		if (!NativeFilePath(destination, native_path, result.error) || !NativeFilePath(candidate, native_candidate, result.error) ||
		    !NativeFilePath(evidence, native_evidence, result.error) || !NativeFilePath(destination.parent_path(), native_parent, result.error, true))
			return result;
		const auto parent_status = fs::symlink_status(native_parent, ec);
		if (ec || !fs::is_directory(parent_status) || paths::IsLinkOrReparsePointNoThrow(native_parent))
		{
			result.error = "Conditional publication parent is not an available directory.";
			return result;
		}
		const auto absent = [&](const fs::path& value)
		{
			std::error_code error;
			const auto status = fs::symlink_status(value, error);
			return status.type() == fs::file_type::not_found &&
			    (!error || error == std::errc::no_such_file_or_directory);
		};
		if (!absent(native_evidence))
		{
			result.error = "Private predecessor path already has storage evidence.";
			return result;
		}
		const auto candidate_status = fs::symlink_status(native_candidate, ec);
		if (ec || !fs::is_regular_file(candidate_status) || paths::IsLinkOrReparsePointNoThrow(native_candidate))
		{
			result.error = "Conditional candidate must be a regular non-link file.";
			return result;
		}
		const auto status = fs::symlink_status(native_path, ec);
		if (ec && ec != std::errc::no_such_file_or_directory)
		{
			result.error = "Could not inspect conditional destination: " + ec.message();
			return result;
		}
		const bool destination_exists = !ec && status.type() != fs::file_type::not_found;
		if (destination_exists && (!fs::is_regular_file(status) || paths::IsLinkOrReparsePointNoThrow(native_path)))
		{
			result.error = "Conditional destination must be a regular non-link file.";
			return result;
		}
		if (!ContinueAtomicWrite(hook, AtomicWriteStage::BeforeReplace, result)) return result;
		// The final callback is read-only; still recheck this reserved absence before native replacement.
		if (!absent(native_evidence))
		{
			result.error = "Private predecessor path changed before replacement.";
			return result;
		}
#if defined(_WIN32)
		// Capture predecessors for secondary files too. Failed ReplaceFileW can be partial:
		// do not delete either possible evidence location after replacement permission.
		if (!ReplaceAtomicTemp(destination, candidate, evidence, destination_exists, destination_exists, result, sync_replace)) return result;
		result.primary_committed = true;
#else
		// The reserved private evidence path holds a fallback's kept predecessor, so retained evidence covers it.
		fs::path predecessor = candidate;
		if (destination_exists)
		{
			if (ReplaceExistingDestination(candidate, destination, evidence, true, sync_directory, predecessor, result) != ReplaceExistingOutcome::Replaced) return result;
		}
		else if (rename(candidate.c_str(), destination.c_str()) != 0)
		{
			result.error = "Failed to atomically install destination: " + AtomicWriteSystemError(errno);
			return result;
		}
#endif
		if (!ContinueAtomicWrite(hook, AtomicWriteStage::AfterReplaceBeforeDirectorySync, result)) return result;
#if !defined(_WIN32)
		if (sync_directory && !SyncAtomicWriteDirectory(destination.parent_path(), result)) return result;
		result.primary_committed = true;
#endif
		if (!ContinueAtomicWrite(hook, AtomicWriteStage::BeforeBackupCleanup, result))
		{
			result.success = result.primary_committed;
			result.backup_degraded = preserve_backup && destination_exists;
			return result;
		}
		if (destination_exists)
		{
			if (!preserve_backup)
			{
#if defined(_WIN32)
				const fs::path& kept = native_evidence;
#else
				const fs::path& kept = predecessor;
#endif
				std::error_code error;
				fs::remove(kept, error);
				result.success = true;
				if (error)
				{
					result.error = "Destination committed; private predecessor cleanup degraded: " + error.message();
					return result;
				}
			}
			else
			{
#if defined(_WIN32)
				fs::path native_backup;
				if (!NativeFilePath(backup, native_backup, result.error) ||
				    !InstallBackupReplacing(native_evidence, native_backup))
				{
					if (result.error.empty()) result.error = "Destination committed; recovery backup installation failed: " + AtomicWriteSystemError(GetLastError());
					result.success = true;
					result.backup_degraded = true;
					return result;
				}
#else
				if (rename(predecessor.c_str(), backup.c_str()) != 0)
				{
					result.error = "Destination committed; recovery backup installation failed: " + AtomicWriteSystemError(errno);
					result.success = true;
					result.backup_degraded = true;
					return result;
				}
				if (sync_directory && !SyncAtomicWriteDirectory(destination.parent_path(), result))
				{
					result.success = true;
					result.backup_degraded = true;
					return result;
				}
#endif
			}
		}
		result.success = true;
		result.primary_committed = true;
		result.stage = AtomicWriteStage::Complete;
		return result;
	}
	/// <summary>Normalised identity of a durable write destination: one lock per key.</summary>
	/// Absolute and lexically normal, so "a/./b.json" and "a/b.json" share a lock. Windows and macOS default to
	/// case-insensitive volumes, so ASCII case is folded there; folding a case-sensitive volume only serialises two
	/// distinct files, which is safe. Links, non-ASCII case, Unicode normalisation forms and Windows extended-length
	/// prefixes are not unified: such aliases of one file are not serialised, but every write is still a complete
	/// temp + rename publication.
	inline std::string DurableWriteLockKey(const std::filesystem::path& path)
	{
		std::error_code error;
		std::filesystem::path absolute = std::filesystem::absolute(path, error);
		if (error) absolute = path;
		std::string key = uam::paths::Utf8PathString(absolute.lexically_normal());
#if defined(_WIN32) || defined(__APPLE__)
		std::ranges::transform(key, key.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
#endif
		return key;
	}

	/// <summary>Holds the durable writer lock of one destination for its lifetime.</summary>
	/// The table mutex is held only to find or retire an entry, never while waiting for or holding a
	/// destination lock, so it is never held across disk I/O, fsync or a caller's stage hook. Entries are
	/// reference counted and removed when their last user leaves, so the table stays as small as the set of
	/// destinations being written at once. Destination locks are not recursive and have no ordering, so a stage
	/// hook must not perform durable writes: writing its own destination self-deadlocks, and two hooks writing
	/// each other's destinations deadlock. Only AtomicWriteFileDetailed and its wrappers take this lock; prepared
	/// publications through CommitSyncedAtomicTemp rely on their callers' own ordering.
	class DurableWriteDestinationLock
	{
	public:
		explicit DurableWriteDestinationLock(const std::filesystem::path& path) : m_key(DurableWriteLockKey(path))
		{
			{
				std::lock_guard<std::mutex> table_lock(TableMutex());
				Entry& entry = Table()[m_key];
				++entry.users;
				m_entry = &entry; // unordered_map nodes stay put until erased, and a counted entry is never erased.
			}
			try
			{
				m_entry->mutex.lock();
			}
			catch (...)
			{
				Release();
				throw;
			}
		}

		~DurableWriteDestinationLock()
		{
			m_entry->mutex.unlock();
			Release();
		}

		DurableWriteDestinationLock(const DurableWriteDestinationLock&) = delete;
		DurableWriteDestinationLock& operator=(const DurableWriteDestinationLock&) = delete;

		/// <summary>Whether a writer holds or waits for this destination's lock; lets a test confirm entries are retired.</summary>
		static bool IsTrackedForTests(const std::filesystem::path& path)
		{
			const std::string key = DurableWriteLockKey(path);
			std::lock_guard<std::mutex> table_lock(TableMutex());
			return Table().contains(key);
		}

	private:
		struct Entry
		{
			std::mutex mutex;
			std::size_t users = 0;
		};

		void Release() noexcept
		{
			std::lock_guard<std::mutex> table_lock(TableMutex());
			if (--m_entry->users == 0) Table().erase(m_key);
		}

		// Both leaked on purpose: a detached publisher thread may still be writing while statics are destroyed at exit.
		static std::mutex& TableMutex()
		{
			static std::mutex* table_mutex = new std::mutex();
			return *table_mutex;
		}

		static std::unordered_map<std::string, Entry>& Table()
		{
			static auto* table = new std::unordered_map<std::string, Entry>();
			return *table;
		}

		std::string m_key;
		Entry* m_entry = nullptr;
	};

	inline AtomicWriteResult AtomicWriteFileDetailed(
	    const std::filesystem::path& path,
	    std::string_view content,
	    const AtomicWriteStageHook& hook = {},
	    bool preserve_backup = false, bool sync_to_disk = true)
	{
		AtomicWriteResult result;
		if (path.empty() || path.filename().empty())
		{
			result.error = "Destination path is empty.";
			return result;
		}
		// Serialise writers of this destination only. Unrelated destinations (a settings save on the UI
		// thread and a large background snapshot, say) sync in parallel, and the caller's stage hooks run
		// under this destination's lock alone, never under a process-wide one.
		const DurableWriteDestinationLock destination_lock(path);

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

		CleanupAbandonedAtomicTempsThrottled(path);

		const std::filesystem::path temp_path = MakeTempWritePath(path);
		const std::filesystem::path backup_path = MakeBackupPath(path);
		const std::filesystem::path backup_temp_path = MakeTempWritePath(backup_path);
		if (!WriteAndSyncAtomicTemp(temp_path, content, hook, result, sync_to_disk))
		{
			if (!result.interrupted)
			{
				RemoveAtomicTempNoThrow(temp_path);
			}
			return result;
		}
		return CommitSyncedAtomicTemp(path, temp_path, preserve_backup, hook, sync_to_disk, sync_to_disk);
	}


	inline bool AtomicWriteFile(const std::filesystem::path& path, std::string_view content)
	{
		return AtomicWriteFileDetailed(path, content).success;
	}

	/// <summary>Atomically publishes reconstructible cache data without durable disk sync.</summary>
	inline bool WriteCacheFile(const std::filesystem::path& path, std::string_view content)
	{
#if defined(_WIN32)
		// Still temp+rename, minus the durable writer's per-destination lock, directory probe, second
		// destination probe and abandoned-temp sweeps: each is a filter-driver round trip on
		// Windows, and together they doubled cold 1000-chat summary regeneration.
		const std::filesystem::path temp_path = MakeTempWritePath(path);
		std::filesystem::path native_parent, native_path, native_temp;
		AtomicWriteResult result;
		std::error_code ec;
		if (path.filename().empty() || !NativeFilePath(path, native_path, result.error) || !NativeFilePath(temp_path, native_temp, result.error)) return false;
		const std::filesystem::file_status status = std::filesystem::symlink_status(native_path, ec);
		if (status.type() != std::filesystem::file_type::not_found && !std::filesystem::is_regular_file(status)) return false;
		bool written = WriteAndSyncAtomicTemp(temp_path, content, {}, result, false);
		if (!written)
		{
			RemoveAtomicTempNoThrow(temp_path);
			written = NativeFilePath(path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path(), native_parent, result.error, true) &&
			    uam::paths::CreateDirectoriesNoThrow(native_parent) && WriteAndSyncAtomicTemp(temp_path, content, {}, result, false);
		}
		if (written && MoveFileExW(native_temp.c_str(), native_path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
		RemoveAtomicTempNoThrow(temp_path);
		return false;
#else
		return AtomicWriteFileDetailed(path, content, {}, false, false).success;
#endif
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
