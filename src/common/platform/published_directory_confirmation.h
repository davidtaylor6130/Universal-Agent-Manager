#pragma once

#include "common/utils/io_utils.h"

#include <filesystem>
#include <string>

namespace uam::platform
{
	enum class PublishedDirectoryConfirmation { Confirmed, NotRequired, Failed };

	/// Additional directory confirmation after an acknowledged publication.
	/// macOS confirms its deferred directory fsync phase. Windows retains its
	/// existing atomic-replace contract with no additional phase here; other
	/// targets retain the same no-extra-phase policy. NotRequired is not a
	/// directory-sync success or a claim that OS guarantees are equivalent.
	/// Worker-only; exact already-admitted directories, no standalone flush,
	/// publication, rollback, path-discovery or file-access authority is granted.
	inline PublishedDirectoryConfirmation ConfirmPublishedDirectories(
	    const std::filesystem::path& primary_parent,
	    const std::filesystem::path& summary_parent,
	    const std::filesystem::path& data_root, std::string& error)
	{
		error.clear();
#if defined(__APPLE__)
		uam::io::AtomicWriteResult synced;
		if (!(uam::io::SyncAtomicWriteDirectory(primary_parent, synced) &&
		      uam::io::SyncAtomicWriteDirectory(summary_parent, synced) &&
		      uam::io::SyncAtomicWriteDirectory(data_root, synced)))
		{
			error = synced.error.empty() ? "Published directory confirmation failed." : synced.error;
			return PublishedDirectoryConfirmation::Failed;
		}
		return PublishedDirectoryConfirmation::Confirmed;
#else
		// In particular, Windows does not acquire a new directory-flush promise.
		// Linux headless helpers also retain their existing no-extra-phase policy.
		(void)primary_parent;
		(void)summary_parent;
		(void)data_root;
		return PublishedDirectoryConfirmation::NotRequired;
#endif
	}
}
