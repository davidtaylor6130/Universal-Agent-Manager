#include "data_folder_test_support.h"
#include "app/data_folder_preflight.h"

using namespace uam_test;
using PreflightState = uam::DataFolderPreflightState;
using Blocker = uam::DataFolderPreflightBlocker;

UAM_TEST(DataFolderPreflightPreservesAuthorityWithoutMigration)
{
	TempDir temp("uam-data-folder-read-only");
	const fs::path source = temp.root / "source";
	const fs::path destination = temp.root / "destination";
	fs::create_directories(destination);
	const std::vector<fs::path> records = {"chats/chat.json", "chats/chat.json.bak", "chat-summaries/chat.json", "queue-outboxes/chat.json", "memory/knowledge.md", "agent-runs/run.json", "usage/ledger.json", "recovery/receipt.json", "settings.txt", "folders.txt", "resource_collections.json", ".uam-data-root.lock"};
	for (const fs::path& relative : records)
	{
		fs::create_directories((source / relative).parent_path());
		UAM_ASSERT(uam::io::WriteTextFile(source / relative, "disposable owned record"));
	}
	std::vector<fs::path> entries_before;
	for (const auto& entry : fs::recursive_directory_iterator(source))
	{
		entries_before.push_back(entry.path());
	}
	std::sort(entries_before.begin(), entries_before.end());
	const auto result = uam::InspectDataFolderTransition(source, destination, false);
	UAM_ASSERT(result.state == PreflightState::RequiresStoppedValidation);
	UAM_ASSERT(result.blocker == Blocker::None);
	UAM_ASSERT(result.source_root == fs::canonical(source));
	UAM_ASSERT(result.destination_root == fs::canonical(destination));
	UAM_ASSERT(fs::is_empty(destination));
	for (const fs::path& relative : records)
	{
		UAM_ASSERT_EQ(uam::io::ReadTextFile(source / relative), "disposable owned record");
	}
	std::vector<fs::path> entries_after;
	for (const auto& entry : fs::recursive_directory_iterator(source))
	{
		entries_after.push_back(entry.path());
	}
	std::sort(entries_after.begin(), entries_after.end());
	UAM_ASSERT(entries_before == entries_after);
}

UAM_TEST(DataFolderPreflightHonorsStartupOverrideBeforeInspectingPaths)
{
	const auto result = uam::InspectDataFolderTransition({}, {}, true);
	UAM_ASSERT(result.state == PreflightState::Blocked);
	UAM_ASSERT(result.blocker == Blocker::StartupEnvironmentOverride);
	UAM_ASSERT(result.source_root.empty() && result.destination_root.empty());
}

UAM_TEST(DataFolderPreflightRejectsInvalidPathsWithoutCreatingFolders)
{
	TempDir temp("uam-data-folder-invalid");
	const fs::path source = temp.root / "source";
	const fs::path destination = temp.root / "destination";
	fs::create_directory(source);
	fs::create_directory(destination);
	UAM_ASSERT(uam::InspectDataFolderTransition({}, destination, false).blocker == Blocker::InvalidSource);
	UAM_ASSERT(uam::InspectDataFolderTransition(temp.root / "missing-source", destination, false).blocker == Blocker::InvalidSource);
	const std::vector<fs::path> invalid = {{}, "relative-data", temp.root / "missing-destination", source.root_path(), destination / ".", destination / ".."};
	for (const fs::path& selected : invalid)
	{
		const auto result = uam::InspectDataFolderTransition(source, selected, false);
		UAM_ASSERT(result.state == PreflightState::Blocked);
		UAM_ASSERT(result.blocker == Blocker::InvalidDestination);
	}
	UAM_ASSERT(!fs::exists(temp.root / "missing-source"));
	UAM_ASSERT(!fs::exists(temp.root / "missing-destination"));
	UAM_ASSERT(fs::is_empty(source) && fs::is_empty(destination));
}

UAM_TEST(DataFolderPreflightRejectsSameNestedAndParentRoots)
{
	TempDir temp("uam-data-folder-overlap");
	const fs::path source = temp.root / "source";
	const fs::path nested = source / "nested";
	fs::create_directories(nested);
	for (const fs::path& selected : {source, nested, temp.root})
	{
		const auto result = uam::InspectDataFolderTransition(source, selected, false);
		UAM_ASSERT(result.state == PreflightState::Blocked);
		UAM_ASSERT(result.blocker == Blocker::OverlappingRoots);
	}
	const fs::path sibling_with_prefix = temp.root / "source-other";
	fs::create_directory(sibling_with_prefix);
	UAM_ASSERT(uam::InspectDataFolderTransition(source, sibling_with_prefix, false).state == PreflightState::RequiresStoppedValidation);
}

UAM_TEST(DataFolderPreflightRejectsOccupiedAndChangedDestinations)
{
	TempDir temp("uam-data-folder-occupied");
	const fs::path source = temp.root / "source";
	const fs::path destination = temp.root / "destination";
	fs::create_directory(source);
	fs::create_directory(destination);
	UAM_ASSERT(uam::InspectDataFolderTransition(source, destination, false).state == PreflightState::RequiresStoppedValidation);
	for (const fs::path& relative : {fs::path("chats/chat.json"), fs::path(".uam-data-root.lock"), fs::path("unknown-user-file")})
	{
		fs::remove_all(destination);
		fs::create_directory(destination);
		fs::create_directories((destination / relative).parent_path());
		UAM_ASSERT(uam::io::WriteTextFile(destination / relative, "foreign survivor"));
		const auto result = uam::InspectDataFolderTransition(source, destination, false);
		UAM_ASSERT(result.state == PreflightState::Blocked);
		UAM_ASSERT(result.blocker == Blocker::DestinationNotEmpty);
		UAM_ASSERT_EQ(uam::io::ReadTextFile(destination / relative), "foreign survivor");
	}
	fs::remove_all(destination);
	UAM_ASSERT(uam::io::WriteTextFile(destination, "replacement file"));
	UAM_ASSERT(uam::InspectDataFolderTransition(source, destination, false).blocker == Blocker::InvalidDestination);
	UAM_ASSERT_EQ(uam::io::ReadTextFile(destination), "replacement file");
	UAM_ASSERT(fs::is_empty(source));
}

