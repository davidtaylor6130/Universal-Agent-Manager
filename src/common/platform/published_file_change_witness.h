#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <stop_token>
#include <algorithm>
#include <functional>
#include <cerrno>
#include "common/platform/platform_incremental_sha256.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace uam::platform { class AtomicTempWriterOrigin; }
namespace uam::io
{
	enum class AtomicWriteStage;
	struct AtomicWriteResult;
	inline bool WriteAndSyncAtomicTempWithOrigin(const std::filesystem::path&,
	    const std::function<bool(const std::function<bool(std::string_view)>&)>&,
	    const std::function<bool(AtomicWriteStage)>&, AtomicWriteResult&,
	    std::shared_ptr<const uam::platform::AtomicTempWriterOrigin>&);
}

namespace uam::platform
{
	/// Retains the actual opened object, preventing file-ID reuse. It grants no read/input authority.
	/// Attribute-only Current checks detect canonical replacement and in-place change since Capture.
	class PublishedFileChangeWitness final
	{
	public:
		PublishedFileChangeWitness(const PublishedFileChangeWitness&) = delete;
		PublishedFileChangeWitness& operator=(const PublishedFileChangeWitness&) = delete;
		~PublishedFileChangeWitness() { Close(m_handle); }

		static std::shared_ptr<const PublishedFileChangeWitness> Capture(const std::filesystem::path& path)
		{
			HandleOwner opened{Open(path, true)};
			if (opened.value == Invalid()) return {};
			auto* raw = new PublishedFileChangeWitness(path, opened.value);
			opened.value = Invalid(); // shared_ptr deletes raw if its control-block allocation fails.
			auto result = std::shared_ptr<PublishedFileChangeWitness>(raw);
			const auto initial = Read(result->m_handle);
			if (!initial) return {};
			result->m_initial = *initial;
			return result->Current() ? result : std::shared_ptr<const PublishedFileChangeWitness>{};
		}

		bool Current() const
		{
			const auto pinned = Read(m_handle);
			if (!pinned || *pinned != m_initial) return false;
			const Handle named = Open(m_path, false);
			if (named == Invalid()) return false;
			const auto current = Read(named);
			Close(named);
			const auto after = Read(m_handle);
			return current && *current == m_initial && after && *after == m_initial;
		}




		/// Identity only across an acknowledged rename; content/currentness require their own checks.
		/// Both retained handles prevent file-ID reuse while this observation is evaluated.
		bool SameRetainedObject(const PublishedFileChangeWitness& other) const
		{
			const auto left = Read(m_handle), right = Read(other.m_handle);
			return left && right && *left == *right && (*left)[0] == m_initial[0] &&
			    (*left)[1] == m_initial[1] && (*left)[2] == m_initial[2];
		}

		/// Worker-only bounded bytes from this exact held regular object for a canonical decoder.
		/// A copy is DATA only; it is not a loaded predecessor, save receipt or input permission.
		std::optional<std::string> ReadBounded(std::uint64_t max_bytes, std::stop_token cancel = {}) const
		try
		{
			const std::uint64_t size = m_initial[3];
			if (cancel.stop_requested() || max_bytes == 0 || size == 0 || size > max_bytes ||
			    size > std::string{}.max_size() || !Current()) return {};
			std::string bytes(static_cast<std::size_t>(size), '\0');
			std::uint64_t offset = 0;
			while (offset < size)
			{
				const auto stamp = Read(m_handle);
				if (cancel.stop_requested() || !stamp || *stamp != m_initial) return {};
				const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(65536, size - offset));
				const auto count = ReadAt(m_handle, bytes.data() + static_cast<std::size_t>(offset), wanted, offset);
				if (count <= 0 || static_cast<std::size_t>(count) > wanted) return {};
				offset += static_cast<std::uint64_t>(count);
			}
			char extra = 0;
			if (ReadAt(m_handle, &extra, 1, size) != 0 || cancel.stop_requested() || !Current()) return {};
			return bytes;
		}
		catch (...) { return {}; }


