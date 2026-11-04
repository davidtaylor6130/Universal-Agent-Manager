#pragma once

#include "common/models/app_models.h"
#include "common/provider/provider_setup.h"
#include <memory>
#include <iterator>
#include <sstream>
#if defined(__APPLE__)
#include <copyfile.h>
#endif

namespace uam::central_instruction_files
{
	struct TextFile
	{
		bool ok = false;
		int status = 400;
		std::string path;
		std::string content;
		std::string error;
		std::filesystem::path resolved;
		std::string revision;
	};

	inline bool Authorized(const CentralProviderConfiguration& configuration, const std::string& path)
	{
		return !path.empty() && path.size() <= 4096 && path.find('\0') == std::string::npos &&
		    paths::PathFromUtf8(path).is_absolute() &&
		    std::find(configuration.instruction_files.begin(), configuration.instruction_files.end(), path) != configuration.instruction_files.end();
	}

	inline bool ValidateText(const std::string& text, std::string& error)
	{
		if (text.size() > provider_setup::kMaxFileBytes) { error = "Instruction files are limited to 2 MiB."; return false; }
		if (text.find('\0') != std::string::npos) { error = "Instruction files must contain text without NUL bytes."; return false; }
		try { (void)nlohmann::json(text).dump(); }
		catch (const nlohmann::json::exception&) { error = "Instruction files must contain valid UTF-8 text."; return false; }
		return true;
	}

	namespace detail
	{
#if !defined(_WIN32)
		/// <summary>Walk canonical components through directory handles so a link or FIFO cannot redirect/block reads.</summary>
		inline int Open(const std::filesystem::path& path)
		{
			int descriptor = open(path.root_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
			for (std::filesystem::path::const_iterator it = ++path.begin(); descriptor >= 0 && it != path.end(); ++it)
			{
				const bool final = std::next(it) == path.end();
				const int next = openat(descriptor, it->c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (final ? 0 : O_DIRECTORY));
				close(descriptor);
				descriptor = next;
			}
			return descriptor;
		}

		inline std::string Revision(const struct stat& info)
		{
			std::ostringstream revision;
			revision << info.st_dev << ':' << info.st_ino << ':' << info.st_size << ':' << info.st_mode << ':' << info.st_uid << ':' << info.st_gid << ':' << info.st_mtime << ':' << info.st_ctime;
#if defined(__APPLE__)
			revision << ':' << info.st_mtimespec.tv_nsec << ':' << info.st_ctimespec.tv_nsec;
#else
			revision << ':' << info.st_mtim.tv_nsec << ':' << info.st_ctim.tv_nsec;
#endif
			return revision.str();
		}
#else
		inline std::string Revision(const BY_HANDLE_FILE_INFORMATION& info)
		{
			std::ostringstream revision;
			revision << info.dwVolumeSerialNumber << ':' << info.nFileIndexHigh << ':' << info.nFileIndexLow << ':' << info.nFileSizeHigh << ':' << info.nFileSizeLow << ':' << info.dwFileAttributes << ':' << info.ftLastWriteTime.dwHighDateTime << ':' << info.ftLastWriteTime.dwLowDateTime;
			return revision.str();
		}
#endif
	}

