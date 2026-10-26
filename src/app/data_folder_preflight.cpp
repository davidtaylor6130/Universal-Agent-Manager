#include "data_folder_preflight.h"

#include "common/paths/path_utils.h"
#include "common/platform/published_file_change_witness.h"

#include <utility>

namespace fs = std::filesystem;

namespace
{
	uam::DataFolderPreflightResult Blocked(uam::DataFolderPreflightBlocker blocker, const std::string& explanation)
	{
		uam::DataFolderPreflightResult result;
		result.blocker = blocker;
		result.explanation = explanation;
		return result;
	}

	bool ResolveDirectory(fs::path path, fs::path& resolved)
	{
		if (path.empty() || !path.is_absolute() || path.native().find(fs::path::value_type{}) != fs::path::string_type::npos)
		{
			return false;
		}
		// Picker paths are explicit: do not lexically collapse '..' through a possible directory alias.
		for (const fs::path& component : path)
		{
			if (component == "." || component == "..")
			{
				return false;
			}
		}
		if (path.has_relative_path() && path.filename().empty())
		{
			path = path.parent_path();
		}
		std::error_code error;
		const fs::file_status status = fs::symlink_status(path, error);
		if (error || !fs::is_directory(status) || uam::paths::IsLinkOrReparsePointNoThrow(path))
		{
			return false;
		}
		resolved = fs::canonical(path, error);
		return !error && !resolved.empty() && resolved != resolved.root_path();
	}

	bool ContainsDirectory(const fs::path& root, fs::path candidate, std::error_code& error)
	{
		// Compare existing directory identities, including case aliases on macOS/Windows.
		while (!candidate.empty())
		{
			const bool same = fs::equivalent(root, candidate, error);
			if (error || same)
			{
				return same;
			}
			const fs::path parent = candidate.parent_path();
			if (parent == candidate)
			{
				break;
			}
			candidate = parent;
		}
		return false;
	}

	bool EmptyExceptNativeLock(const fs::path& root, std::error_code& error)
	{
		fs::directory_iterator cursor(root, error), end;
		// Native locks persist after release. Their marker is not profile data or proof
		// of ownership: only the later nonblocking OS lease may admit the destination.
		for (; !error && cursor != end; cursor.increment(error))
		{
			const auto path = cursor->path();
			const bool writers = path.filename() == ".uam-data-writers.lock";
			if (!writers && path.filename() != ".uam-data-root.lock")
				return false;
			const auto witness = uam::platform::PublishedFileChangeWitness::Capture(path);
			if (!witness)
				return false;
			const auto size = fs::file_size(path, error);
			if (error || !witness->Current())
				return false;
			// Canonical-record ReadBounded intentionally refuses empty data. These
			// reserved inert markers instead require an exact attribute witness.
			if (size == 0)
				continue;
			if (writers)
				return false;
			const auto marker = witness ? witness->ReadBounded(64) : std::nullopt;
			if (!marker || (!marker->empty() && (writers || marker->back() != '\n' || marker->size() <= 1 || marker->find_first_not_of("0123456789") != marker->size() - 1)))
				return false;
		}
		return !error;
	}
} // namespace

uam::DataFolderPreflightResult uam::InspectDataFolderTransition(const fs::path& active_root, const fs::path& selected_root, bool had_startup_environment_override, const std::vector<fs::path>& retained_roots, DataFolderDestinationCheck destination_check)
{
	using Blocker = DataFolderPreflightBlocker;
	if (had_startup_environment_override)
	{
		return Blocked(Blocker::StartupEnvironmentOverride, "UAM_DATA_DIR controls this launch. Remove the startup override before choosing a data folder in Settings.");
	}
	fs::path source;
	fs::path destination;
	if (!ResolveDirectory(active_root, source))
	{
		return Blocked(Blocker::InvalidSource, "The current data folder must be an existing absolute directory without dot components, a linked root or a filesystem root.");
	}
	if (!ResolveDirectory(selected_root, destination))
	{
		return Blocked(Blocker::InvalidDestination, "Choose an existing absolute directory without dot components, a linked root or a filesystem root.");
	}
	std::error_code error;
	std::vector<fs::path> protected_roots{source};
	protected_roots.insert(protected_roots.end(), retained_roots.begin(), retained_roots.end());
	for (const auto& retained : protected_roots)
	{
		fs::path root;
		if (!ResolveDirectory(retained, root))
			return Blocked(Blocker::InspectionFailed, "A retained data folder or bootstrap directory cannot be verified.");
		const bool destination_inside_root = ContainsDirectory(root, destination, error);
		if (error)
			return Blocked(Blocker::InspectionFailed, "Could not verify the data folders' directory identities.");
		const bool root_inside_destination = ContainsDirectory(destination, root, error);
		if (error)
			return Blocked(Blocker::InspectionFailed, "Could not verify the data folders' directory identities.");
		if (destination_inside_root || root_inside_destination)
			return Blocked(Blocker::OverlappingRoots, "Choose a separate folder outside the current, bootstrap and retained data folders and their parent directories.");
	}
	const bool empty = destination_check == DataFolderDestinationCheck::SeparationOnly || EmptyExceptNativeLock(destination, error);
	if (error)
	{
		return Blocked(Blocker::InspectionFailed, "Could not inspect the selected data folder.");
	}
	if (!empty)
	{
		return Blocked(Blocker::DestinationNotEmpty, "Choose an empty folder. Existing data folders cannot be merged or overwritten.");
	}
	DataFolderPreflightResult result;
	result.state = DataFolderPreflightState::RequiresStoppedValidation;
	result.source_root = std::move(source);
	result.destination_root = std::move(destination);
	result.explanation = "Folder paths inspected only. A stopped, exclusively owned and verified transition is still required before changing data folders.";
	return result;
}