		/// Worker-only. Hashes this SAME held regular object using explicit bounded offsets.
		/// No leaf reopen for content, FIFO/symlink traversal or shared file-position mutation.
		bool MatchesContent(std::uint64_t expected_bytes, std::string_view expected_sha256) const
		{
			if (expected_sha256.size() != 64 || expected_bytes == 0 || expected_bytes != m_initial[3] || !Current()) return false;
			IncrementalSha256 hash;
			std::array<char, 65536> bytes{};
			std::uint64_t offset = 0;
			while (offset < expected_bytes)
			{
				const auto stamp = Read(m_handle);
				if (!stamp || *stamp != m_initial) return false;
				const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), expected_bytes - offset));
				const auto count = ReadAt(m_handle, bytes.data(), wanted, offset);
				if (count <= 0 || static_cast<std::uint64_t>(count) > expected_bytes - offset ||
				    !hash.Add(std::string_view(bytes.data(), static_cast<std::size_t>(count)))) return false;
				offset += static_cast<std::uint64_t>(count);
			}
			char extra = 0;
			if (ReadAt(m_handle, &extra, 1, expected_bytes) != 0) return false;
			const auto after = Read(m_handle);
			return after && *after == m_initial && hash.Finish() == expected_sha256 && Current();
		}

	private:
		friend class AtomicTempWriterOrigin;
		using Stamp = std::array<std::uint64_t, 10>;
#if defined(_WIN32)
		using Handle = HANDLE;
		static Handle Invalid() { return INVALID_HANDLE_VALUE; }
		// Retained handles stay attribute-only: ReplaceFileW opens its replacement with no sharing,
		// so a held GENERIC_READ witness on a staged file fails the publish with ERROR_SHARING_VIOLATION.
		static Handle Open(const std::filesystem::path& path, bool)
		{
			return CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
			    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			    FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		}
		static std::ptrdiff_t ReadAt(Handle retained, char* bytes, std::size_t wanted, std::uint64_t offset)
		{
			// Content reads reopen the SAME held object (never its pathname) and close before returning.
			HandleOwner content{ReOpenFile(retained, GENERIC_READ,
			    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED)};
			if (content.value == Invalid()) return -1;
			const Handle handle = content.value;
			HandleOwner event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
			if (!event.value || event.value == Invalid()) return -1;
			OVERLAPPED operation{};
			operation.Offset = static_cast<DWORD>(offset);
			operation.OffsetHigh = static_cast<DWORD>(offset >> 32);
			operation.hEvent = event.value;
			DWORD count = 0;
			if (!ReadFile(handle, bytes, static_cast<DWORD>(wanted), &count, &operation))
			{
				const DWORD error = GetLastError();
				if (error == ERROR_HANDLE_EOF) return 0;
				if (error != ERROR_IO_PENDING || !GetOverlappedResult(handle, &operation, &count, TRUE))
					return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
			}
			return static_cast<std::ptrdiff_t>(count);
		}
		static void Close(Handle handle) { if (handle != Invalid()) CloseHandle(handle); }
		static std::optional<Stamp> Read(Handle handle)
		{
			BY_HANDLE_FILE_INFORMATION identity{};
			FILE_BASIC_INFO basic{};
			if (handle == Invalid() || GetFileType(handle) != FILE_TYPE_DISK ||
			    !GetFileInformationByHandle(handle, &identity) ||
			    !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) ||
			    (identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
			    identity.nNumberOfLinks != 1) return {};
			const auto size = (static_cast<std::uint64_t>(identity.nFileSizeHigh) << 32) | identity.nFileSizeLow;
			return Stamp{identity.dwVolumeSerialNumber, identity.nFileIndexHigh, identity.nFileIndexLow, size,
			    static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart), 0,
			    static_cast<std::uint64_t>(basic.ChangeTime.QuadPart), 0, identity.nNumberOfLinks, basic.FileAttributes};
		}
