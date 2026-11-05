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
	app.resource_collections[1].custom_icon = {"text", "📚"};
	app.resource_collections[1].references[0].custom_icon = {"text", "E"};
	UAM_ASSERT(uam::ResourceCollectionService::Save(temp.root, app.resource_collections));

	const std::vector<ResourceCollection> loaded = uam::ResourceCollectionService::Load(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(loaded[0].id, second.id);
	UAM_ASSERT_EQ(loaded[1].name, std::string("Resources"));
	UAM_ASSERT(loaded[1].collapsed);
	UAM_ASSERT_EQ(loaded[1].custom_icon.value, std::string("📚"));
	UAM_ASSERT_EQ(loaded[1].references[0].custom_icon.value, std::string("E"));
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

namespace
{
	ResourceCollection CreateNested(uam::AppState& app, const std::string& name, const std::string& parent_id)
	{
		ResourceCollection created;
		std::string error;
		UAM_ASSERT(uam::ResourceCollectionService::Create(app, name, &created, &error, parent_id));
		return created;
	}

	const ResourceCollection* FindLoaded(const std::vector<ResourceCollection>& collections, const std::string& id)
	{
		const auto found = std::ranges::find_if(collections, [&](const ResourceCollection& collection) { return collection.id == id; });
		return found == collections.end() ? nullptr : &*found;
	}
} // namespace

UAM_TEST(ResourceCollectionNestingRejectsCyclesSelfUnknownParentsAndExcessDepth)
{
	TempDir temp("uam-resource-nesting");
	uam::AppState app = ResourceTestApp(temp.root);
	std::string error;
	const ResourceCollection level1 = CreateNested(app, "Homelab", "");
	const ResourceCollection level2 = CreateNested(app, "Containers", level1.id);
	const ResourceCollection level3 = CreateNested(app, "Level 3", level2.id);
	const ResourceCollection level4 = CreateNested(app, "Level 4", level3.id);
	const ResourceCollection level5 = CreateNested(app, "Level 5", level4.id);
	UAM_ASSERT_EQ(level2.parent_id, level1.id);
	const std::vector<ResourceCollection> before = app.resource_collections;

	UAM_ASSERT(!uam::ResourceCollectionService::Create(app, "Level 6", nullptr, &error, level5.id));
	UAM_ASSERT(error.find("deep") != std::string::npos);
	UAM_ASSERT(!uam::ResourceCollectionService::Create(app, "Orphan", nullptr, &error, "resource-collection-missing"));
	UAM_ASSERT(!uam::ResourceCollectionService::Move(app, level1.id, level1.id, &error));
	UAM_ASSERT(!uam::ResourceCollectionService::Move(app, level1.id, level3.id, &error));
	UAM_ASSERT(error.find("inside itself") != std::string::npos);
	UAM_ASSERT(!uam::ResourceCollectionService::Move(app, level2.id, "resource-collection-missing", &error));
	UAM_ASSERT_EQ(app.resource_collections.size(), before.size());

	// A two-level subtree cannot move under level 4, which would make it six levels deep.
	const ResourceCollection sibling = CreateNested(app, "Sibling", "");
	const ResourceCollection sibling_child = CreateNested(app, "Sibling child", sibling.id);
	UAM_ASSERT(!uam::ResourceCollectionService::Move(app, sibling.id, level4.id, &error));
	UAM_ASSERT(uam::ResourceCollectionService::Move(app, sibling.id, level3.id, &error));
	UAM_ASSERT_EQ(FindLoaded(app.resource_collections, sibling_child.id)->parent_id, sibling.id);
	UAM_ASSERT(uam::ResourceCollectionService::Move(app, sibling.id, "", &error));
	UAM_ASSERT(FindLoaded(app.resource_collections, sibling.id)->parent_id.empty());

	for (const ResourceCollection& original : before)
	{
		UAM_ASSERT_EQ(FindLoaded(app.resource_collections, original.id)->parent_id, original.parent_id);
	}

	// Save itself refuses an invalid hierarchy so no caller can persist one.
	std::vector<ResourceCollection> cyclic = app.resource_collections;
	cyclic[0].parent_id = level5.id;
	UAM_ASSERT(!uam::ResourceCollectionService::Save(temp.root, cyclic));
	std::vector<ResourceCollection> self_parented = app.resource_collections;
	self_parented[0].parent_id = self_parented[0].id;
	UAM_ASSERT(!uam::ResourceCollectionService::Save(temp.root, self_parented));
	const std::vector<ResourceCollection> loaded = uam::ResourceCollectionService::Load(temp.root);
	UAM_ASSERT(FindLoaded(loaded, level1.id)->parent_id.empty());
	UAM_ASSERT_EQ(FindLoaded(loaded, level5.id)->parent_id, level4.id);
}

UAM_TEST(ResourceCollectionLegacyFlatFileLoadsUnchangedAsTopLevel)
{
	TempDir temp("uam-resource-legacy");
	const nlohmann::json legacy = {
	    {"version", 1},
	    {"collections", nlohmann::json::array({
	                        {{"id", "resource-collection-a"}, {"name", "A"}, {"collapsed", true}, {"references", nlohmann::json::array({{{"id", "resource-reference-a"}, {"type", "workspace-folder"}, {"target", "workspace-1"}, {"label", "W"}}})}},
	                        {{"id", "resource-collection-b"}, {"name", "B"}, {"collapsed", false}},
	                    })},
	};
	const fs::path primary = temp.root / "resource_collections.json";
	const std::string legacy_text = legacy.dump(2);
	UAM_ASSERT(uam::io::WriteTextFile(primary, legacy_text));
	const std::vector<ResourceCollection> loaded = uam::ResourceCollectionService::Load(temp.root);
	UAM_ASSERT_EQ(loaded.size(), static_cast<std::size_t>(2));
	UAM_ASSERT(loaded[0].parent_id.empty());
	UAM_ASSERT(loaded[1].parent_id.empty());
	UAM_ASSERT(loaded[0].collapsed);
	UAM_ASSERT_EQ(loaded[0].references.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(uam::io::ReadTextFile(primary), legacy_text);

	// Damaged hierarchy fields fall back to top level instead of dropping collections.
	const nlohmann::json damaged = {
	    {"version", 1},
	    {"collections", nlohmann::json::array({
	                        {{"id", "resource-collection-a"}, {"name", "A"}, {"parentId", "resource-collection-b"}},
	                        {{"id", "resource-collection-b"}, {"name", "B"}, {"parentId", "resource-collection-a"}},
	                        {{"id", "resource-collection-c"}, {"name", "C"}, {"parentId", "resource-collection-c"}},
	                        {{"id", "resource-collection-d"}, {"name", "D"}, {"parentId", "resource-collection-missing"}},
	                        {{"id", "resource-collection-e"}, {"name", "E"}, {"parentId", 7}},
	                    })},
	};
	UAM_ASSERT(uam::io::WriteTextFile(primary, damaged.dump()));
	const std::vector<ResourceCollection> sanitized = uam::ResourceCollectionService::Load(temp.root);
	UAM_ASSERT_EQ(sanitized.size(), static_cast<std::size_t>(5));
	UAM_ASSERT(sanitized[0].parent_id.empty());
	UAM_ASSERT_EQ(sanitized[1].parent_id, std::string("resource-collection-a"));
	UAM_ASSERT(sanitized[2].parent_id.empty());
	UAM_ASSERT(sanitized[3].parent_id.empty());
	UAM_ASSERT(sanitized[4].parent_id.empty());
	UAM_ASSERT(uam::ResourceCollectionService::Save(temp.root, sanitized));
}

UAM_TEST(ResourceCollectionMoveCarriesReferencesWithoutTouchingTargets)
{
	TempDir temp("uam-resource-move");
	uam::AppState app = ResourceTestApp(temp.root);
	std::string error;
	const ResourceCollection parent = CreateNested(app, "Homelab", "");
	const ResourceCollection child = CreateNested(app, "Services", "");
	UAM_ASSERT(uam::ResourceCollectionService::AddReference(app, child.id, "workspace-folder", "workspace-1", "Workspace", nullptr, &error));
	UAM_ASSERT(uam::ResourceCollectionService::AddReference(app, child.id, "chat", "chat-1", "Chat", nullptr, &error));
	UAM_ASSERT(uam::ResourceCollectionService::AddReference(app, child.id, "file", "/tmp/notes.md", "Notes", nullptr, &error));
	const std::vector<ResourceReference> references = FindLoaded(app.resource_collections, child.id)->references;

	UAM_ASSERT(uam::ResourceCollectionService::Move(app, child.id, parent.id, &error));
	const std::vector<ResourceCollection> loaded = uam::ResourceCollectionService::Load(temp.root);
	const ResourceCollection* moved = FindLoaded(loaded, child.id);
	UAM_ASSERT(moved != nullptr);
	UAM_ASSERT_EQ(moved->parent_id, parent.id);
	UAM_ASSERT_EQ(moved->references.size(), references.size());
	for (std::size_t index = 0; index < references.size(); ++index)
	{
		UAM_ASSERT_EQ(moved->references[index].id, references[index].id);
		UAM_ASSERT_EQ(moved->references[index].target, references[index].target);
	}
	UAM_ASSERT_EQ(app.folders.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(uam::StateSerializer::Serialize(app)["resourceCollections"][1]["parentId"].get<std::string>(), parent.id);
}

UAM_TEST(ResourceCollectionDeleteParentMovesChildrenUpOrDeletesThemWithIt)
{
	TempDir temp("uam-resource-delete-tree");
	uam::AppState app = ResourceTestApp(temp.root);
	std::string error;
	const ResourceCollection root = CreateNested(app, "Root", "");
	const ResourceCollection middle = CreateNested(app, "Middle", root.id);
	const ResourceCollection leaf = CreateNested(app, "Leaf", middle.id);
	const ResourceCollection deepest = CreateNested(app, "Deepest", leaf.id);
	UAM_ASSERT(uam::ResourceCollectionService::AddReference(app, leaf.id, "workspace-folder", "workspace-1", "Workspace", nullptr, &error));

	UAM_ASSERT(uam::ResourceCollectionService::Delete(app, middle.id, &error, false));
	UAM_ASSERT(FindLoaded(app.resource_collections, middle.id) == nullptr);
	UAM_ASSERT_EQ(FindLoaded(app.resource_collections, leaf.id)->parent_id, root.id);
	UAM_ASSERT_EQ(FindLoaded(app.resource_collections, deepest.id)->parent_id, leaf.id);
	UAM_ASSERT_EQ(FindLoaded(app.resource_collections, leaf.id)->references.size(), static_cast<std::size_t>(1));

	UAM_ASSERT(uam::ResourceCollectionService::Delete(app, root.id, &error, true));
	UAM_ASSERT(app.resource_collections.empty());
	UAM_ASSERT(uam::ResourceCollectionService::Load(temp.root).empty());
	UAM_ASSERT_EQ(app.folders.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.folders[0].id, std::string("workspace-1"));
	UAM_ASSERT_EQ(app.chats.size(), static_cast<std::size_t>(1));
}

UAM_TEST(ResourceCollectionDeleteMovesReferencesToParentOrDropsThemAtTopLevel)
{
	TempDir temp("uam-resource-delete-contents");
	uam::AppState app = ResourceTestApp(temp.root);
	std::string error;
	const ResourceCollection homelab = CreateNested(app, "Homelab", "");
	const ResourceCollection services = CreateNested(app, "Services", homelab.id);
	const ResourceCollection sub = CreateNested(app, "Sub", services.id);
	UAM_ASSERT(uam::ResourceCollectionService::AddReference(app, services.id, "workspace-folder", "workspace-1", "Workspace", nullptr, &error));
	UAM_ASSERT(uam::ResourceCollectionService::AddReference(app, services.id, "desktop-app", "/Applications/App.app", "App", nullptr, &error));
	UAM_ASSERT(uam::ResourceCollectionService::AddReference(app, homelab.id, "workspace-folder", "workspace-1", "Workspace", nullptr, &error));
	const std::vector<ResourceReference> moved = FindLoaded(app.resource_collections, services.id)->references;

	UAM_ASSERT(uam::ResourceCollectionService::Delete(app, services.id, &error, false));
	const std::vector<ResourceCollection> loaded = uam::ResourceCollectionService::Load(temp.root);
	UAM_ASSERT(FindLoaded(loaded, services.id) == nullptr);
	UAM_ASSERT_EQ(FindLoaded(loaded, sub.id)->parent_id, homelab.id);
	const std::vector<ResourceReference>& kept = FindLoaded(loaded, homelab.id)->references;
	// The workspace the parent already held is not added twice; the desktop app keeps its identity.
	UAM_ASSERT_EQ(kept.size(), static_cast<std::size_t>(2));
	UAM_ASSERT_EQ(kept[0].target, std::string("workspace-1"));
	UAM_ASSERT_EQ(kept[1].id, moved[1].id);
	UAM_ASSERT_EQ(kept[1].target, std::string("/Applications/App.app"));
	UAM_ASSERT_EQ(kept[1].label, std::string("App"));

	// A top-level collection has no parent: children become top level, references go, targets stay.
	UAM_ASSERT(uam::ResourceCollectionService::Delete(app, homelab.id, &error, false));
	UAM_ASSERT_EQ(app.resource_collections.size(), static_cast<std::size_t>(1));
	UAM_ASSERT_EQ(app.resource_collections[0].id, sub.id);
	UAM_ASSERT(app.resource_collections[0].parent_id.empty());
	UAM_ASSERT(app.resource_collections[0].references.empty());
	UAM_ASSERT_EQ(app.folders.size(), static_cast<std::size_t>(1));
}

UAM_TEST(ResourceCollectionDeleteKeepsOrganisationWhenParentCannotHoldTheReferences)
{
	TempDir temp("uam-resource-delete-full");
	uam::AppState app = ResourceTestApp(temp.root);
	std::string error;
	const ResourceCollection parent = CreateNested(app, "Parent", "");
	const ResourceCollection child = CreateNested(app, "Child", parent.id);
	UAM_ASSERT(uam::ResourceCollectionService::AddReference(app, child.id, "website", "https://child.example", "Child", nullptr, &error));
	ResourceCollection* full = &app.resource_collections[0];
	for (int index = 0; index < 500; ++index)
	{
		ResourceReference reference;
		reference.id = "resource-reference-" + std::to_string(index);
		reference.type = "website";
		reference.target = "https://parent.example/" + std::to_string(index);
		full->references.push_back(std::move(reference));
	}
	const std::string before = uam::StateSerializer::Serialize(app)["resourceCollections"].dump();

	UAM_ASSERT(!uam::ResourceCollectionService::Delete(app, child.id, &error, false));
	UAM_ASSERT_EQ(error, std::string("Resource reference limit reached."));
	UAM_ASSERT_EQ(uam::StateSerializer::Serialize(app)["resourceCollections"].dump(), before);
}

UAM_TEST(ResourceCollectionNestingMutationsKeepPreviousOrganisationWhenSaveFails)
{
	TempDir temp("uam-resource-nesting-rollback");
	const fs::path data_root_file = temp.root / "not-a-directory";
	UAM_ASSERT(uam::io::WriteTextFile(data_root_file, "occupied"));
	uam::AppState app = ResourceTestApp(data_root_file);
	ResourceCollection parent;
	parent.id = "resource-collection-parent";
	parent.name = "Parent";
	ResourceCollection child;
	child.id = "resource-collection-child";
	child.name = "Child";
	child.parent_id = parent.id;
	ResourceCollection loose;
	loose.id = "resource-collection-loose";
	loose.name = "Loose";
	app.resource_collections = {parent, child, loose};

	std::string error;
	UAM_ASSERT(!uam::ResourceCollectionService::Create(app, "Inside", nullptr, &error, parent.id));
	UAM_ASSERT(!uam::ResourceCollectionService::Move(app, loose.id, parent.id, &error));
	UAM_ASSERT(!uam::ResourceCollectionService::Delete(app, parent.id, &error, false));
	UAM_ASSERT(!uam::ResourceCollectionService::Delete(app, parent.id, &error, true));
	UAM_ASSERT(error.find("persist") != std::string::npos);
	UAM_ASSERT_EQ(app.resource_collections.size(), static_cast<std::size_t>(3));
	UAM_ASSERT_EQ(app.resource_collections[1].parent_id, parent.id);
	UAM_ASSERT(app.resource_collections[2].parent_id.empty());
}
