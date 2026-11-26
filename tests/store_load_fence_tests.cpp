#include "test_harness.h"

#include "app/resource_collection_service.h"

using namespace uam_test;

namespace
{
	ChatFolder FenceTestFolder(const std::string& id)
	{
		ChatFolder folder;
		folder.id = id;
		folder.title = "Workspace " + id;
		folder.directory = "/workspace/" + id;
		return folder;
	}

	ResourceCollection FenceTestCollection(const std::string& id)
	{
		ResourceCollection collection;
		collection.id = id;
		collection.name = "Collection " + id;
		return collection;
	}

	struct ReadFailureInjection
	{
		explicit ReadFailureInjection(const fs::path& path)
		{
			uam::io::t_read_failure_path_for_tests = path;
		}
		~ReadFailureInjection()
		{
			uam::io::t_read_failure_path_for_tests.clear();
		}
	};
} // namespace

UAM_TEST(TryReadTextFileStatusSeparatesMissingEmptyAndUnreadable)
{
	TempDir temp("uam-read-status");
	std::string text = "stale";
	UAM_ASSERT(uam::io::TryReadTextFileStatus(temp.root / "absent.txt", text) == uam::io::ReadFileStatus::Missing);
	UAM_ASSERT(text.empty());

	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "empty.txt", ""));
	UAM_ASSERT(uam::io::TryReadTextFileStatus(temp.root / "empty.txt", text) == uam::io::ReadFileStatus::Read);
	UAM_ASSERT(text.empty());

	// Something that exists but cannot be read as a file is an error, never an empty file.
	fs::create_directories(temp.root / "directory.txt");
	UAM_ASSERT(uam::io::TryReadTextFileStatus(temp.root / "directory.txt", text) == uam::io::ReadFileStatus::Error);

	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "data.txt", "payload"));
	{
		ReadFailureInjection failure(temp.root / "data.txt");
		UAM_ASSERT(uam::io::TryReadTextFileStatus(temp.root / "data.txt", text) == uam::io::ReadFileStatus::Error);
		UAM_ASSERT(text.empty());
	}
	UAM_ASSERT(uam::io::TryReadTextFileStatus(temp.root / "data.txt", text) == uam::io::ReadFileStatus::Read);
	UAM_ASSERT_EQ(text, std::string("payload"));
}

UAM_TEST(FolderStoreInvalidPrimaryWithoutBackupIsNotOverwrittenByNextSave)
{
	TempDir temp("uam-folder-fence-invalid");
	const fs::path file = temp.root / "folders.txt";
	// A modern file cut off before its completion marker: one workspace readable, the rest lost.
	const std::string damaged = "folders_format_version=1\n\n[folder]\nid=kept\ntitle=Kept\n\n[folder]\nid=second\ntit";
	UAM_ASSERT(uam::io::WriteTextFile(file, damaged));
	UAM_ASSERT(!fs::exists(uam::io::MakeBackupPath(file)));

	uam::io::StoreLoadResult result;
	const std::vector<ChatFolder> loaded = ChatFolderStore::Load(temp.root, &result, uam::io::StoreFenceMode::Arm);
	UAM_ASSERT(result.status == uam::io::StoreLoadStatus::Unrecovered);
	UAM_ASSERT(!result.warning.empty());
	UAM_ASSERT(result.warning.find('\n') == std::string::npos);
	UAM_ASSERT(!loaded.empty()); // Best-effort display only.
	UAM_ASSERT(ChatFolderStore::IsSaveFenced(temp.root));

	// Every save path refuses, so the partial list cannot replace the bytes on disk.
	UAM_ASSERT(!ChatFolderStore::Save(temp.root, loaded));
	UAM_ASSERT(!ChatFolderStore::Save(temp.root, {}));
	UAM_ASSERT(ChatFolderStore::PrepareSave(temp.root, loaded) == nullptr);
	UAM_ASSERT_EQ(ReadFile(file), damaged);
	UAM_ASSERT(!fs::exists(uam::io::MakeBackupPath(file)));

	// A different data root is unaffected.
	TempDir other("uam-folder-fence-other");
	UAM_ASSERT(!ChatFolderStore::IsSaveFenced(other.root));
	UAM_ASSERT(ChatFolderStore::Save(other.root, {FenceTestFolder("elsewhere")}));
}

UAM_TEST(FolderStoreReadErrorIsNotTreatedAsEmpty)
{
	TempDir temp("uam-folder-fence-io");
	const fs::path file = temp.root / "folders.txt";
	UAM_ASSERT(ChatFolderStore::Save(temp.root, {FenceTestFolder("a"), FenceTestFolder("b")}));
	std::error_code ec;
	fs::remove(uam::io::MakeBackupPath(file), ec);
	const std::string saved = ReadFile(file);

	uam::io::StoreLoadResult result;
	{
		ReadFailureInjection failure(file);
		const std::vector<ChatFolder> loaded = ChatFolderStore::Load(temp.root, &result, uam::io::StoreFenceMode::Arm);
		UAM_ASSERT(loaded.empty());
	}
	UAM_ASSERT(result.status == uam::io::StoreLoadStatus::Unrecovered);
	UAM_ASSERT(result.status != uam::io::StoreLoadStatus::Missing);
	UAM_ASSERT(ChatFolderStore::IsSaveFenced(temp.root));
	// The empty in-memory list from the failed read must not reach disk, even once reads work again.
	UAM_ASSERT(!ChatFolderStore::Save(temp.root, {}));
	UAM_ASSERT_EQ(ReadFile(file), saved);
	UAM_ASSERT_EQ(ChatFolderStore::Load(temp.root).size(), std::size_t{2});
}