#else
		using Handle = int;
		static Handle Invalid() { return -1; }
		static Handle Open(const std::filesystem::path& path, bool)
		{
			return open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
		}
		static std::ptrdiff_t ReadAt(Handle handle, char* bytes, std::size_t wanted, std::uint64_t offset)
		{
			ssize_t count;
			do { count = pread(handle, bytes, wanted, static_cast<off_t>(offset)); } while (count < 0 && errno == EINTR);
			return static_cast<std::ptrdiff_t>(count);
		}
		static void Close(Handle handle) { if (handle >= 0) close(handle); }
		static std::optional<Stamp> Read(Handle handle)
		{
			struct stat file{};
			if (handle < 0 || fstat(handle, &file) != 0 || !S_ISREG(file.st_mode) || file.st_size < 0 || file.st_nlink != 1) return {};
#if defined(__APPLE__)
			const auto modified = file.st_mtimespec, changed = file.st_ctimespec;
#else
			const auto modified = file.st_mtim, changed = file.st_ctim;
#endif
			return Stamp{static_cast<std::uint64_t>(file.st_dev), static_cast<std::uint64_t>(file.st_ino), 0,
			    static_cast<std::uint64_t>(file.st_size), static_cast<std::uint64_t>(modified.tv_sec), static_cast<std::uint64_t>(modified.tv_nsec),
			    static_cast<std::uint64_t>(changed.tv_sec), static_cast<std::uint64_t>(changed.tv_nsec),
			    static_cast<std::uint64_t>(file.st_nlink), static_cast<std::uint64_t>(file.st_mode)};
		}
#endif
		struct HandleOwner
		{
			Handle value;
			~HandleOwner() { Close(value); }
		};
		PublishedFileChangeWitness(std::filesystem::path path, Handle handle) : m_path(std::move(path)), m_handle(handle) {}
		std::filesystem::path m_path;
		Handle m_handle = Invalid();
		Stamp m_initial{};
	};
	/// Identity of an actual exclusive atomic writer, retained while that writer is alive.
	/// No public factory, raw-handle API, content claim or runtime/input authority.
	class AtomicTempWriterOrigin final
	{
	public:
		AtomicTempWriterOrigin(const AtomicTempWriterOrigin&) = delete;
		AtomicTempWriterOrigin& operator=(const AtomicTempWriterOrigin&) = delete;
		~AtomicTempWriterOrigin() { PublishedFileChangeWitness::Close(m_handle); }
		bool Matches(const PublishedFileChangeWitness& physical) const
		{
			const auto writer = PublishedFileChangeWitness::Read(m_handle);
			const auto observed = PublishedFileChangeWitness::Read(physical.m_handle);
			return writer && observed && *writer == *observed && (*writer)[0] == m_identity[0] &&
			    (*writer)[1] == m_identity[1] && (*writer)[2] == m_identity[2];
		}
	private:
		friend bool uam::io::WriteAndSyncAtomicTempWithOrigin(const std::filesystem::path&,
		    const std::function<bool(const std::function<bool(std::string_view)>&)>&,
		    const std::function<bool(uam::io::AtomicWriteStage)>&, uam::io::AtomicWriteResult&,
		    std::shared_ptr<const AtomicTempWriterOrigin>&);
		using Handle = PublishedFileChangeWitness::Handle;
		static std::shared_ptr<const AtomicTempWriterOrigin> Retain(Handle writer)
		{
#if defined(_WIN32)
			// A separate attribute-only open avoids keeping a writing file object alive after creator close.
			// ReOpenFile uses the ACTUAL live creator HANDLE, never its pathname or a sampled file ID.
			const Handle duplicate = ReOpenFile(writer, FILE_READ_ATTRIBUTES,
			    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_OPEN_REPARSE_POINT);
			if (duplicate == PublishedFileChangeWitness::Invalid()) return {};
			if (!SetHandleInformation(duplicate, HANDLE_FLAG_INHERIT, 0)) { CloseHandle(duplicate); return {}; }
#else
			const Handle duplicate = fcntl(writer, F_DUPFD_CLOEXEC, 0);
			if (duplicate < 0) return {};
#endif
			PublishedFileChangeWitness::HandleOwner owned{duplicate};
			const auto identity = PublishedFileChangeWitness::Read(duplicate);
			if (!identity) return {};
			auto* raw = new AtomicTempWriterOrigin(duplicate, *identity);
			owned.value = PublishedFileChangeWitness::Invalid();
			return std::shared_ptr<const AtomicTempWriterOrigin>(raw);
		}
		AtomicTempWriterOrigin(Handle handle, PublishedFileChangeWitness::Stamp identity) : m_handle(handle), m_identity(identity) {}
		Handle m_handle;
		PublishedFileChangeWitness::Stamp m_identity;
	};

}
