#include "test_harness.h"

#include "app/resource_collection_service.h"

#include <tuple>

using namespace uam_test;

namespace
{
	uam::AppState ResourceTestApp(const fs::path& data_root)
	{
		uam::AppState app;
		app.data_root = data_root;
		ChatFolder folder;
		folder.id = "workspace-1";
		folder.title = "Workspace";
		folder.directory = data_root.string();
		app.folders.push_back(std::move(folder));
		ChatSession chat;
		chat.id = "chat-1";
		app.chats.push_back(std::move(chat));
		return app;
	}
} // namespace

UAM_TEST(ResourceCollectionsPersistAllReferenceTypesAndOrdering)
{
	TempDir temp("uam-resource-collections");
	uam::AppState app = ResourceTestApp(temp.root);
	std::string error;
	ResourceCollection first;
	ResourceCollection second;
	UAM_ASSERT(uam::ResourceCollectionService::Create(app, " First ", &first, &error));
	UAM_ASSERT(uam::ResourceCollectionService::Create(app, "Second", &second, &error));
	UAM_ASSERT_EQ(first.name, std::string("First"));
	UAM_ASSERT(uam::ResourceCollectionService::Rename(app, first.id, "Resources", &error));
	UAM_ASSERT(uam::ResourceCollectionService::ToggleCollapsed(app, first.id, &error));

	std::vector<ResourceReference> references;
	const std::vector<std::tuple<std::string, std::string, std::string>> inputs = {
	    {"workspace-folder", "workspace-1", "Workspace"},
	    {"chat", "chat-1", "Chat"},
	    {"file", "/tmp/readme.md", "Readme"},
	    {"website", "https://example.com", "Example"},
	    {"desktop-app", "com.example.Editor", "Editor"},
	};
	for (const auto& [type, target, label] : inputs)
	{
		ResourceReference created;
		UAM_ASSERT(uam::ResourceCollectionService::AddReference(app, first.id, type, target, label, &created, &error));
		references.push_back(std::move(created));
	}

	std::vector<std::string> reversed_reference_ids;
	for (auto found = references.rbegin(); found != references.rend(); ++found)
	{
		reversed_reference_ids.push_back(found->id);
	}
	UAM_ASSERT(uam::ResourceCollectionService::ReorderReferences(app, first.id, reversed_reference_ids, &error));
	UAM_ASSERT(uam::ResourceCollectionService::ReorderCollections(app, {second.id, first.id}, &error));

	const std::vector<ResourceCollection> loaded = uam::ResourceCollectionService::Load(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(loaded[0].id, second.id);
	UAM_ASSERT_EQ(loaded[1].name, std::string("Resources"));
	UAM_ASSERT(loaded[1].collapsed);
	UAM_ASSERT_EQ(loaded[1].references.size(), inputs.size());
	UAM_ASSERT_EQ(loaded[1].references[0].type, std::string("desktop-app"));
	UAM_ASSERT_EQ(loaded[1].references[4].type, std::string("workspace-folder"));

	app.resource_collections = loaded;
	const nlohmann::json state = uam::StateSerializer::Serialize(app);
	UAM_ASSERT_EQ(state["resourceCollections"].size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(state["resourceCollections"][1]["references"][0]["target"].get<std::string>(), std::string("com.example.Editor"));
	UAM_ASSERT(uam::StateSerializer::SerializeFingerprint(app).contains("resourceCollections"));
}

UAM_TEST(ResourceCollectionsRejectInvalidAndDuplicateReferences)
{
	TempDir temp("uam-resource-validation");
	uam::AppState app = ResourceTestApp(temp.root);
	std::string error;
	ResourceCollection collection;
	UAM_ASSERT(!uam::ResourceCollectionService::Create(app, "   ", nullptr, &error));
	UAM_ASSERT(uam::ResourceCollectionService::Create(app, "Resources", &collection, &error));
	UAM_ASSERT(!uam::ResourceCollectionService::AddReference(app, collection.id, "unknown", "value", "", nullptr, &error));
	UAM_ASSERT(!uam::ResourceCollectionService::AddReference(app, collection.id, "workspace-folder", "missing", "", nullptr, &error));
	UAM_ASSERT(!uam::ResourceCollectionService::AddReference(app, collection.id, "chat", "missing", "", nullptr, &error));
	UAM_ASSERT(!uam::ResourceCollectionService::AddReference(app, collection.id, "website", "ftp://example.com", "", nullptr, &error));

	ResourceReference reference;
	UAM_ASSERT(uam::ResourceCollectionService::AddReference(app, collection.id, "file", "/tmp/item", "Item", &reference, &error));
	UAM_ASSERT(!uam::ResourceCollectionService::AddReference(app, collection.id, "file", "/tmp/item", "Duplicate", nullptr, &error));
	UAM_ASSERT(!uam::ResourceCollectionService::ReorderReferences(app, collection.id, {}, &error));
	UAM_ASSERT(!uam::ResourceCollectionService::ReorderCollections(app, {}, &error));
	UAM_ASSERT_EQ(app.resource_collections[0].references.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(uam::ResourceCollectionService::RemoveReference(app, collection.id, reference.id, &error));
	UAM_ASSERT(uam::ResourceCollectionService::Delete(app, collection.id, &error));
	UAM_ASSERT(app.resource_collections.empty());
}

UAM_TEST(ResourceCollectionsRecoverValidatedBackupWhenPrimaryIsMissingOrCorrupt)
{
	for (const bool missing_primary : {false, true})
	{
		TempDir temp(std::string("uam-resource-backup-") + (missing_primary ? "missing" : "corrupt"));
		uam::AppState app = ResourceTestApp(temp.root);
		std::string error;
		ResourceCollection collection;
		UAM_ASSERT(uam::ResourceCollectionService::Create(app, "Recovered", &collection, &error));
		UAM_ASSERT(uam::ResourceCollectionService::Rename(app, collection.id, "Replacement", &error));

		const fs::path primary = temp.root / "resource_collections.json";
		if (missing_primary)
		{
			std::error_code remove_error;
			UAM_ASSERT(fs::remove(primary, remove_error));
			UAM_ASSERT(!remove_error);
		}
		else
		{
			const nlohmann::json unsupported_version = {
			    {"version", 2}, {"collections", nlohmann::json::array()},
			};
			UAM_ASSERT(uam::io::WriteTextFile(primary, unsupported_version.dump()));
		}

		const std::vector<ResourceCollection> loaded = uam::ResourceCollectionService::Load(temp.root);
		UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(1));
		UAM_ASSERT_EQ(loaded.front().id, collection.id);
		UAM_ASSERT_EQ(loaded.front().name, std::string("Recovered"));
	}
}

UAM_TEST(ResourceCollectionMutationsRollbackWhenPersistenceFails)
{
	TempDir temp("uam-resource-rollback");
	const fs::path data_root_file = temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(data_root_file, "occupied"));
	uam::AppState app = ResourceTestApp(data_root_file);
	ResourceCollection existing;
	existing.id = "resource-collection-existing";
	existing.name = "Original";
	app.resource_collections.push_back(existing);

	std::string error;
	UAM_ASSERT(!uam::ResourceCollectionService::Rename(app, existing.id, "Changed", &error));
	UAM_ASSERT_EQ(app.resource_collections[0].name, std::string("Original"));
	UAM_ASSERT(error.find("persist") != std::string::npos);
}

UAM_TEST(ResourceCollectionLoadSanitizesMalformedJsonWithoutChangingWorkspaceFolders)
{
	TempDir temp("uam-resource-load-validation");
	const nlohmann::json persisted = {
	    {"version", 1},
	    {"collections", nlohmann::json::array({
	                        {{"id", 42}, {"name", "Bad"}, {"collapsed", "yes"}},
	                        {{"id", "resource-collection-valid"},
	                         {"name", " Valid "},
	                         {"collapsed", true},
	                         {"references", nlohmann::json::array({
	                                            {{"id", "resource-reference-one"}, {"type", "file"}, {"target", "/tmp/item"}, {"label", 9}},
	                                            {{"id", "resource-reference-two"}, {"type", "file"}, {"target", "/tmp/item"}, {"label", "duplicate"}},
	                                        })}},
	                    })},
	};
	UAM_ASSERT(uam::io::WriteTextFile(temp.root / "resource_collections.json", persisted.dump()));

	uam::AppState app = ResourceTestApp(temp.root);
	const std::vector<ChatFolder> original_folders = app.folders;
	app.resource_collections = uam::ResourceCollectionService::Load(temp.root);
	UAM_ASSERT_EQ(app.resource_collections.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.resource_collections[0].name, std::string("Valid"));
	UAM_ASSERT_EQ(app.resource_collections[0].references.size(), static_cast<std::size_t>(1));
	UAM_ASSERT(app.resource_collections[0].references[0].label.empty());
	UAM_ASSERT_EQ(app.folders.size(), original_folders.size());
	UAM_ASSERT_EQ(app.folders[0].id, original_folders[0].id);
}