UAM_TEST(FolderStoreValidBackupStillRecoversAndSaves)
{
	TempDir temp("uam-folder-fence-backup");
	const fs::path file = temp.root / "folders.txt";
	UAM_ASSERT(ChatFolderStore::Save(temp.root, {FenceTestFolder("first")}));
	UAM_ASSERT(ChatFolderStore::Save(temp.root, {FenceTestFolder("first"), FenceTestFolder("second")}));
	UAM_ASSERT(fs::exists(uam::io::MakeBackupPath(file)));
	UAM_ASSERT(uam::io::WriteTextFile(file, "not a folder file"));

	uam::io::StoreLoadResult result;
	const std::vector<ChatFolder> recovered = ChatFolderStore::Load(temp.root, &result, uam::io::StoreFenceMode::Arm);
	UAM_ASSERT(result.status == uam::io::StoreLoadStatus::RecoveredFromBackup);
	UAM_ASSERT_EQ(recovered.size(), std::size_t{1});
	UAM_ASSERT_EQ(recovered.front().id, std::string("first"));
	UAM_ASSERT(!ChatFolderStore::IsSaveFenced(temp.root));

	// The primary read failing still recovers from a valid backup.
	{
		ReadFailureInjection failure(file);
		UAM_ASSERT_EQ(ChatFolderStore::Load(temp.root, &result, uam::io::StoreFenceMode::Arm).size(), std::size_t{1});
	}
	UAM_ASSERT(result.status == uam::io::StoreLoadStatus::RecoveredFromBackup);
	UAM_ASSERT(!ChatFolderStore::IsSaveFenced(temp.root));

	// Recovered data is saved normally and becomes the valid primary.
	UAM_ASSERT(ChatFolderStore::Save(temp.root, recovered));
	UAM_ASSERT(ChatFolderStore::Load(temp.root, &result).front().id == "first");
	UAM_ASSERT(result.status == uam::io::StoreLoadStatus::Valid);
}

UAM_TEST(FolderStoreMissingOrBlankFilesLoadEmptyWithoutFence)
{
	TempDir temp("uam-folder-fence-empty");
	uam::io::StoreLoadResult result;
	UAM_ASSERT(ChatFolderStore::Load(temp.root, &result, uam::io::StoreFenceMode::Arm).empty());
	UAM_ASSERT(result.status == uam::io::StoreLoadStatus::Missing);
	UAM_ASSERT(result.warning.empty());

	// Early releases wrote an empty folders.txt for no workspaces; it holds nothing to protect.
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "folders.txt", ""));
	UAM_ASSERT(ChatFolderStore::Load(temp.root, &result, uam::io::StoreFenceMode::Arm).empty());
	UAM_ASSERT(result.status == uam::io::StoreLoadStatus::Valid);
	UAM_ASSERT(!ChatFolderStore::IsSaveFenced(temp.root));
	UAM_ASSERT(ChatFolderStore::Save(temp.root, {FenceTestFolder("new")}));
	UAM_ASSERT_EQ(ChatFolderStore::Load(temp.root).size(), std::size_t{1});
}

UAM_TEST(ResourceCollectionInvalidPrimaryWithoutBackupIsNotOverwrittenByNextSave)
{
	TempDir temp("uam-collection-fence-invalid");
	const fs::path file = temp.root / "resource_collections.json";
	const std::string damaged = "{\"version\":1,\"collections\":[{\"id\":\"kept\",\"name\":\"Ke";
	UAM_ASSERT(uam::io::WriteTextFile(file, damaged));

	uam::io::StoreLoadResult result;
	uam::AppState app;
	app.data_root = temp.root;
	app.resource_collections = uam::ResourceCollectionService::Load(temp.root, &result, uam::io::StoreFenceMode::Arm);
	UAM_ASSERT(result.status == uam::io::StoreLoadStatus::Unrecovered);
	UAM_ASSERT(app.resource_collections.empty());
	UAM_ASSERT(uam::ResourceCollectionService::IsSaveFenced(temp.root));

	UAM_ASSERT(!uam::ResourceCollectionService::Save(temp.root, {}));
	std::string error;
	UAM_ASSERT(!uam::ResourceCollectionService::Create(app, "New", nullptr, &error));
	UAM_ASSERT(app.resource_collections.empty());
	UAM_ASSERT_EQ(ReadFile(file), damaged);
	UAM_ASSERT(!fs::exists(uam::io::MakeBackupPath(file)));
}