#if !defined(_WIN32)
UAM_TEST(DataFolderPreflightRejectsLinkedRootsAndResolvesAncestorAliases)
{
	TempDir temp("uam-data-folder-alias");
	const fs::path source = temp.root / "source";
	const fs::path destination = temp.root / "destination";
	fs::create_directories(source / "nested");
	fs::create_directory(destination);
	fs::create_directory_symlink(destination, temp.root / "linked-destination");
	fs::create_directory_symlink(source, temp.root / "linked-source");
	fs::create_directory_symlink(temp.root / "missing", temp.root / "dangling");
	for (const fs::path& selected : {temp.root / "linked-destination", temp.root / "linked-destination" / "", temp.root / "dangling"})
	{
		UAM_ASSERT(uam::InspectDataFolderTransition(source, selected, false).blocker == Blocker::InvalidDestination);
	}
	UAM_ASSERT(uam::InspectDataFolderTransition(temp.root / "linked-source", destination, false).blocker == Blocker::InvalidSource);
	UAM_ASSERT(uam::InspectDataFolderTransition(source, temp.root / "linked-source" / "nested", false).blocker == Blocker::OverlappingRoots);
	UAM_ASSERT(fs::is_empty(destination));
	UAM_ASSERT(fs::is_symlink(temp.root / "linked-source") && fs::is_symlink(temp.root / "linked-destination"));
}
#endif

UAM_TEST(DataFolderPreflightAdmitsOnlyAnInertNativeLockMarker)
{
	TempDir temp("uam-data-folder-stale-lock");
	const auto source = temp.root / "source", destination = temp.root / "destination";
	fs::create_directories(source);
	fs::create_directories(destination);
	const auto lock = destination / ".uam-data-root.lock";
	UAM_ASSERT(uam::io::WriteTextFile(lock, "12345\n"));
	UAM_ASSERT(uam::InspectDataFolderTransition(source, destination, false).state == PreflightState::RequiresStoppedValidation);
	UAM_ASSERT_EQ(ReadFile(lock), "12345\n");
	const auto writers = destination / ".uam-data-writers.lock";
	UAM_ASSERT(uam::io::WriteTextFile(writers, ""));
	UAM_ASSERT(uam::InspectDataFolderTransition(source, destination, false).state == PreflightState::RequiresStoppedValidation);
	UAM_ASSERT(uam::io::WriteTextFile(writers, "foreign"));
	UAM_ASSERT(uam::InspectDataFolderTransition(source, destination, false).state == PreflightState::Blocked);
	UAM_ASSERT(uam::io::WriteTextFile(writers, ""));
	UAM_ASSERT(uam::io::WriteTextFile(lock, "foreign authority survivor"));
	UAM_ASSERT(uam::InspectDataFolderTransition(source, destination, false).state == PreflightState::Blocked);
	UAM_ASSERT_EQ(ReadFile(lock), "foreign authority survivor");
}

UAM_TEST(DataFolderPreflightExcludesRetainedAncestorsAndDirectoryAliases)
{
	TempDir temp("uam-data-folder-retained-identities");
	const auto active = temp.root / "active", retained = temp.root / "history/retained", bootstrap = temp.root / "bootstrap";
	fs::create_directories(active);
	fs::create_directories(retained / "empty");
	fs::create_directories(bootstrap / ".data-folder-control/empty");
	for (const auto& root : {retained, bootstrap})
	{
		const auto child = root == retained ? root / "empty" : root / ".data-folder-control/empty";
		for (const auto& selected : {root, child, root.parent_path()})
			UAM_ASSERT(uam::InspectDataFolderTransition(active, selected, false, {root}).blocker == Blocker::OverlappingRoots);
	}
	// An ancestor of only the historical root must still refuse; the active root
	// is a sibling elsewhere, so this is not the existing active-source check.
	UAM_ASSERT(uam::InspectDataFolderTransition(active, retained.parent_path(), false).blocker == Blocker::DestinationNotEmpty);
	const auto prefix_sibling = temp.root / "history/retained-other";
	fs::create_directory(prefix_sibling);
	UAM_ASSERT(uam::InspectDataFolderTransition(active, prefix_sibling, false, {retained, bootstrap}).state == PreflightState::RequiresStoppedValidation);
#if !defined(_WIN32)
	fs::create_directory_symlink(retained.parent_path(), temp.root / "history-alias");
	UAM_ASSERT(uam::InspectDataFolderTransition(active, temp.root / "history-alias/retained/empty", false, {retained}).blocker == Blocker::OverlappingRoots);
	// On case-insensitive volumes, spelling changes must preserve identity semantics.
	const auto case_alias = temp.root / "HISTORY/RETAINED/empty";
	std::error_code error;
	if (fs::exists(case_alias, error) && !error)
		UAM_ASSERT(uam::InspectDataFolderTransition(active, case_alias, false, {retained}).blocker == Blocker::OverlappingRoots);
#endif
	UAM_ASSERT(fs::is_empty(retained / "empty") && fs::is_empty(bootstrap / ".data-folder-control/empty"));
}
