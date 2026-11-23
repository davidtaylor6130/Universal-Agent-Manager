#pragma once

#include "common/paths/path_utils.h"
#include "common/utils/io_utils.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>

namespace uam::io
{
	/// How a primary-plus-.bak store load ended.
	enum class StoreLoadStatus
	{
		Missing,             ///< Neither the primary nor its backup exists; empty data is authoritative.
		Valid,               ///< The primary validated.
		RecoveredFromBackup, ///< The primary was missing, unreadable or invalid; the validated .bak was used.
		Unrecovered,         ///< Bytes exist that could not be read or validated and no valid .bak replaced them.
	};

	struct StoreLoadResult
	{
		StoreLoadStatus status = StoreLoadStatus::Missing;
		/// One plain line for the user; empty unless the load recovered or failed.
		std::string warning;
		[[nodiscard]] bool Unrecovered() const
		{
			return status == StoreLoadStatus::Unrecovered;
		}
	};

	/// Process-wide save fence for stores whose last load could not be recovered. While a store
	/// file is fenced, its owning store refuses every save, so partial or empty in-memory state
	/// can never replace bytes UAM failed to read. The fence ends with the process: a restart
	/// after the user repairs or removes the file performs a fresh, valid read.
	class UnrecoveredStoreFence
	{
	  public:
		static void Mark(const std::filesystem::path& file)
		{
			std::lock_guard<std::mutex> lock(Mutex());
			Files().insert(Key(file));
		}

		static bool IsFenced(const std::filesystem::path& file)
		{
			std::lock_guard<std::mutex> lock(Mutex());
			return Files().contains(Key(file));
		}

	  private:
		static std::string Key(const std::filesystem::path& file)
		{
			// UTF-8, never the narrow code page: a non-ANSI character in the data root must not throw on Windows.
			return uam::paths::NormalizedPortablePathString(file);
		}

		static std::mutex& Mutex()
		{
			static std::mutex mutex;
			return mutex;
		}

		static std::unordered_set<std::string>& Files()
		{
			static std::unordered_set<std::string> files;
			return files;
		}
	};

	/// Whether a load arms the save fence. Only a load whose result becomes the live saved state
	/// (startup, startup deletion recovery) arms it; a passing read or a staging/legacy root only reports.
	enum class StoreFenceMode
	{
		Observe,
		Arm,
	};

	/// Blank text holds no data to protect: an empty legacy file is an empty store, not damage.
	inline bool IsBlankStoreText(const std::string& text)
	{
		return text.find_first_not_of(" \t\r\n") == std::string::npos;
	}

	/// Reads `file`, falling back to its validated .bak, and classifies the outcome. `valid` decides
	/// whether text is a complete store file. On Valid or RecoveredFromBackup, `text_out` holds the
	/// validated text; on Unrecovered it holds whatever primary bytes were read (possibly none) for a
	/// best-effort display, and with StoreFenceMode::Arm the file is fenced against saves. `label` names the store in the warning.
	template <typename Validate> StoreLoadResult LoadStoreTextWithBackup(const std::filesystem::path& file, Validate&& valid, std::string& text_out, const std::string& label, StoreFenceMode fence = StoreFenceMode::Observe)
	{
		StoreLoadResult result;
		text_out.clear();
		std::string primary;
		const ReadFileStatus primary_status = TryReadTextFileStatus(file, primary);
		if (primary_status == ReadFileStatus::Read && valid(primary))
		{
			result.status = StoreLoadStatus::Valid;
			text_out = std::move(primary);
			return result;
		}
		std::string backup;
		const ReadFileStatus backup_status = TryReadTextFileStatus(MakeBackupPath(file), backup);
		if (backup_status == ReadFileStatus::Read && valid(backup))
		{
			result.status = StoreLoadStatus::RecoveredFromBackup;
			result.warning = "Recovered " + label + " from the validated backup because the primary file was missing, unreadable or invalid.";
			text_out = std::move(backup);
			return result;
		}
		const auto holds_unvalidated_bytes = [](ReadFileStatus status, const std::string& text) { return status == ReadFileStatus::Error || (status == ReadFileStatus::Read && !IsBlankStoreText(text)); };
		if (holds_unvalidated_bytes(primary_status, primary) || holds_unvalidated_bytes(backup_status, backup))
		{
			result.status = StoreLoadStatus::Unrecovered;
			result.warning = "Could not load " + label + ": " + file.filename().string() + " is unreadable or invalid and has no valid backup. UAM will not save " + label + " until the file is repaired (or removed with its .bak) and UAM restarts.";
			if (fence == StoreFenceMode::Arm)
				UnrecoveredStoreFence::Mark(file);
			if (primary_status == ReadFileStatus::Read)
				text_out = std::move(primary);
			return result;
		}
		// Nothing readable holds data: absent files, or blank legacy files with no valid backup.
		result.status = primary_status == ReadFileStatus::Missing && backup_status == ReadFileStatus::Missing ? StoreLoadStatus::Missing : StoreLoadStatus::Valid;
		return result;
	}
} // namespace uam::io