UAM_TEST(ResourceCollectionReadErrorFencesAndValidBackupRecovers)
{
	TempDir temp("uam-collection-fence-io");
	const fs::path file = temp.root / "resource_collections.json";
	UAM_ASSERT(uam::ResourceCollectionService::Save(temp.root, {FenceTestCollection("one")}));
	UAM_ASSERT(uam::ResourceCollectionService::Save(temp.root, {FenceTestCollection("one"), FenceTestCollection("two")}));

	// A valid backup still recovers when the primary is damaged or unreadable.
	uam::io::StoreLoadResult result;
	{
		ReadFailureInjection failure(file);
		UAM_ASSERT_EQ(uam::ResourceCollectionService::Load(temp.root, &result, uam::io::StoreFenceMode::Arm).size(), std::size_t{1});
	}
	UAM_ASSERT(result.status == uam::io::StoreLoadStatus::RecoveredFromBackup);
	UAM_ASSERT(!uam::ResourceCollectionService::IsSaveFenced(temp.root));

	// Without a backup, the same read error fences rather than loading an empty list.
	std::error_code ec;
	fs::remove(uam::io::MakeBackupPath(file), ec);
	const std::string saved = ReadFile(file);
	{
		ReadFailureInjection failure(file);
		UAM_ASSERT(uam::ResourceCollectionService::Load(temp.root, &result, uam::io::StoreFenceMode::Arm).empty());
	}
	UAM_ASSERT(result.status == uam::io::StoreLoadStatus::Unrecovered);
	UAM_ASSERT(!uam::ResourceCollectionService::Save(temp.root, {}));
	UAM_ASSERT_EQ(ReadFile(file), saved);
}

UAM_TEST(StoreLoadWithoutArmReportsButDoesNotFence)
{
	TempDir temp("uam-store-fence-observe");
	const std::string damaged = "folders_format_version=1\n\n[folder]\nid=partial\n";
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "folders.txt", damaged));
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "resource_collections.json", "{"));

	// A passing read (legacy import, staging, read-only checks) reports damage without owning the fence.
	uam::io::StoreLoadResult result;
	(void)ChatFolderStore::Load(temp.root, &result);
	UAM_ASSERT(result.Unrecovered());
	UAM_ASSERT(!ChatFolderStore::IsSaveFenced(temp.root));
	(void)uam::ResourceCollectionService::Load(temp.root, &result);
	UAM_ASSERT(result.Unrecovered());
	UAM_ASSERT(!uam::ResourceCollectionService::IsSaveFenced(temp.root));
}

UAM_TEST(FolderStoreInvalidBackupWithoutUsablePrimaryIsUnrecovered)
{
	TempDir temp("uam-folder-fence-bad-backup");
	const fs::path file = temp.root / "folders.txt";
	const fs::path backup = uam::io::MakeBackupPath(file);
	UAM_ASSERT(uam::io::WriteTextFile(backup, "damaged backup"));

	// Primary missing, backup holds unvalidated bytes.
	uam::io::StoreLoadResult result;
	UAM_ASSERT(ChatFolderStore::Load(temp.root, &result, uam::io::StoreFenceMode::Arm).empty());
	UAM_ASSERT(result.Unrecovered());
	UAM_ASSERT(!ChatFolderStore::Save(temp.root, {FenceTestFolder("new")}));
	UAM_ASSERT_EQ(ReadFile(backup), std::string("damaged backup"));
	UAM_ASSERT(!fs::exists(file));

	// A blank primary beside a damaged backup is still unrecovered.
	TempDir blank("uam-folder-fence-blank-bad-backup");
	UAM_ASSERT(uam::io::WriteTextFile(blank.root / "folders.txt", ""));
	UAM_ASSERT(uam::io::WriteTextFile(uam::io::MakeBackupPath(blank.root / "folders.txt"), "damaged backup"));
	(void)ChatFolderStore::Load(blank.root, &result);
	UAM_ASSERT(result.Unrecovered());
}

UAM_TEST(FolderStorePreparedSaveIsNotPublishedOnceFenced)
{
	TempDir temp("uam-folder-fence-prepared");
	const fs::path file = temp.root / "folders.txt";
	UAM_ASSERT(ChatFolderStore::Save(temp.root, {FenceTestFolder("a")}));
	const std::shared_ptr<PreparedFolderSave> prepared = ChatFolderStore::PrepareSave(temp.root, {});
	UAM_ASSERT(prepared != nullptr);

	// The store becomes fenced between preparation and publication.
	UAM_ASSERT(uam::io::WriteTextFile(file, "damaged"));
	std::error_code ec;
	fs::remove(uam::io::MakeBackupPath(file), ec);
	uam::io::StoreLoadResult result;
	(void)ChatFolderStore::Load(temp.root, &result, uam::io::StoreFenceMode::Arm);
	UAM_ASSERT(result.Unrecovered());
	UAM_ASSERT(!ChatFolderStore::PublishPreparedSave(prepared));
	UAM_ASSERT_EQ(ReadFile(file), std::string("damaged"));
}
