#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace uam
{
	enum class DataFolderPreflightState
	{
		Blocked,
		RequiresStoppedValidation
	};
	enum class DataFolderDestinationCheck
	{
		RequireEmpty,
		SeparationOnly // An owned destination's exclusively locked marker cannot be read on Windows.
	};

	enum class DataFolderPreflightBlocker
	{
		None,
		StartupEnvironmentOverride,
		InvalidSource,
		InvalidDestination,
		OverlappingRoots,
		DestinationNotEmpty,
		NetworkVolume,
		InspectionFailed
	};

	struct DataFolderPreflightResult
	{
		DataFolderPreflightState state = DataFolderPreflightState::Blocked;
		DataFolderPreflightBlocker blocker = DataFolderPreflightBlocker::None;
		std::filesystem::path source_root;
		std::filesystem::path destination_root;
		std::string explanation;
	};

	/// Read-only path admission for a future Settings transition; never changes routing or acquires locks.
	/// Both roots must already exist. Source contents are not traversed and destination emptiness is only an observation.
	/// RequiresStoppedValidation is NOT migration authority: quiesce writers, pin/revalidate both roots, acquire
	/// exclusive ownership, validate all records and publish durable rollback/restart receipts separately.
	/// The override flag must come from native startup before UAM exports its effective UAM_DATA_DIR, never the UI.
	/// Retained roots include the bootstrap/default directory and every frozen source/passive image.
	/// Admission excludes either direction of identity-based overlap with all of them.
	/// A destination on a volume that reports itself as networked is refused: the agreement is a local folder, and root
	/// ownership, directory identity and durable replacement are not established over network filesystems.
	DataFolderPreflightResult InspectDataFolderTransition(const std::filesystem::path& active_root, const std::filesystem::path& selected_root, bool had_startup_environment_override, const std::vector<std::filesystem::path>& retained_roots = {}, DataFolderDestinationCheck destination_check = DataFolderDestinationCheck::RequireEmpty);

	/// One plain line for a local folder whose volume cannot swap files atomically (for example exFAT), else empty.
	/// Saves there still work through the weaker durable replace documented in docs/engineering/durable-writes.md.
	std::string DataFolderVolumeWarning(const std::filesystem::path& folder);
} // namespace uam