	/// <summary>Read only an existing, persisted instruction path and return its exact bounded UTF-8 bytes.</summary>
	inline TextFile Read(const CentralProviderConfiguration& configuration, const std::string& path)
	{
		TextFile result;
		result.path = path;
		if (!Authorized(configuration, path)) { result.status = 403; result.error = "Save this instruction path in Shared setup before editing it."; return result; }
		result.error = "The instruction file is missing, unreadable or contains a symbolic link.";
		const std::filesystem::path requested = paths::PathFromUtf8(path);
		if (!provider_setup::NoSymlinks(requested)) return result;
		std::error_code ec;
		result.resolved = std::filesystem::canonical(requested, ec);
		if (ec || !provider_setup::NoSymlinks(result.resolved)) return result;
#if defined(_WIN32)
		std::filesystem::path native;
		if (!io::NativeFilePath(result.resolved, native, result.error)) return result;
		HANDLE handle = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (handle == INVALID_HANDLE_VALUE) return result;
		BY_HANDLE_FILE_INFORMATION before{};
		bool ok = GetFileType(handle) == FILE_TYPE_DISK && GetFileInformationByHandle(handle, &before) &&
		    (before.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0 && before.nFileSizeHigh == 0 && before.nFileSizeLow <= provider_setup::kMaxFileBytes;
		if (before.nFileSizeHigh != 0 || before.nFileSizeLow > provider_setup::kMaxFileBytes) result.error = "Instruction files are limited to 2 MiB.";
		if (ok)
		{
			result.content.resize(before.nFileSizeLow);
			DWORD count = 0; DWORD extra_count = 0; char extra = 0;
			BY_HANDLE_FILE_INFORMATION after{};
			ok = (result.content.empty() || (ReadFile(handle, result.content.data(), static_cast<DWORD>(result.content.size()), &count, nullptr) && count == result.content.size())) &&
			    ReadFile(handle, &extra, 1, &extra_count, nullptr) && extra_count == 0 && GetFileInformationByHandle(handle, &after) && detail::Revision(before) == detail::Revision(after);
			result.revision = detail::Revision(before);
		}
		CloseHandle(handle);
#else
		const int descriptor = detail::Open(result.resolved);
		if (descriptor < 0) return result;
		struct stat before{};
		bool ok = fstat(descriptor, &before) == 0 && S_ISREG(before.st_mode) && before.st_size >= 0 && static_cast<std::uintmax_t>(before.st_size) <= provider_setup::kMaxFileBytes;
		if (before.st_size > 0 && static_cast<std::uintmax_t>(before.st_size) > provider_setup::kMaxFileBytes) result.error = "Instruction files are limited to 2 MiB.";
		if (ok)
		{
			result.content.resize(static_cast<std::size_t>(before.st_size));
			std::size_t offset = 0;
			while (offset < result.content.size())
			{
				const ssize_t count = read(descriptor, result.content.data() + offset, result.content.size() - offset);
				if (count < 0 && errno == EINTR) continue;
				if (count <= 0) { ok = false; break; }
				offset += static_cast<std::size_t>(count);
			}
			char extra = 0; struct stat after{};
			ok = ok && read(descriptor, &extra, 1) == 0 && fstat(descriptor, &after) == 0 && detail::Revision(before) == detail::Revision(after);
			result.revision = detail::Revision(before);
		}
		close(descriptor);
#endif
		if (!ok || !provider_setup::NoSymlinks(requested)) { result.content.clear(); return result; }
		if (!ValidateText(result.content, result.error)) { result.content.clear(); return result; }
		result.ok = true;
		result.error.clear();
		return result;
	}

	struct PreparedSave
	{
		TextFile result;
		std::string expected_content;
		std::filesystem::path temporary;
		~PreparedSave() { if (!temporary.empty()) io::RemoveAtomicTempNoThrow(temporary); }
	};

	/// <summary>Stage and sync off the UI thread; no application state is accessed by this operation.</summary>
	inline std::shared_ptr<PreparedSave> PrepareSave(const CentralProviderConfiguration& configuration, const std::string& path,
	    const std::string& content, const std::string& expected_content, const io::AtomicWriteStageHook& hook = {})
	{
		const std::shared_ptr<PreparedSave> prepared = std::make_shared<PreparedSave>();
		prepared->result = Read(configuration, path);
		TextFile& result = prepared->result;
		if (!result.ok) return prepared;
		result.ok = false;
		if (!ValidateText(content, result.error) || !ValidateText(expected_content, result.error)) return prepared;
		if (result.content == content) { result.ok = true; prepared->expected_content = content; return prepared; }
		if (result.content != expected_content) { result.status = 409; result.error = "The instruction file changed on disk. Reload it before saving."; return prepared; }
		prepared->expected_content = expected_content;
		prepared->temporary = io::MakeTempWritePath(result.resolved);
		io::AtomicWriteResult written;
		const io::AtomicWriteStageHook preserve = [&](io::AtomicWriteStage stage)
		{
			if (hook && !hook(stage)) return false;
			if (stage != io::AtomicWriteStage::AfterFlush) return true;
#if defined(__APPLE__)
			const int source = detail::Open(result.resolved);
			const int destination = open(prepared->temporary.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
			struct stat original{}; struct stat staged{};
			const bool copied = source >= 0 && destination >= 0 && fcopyfile(source, destination, nullptr, COPYFILE_METADATA) == 0 &&
			    fstat(source, &original) == 0 && fstat(destination, &staged) == 0 && original.st_uid == staged.st_uid && original.st_gid == staged.st_gid &&
			    (original.st_mode & 07777) == (staged.st_mode & 07777) && futimens(destination, nullptr) == 0;
			if (source >= 0) close(source);
			if (destination >= 0) close(destination);
			if (!copied) result.error = "Could not preserve the instruction file's permissions and metadata.";
			return copied;
#elif defined(_WIN32)
			return true; // ReplaceFileW preserves the destination security descriptor and attributes.
#else
			result.error = "Instruction file editing is supported on macOS and Windows.";
			return false;
#endif
		};
		if (!io::WriteAndSyncAtomicTemp(prepared->temporary, content, preserve, written))
		{
			result.status = 500;
			if (result.error.empty()) result.error = "Could not stage the instruction file: " + written.error;
			return prepared;
		}
		result.content = content;
		result.ok = true;
		return prepared;
	}

	/// <summary>Call on the settings-owning UI thread: authorization and replacement occur in one event turn.</summary>
	inline TextFile CommitSave(const CentralProviderConfiguration& configuration, PreparedSave& prepared)
	{
		if (!prepared.result.ok) return prepared.result;
		TextFile current = Read(configuration, prepared.result.path);
		if (!current.ok) return current;
		if (current.content == prepared.result.content) return current;
		if (current.content != prepared.expected_content || current.resolved != prepared.result.resolved || current.revision != prepared.result.revision)
		{ current.ok = false; current.status = 409; current.error = "The instruction file changed on disk. Reload it before saving."; return current; }
		// shortcut: external writers can race the final check/rename, add a cooperative editor lock if cross-process compare-and-swap is required.
		const io::AtomicWriteResult committed = io::CommitSyncedAtomicTemp(current.resolved, prepared.temporary);
		if (!committed.success)
		{
			current.ok = false; current.status = 500;
			current.error = "Could not save the instruction file: " + committed.error;
			if (committed.stage == io::AtomicWriteStage::AfterReplaceBeforeDirectorySync)
			{
				// A failed directory sync leaves the old file at the temporary path for recovery.
				current.error = "The file was replaced but disk sync failed. Reload it before saving again. Recovery file: " + paths::Utf8PathString(prepared.temporary);
				prepared.temporary.clear();
			}
			return current;
		}
		prepared.temporary.clear();
		return prepared.result;
	}
}
