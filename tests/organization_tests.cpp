#include "test_harness.h"
#include "app/organization_service.h"
#include "app/resource_collection_service.h"
using namespace uam_test;

namespace
{
	nlohmann::json Membership(const std::string& collection)
	{
		return {{"collectionId", collection}, {"index", 0}, {"reference", {{"id", "ref"}, {"type", "chat"}, {"target", "chat"}, {"label", "Original"}, {"customIcon", nullptr}}}};
	}
	void Setup(uam::AppState& app)
	{
		ChatSession chat;
		chat.id = "chat";
		app.chats.push_back(chat);
		ResourceCollection first;
		first.id = "first";
		first.name = "First";
		first.references.push_back(ResourceReference{"ref", "chat", "chat", "Original"});
		ResourceCollection second;
		second.id = "second";
		second.name = "Second";
		app.resource_collections = {first, second};
	}
}
UAM_TEST(OrganizationMembershipMoveAndUndoPreserveResourceIdentity)
{
	TempDir temp("uam-organization");
	uam::AppState app;
	app.data_root = temp.root;
	Setup(app);
	std::string error;
	nlohmann::json operation = {{"kind", "memberships"}, {"type", "chat"}, {"target", "chat"},
	    {"expected", nlohmann::json::array({Membership("first")})}, {"replacement", nlohmann::json::array({Membership("second")})}};
	UAM_ASSERT(uam::ApplyOrganizationAction(app, operation, &error));
	UAM_ASSERT(app.resource_collections.front().references.empty());
	UAM_ASSERT_EQ(app.resource_collections.back().references.front().id, std::string("ref"));
	UAM_ASSERT(!uam::ApplyOrganizationAction(app, operation, &error));
	std::swap(operation["expected"], operation["replacement"]);
	UAM_ASSERT(uam::ApplyOrganizationAction(app, operation, &error));
	const std::vector<ResourceCollection> saved = uam::ResourceCollectionService::Load(temp.root);
	UAM_ASSERT_EQ(saved.front().references.front().label, std::string("Original"));
	UAM_ASSERT(saved.back().references.empty());
}
UAM_TEST(OrganizationFailedSavePreservesAllMembershipsAndOrder)
{
	TempDir temp("uam-organization-save");
	uam::AppState app;
	Setup(app);
	const fs::path blocked = temp.root / "blocked";
	UAM_ASSERT(uam::io::WriteTextFile(blocked, "Not a directory"));
	app.data_root = blocked;
	std::string error;
	UAM_ASSERT(!uam::ApplyOrganizationAction(app, {{"kind", "memberships"}, {"type", "chat"}, {"target", "chat"},
	    {"expected", nlohmann::json::array({Membership("first")})}, {"replacement", nlohmann::json::array({Membership("second")})}}, &error));
	UAM_ASSERT_EQ(app.resource_collections.front().references.front().id, std::string("ref"));
	UAM_ASSERT(app.resource_collections.back().references.empty());
	UAM_ASSERT(!uam::ApplyOrganizationAction(app, {{"kind", "collectionOrder"}, {"expected", {"first", "second"}}, {"replacement", {"second", "first"}}}, &error));
	UAM_ASSERT_EQ(app.resource_collections.front().id, std::string("first"));
}
UAM_TEST(OrganizationOrderUndoRejectsConcurrentChanges)
{
	TempDir temp("uam-organization-order");
	uam::AppState app;
	app.data_root = temp.root;
	Setup(app);
	std::string error;
	UAM_ASSERT(uam::ApplyOrganizationAction(app, {{"kind", "collectionOrder"}, {"expected", {"first", "second"}}, {"replacement", {"second", "first"}}}, &error));
	UAM_ASSERT(!uam::ApplyOrganizationAction(app, {{"kind", "collectionOrder"}, {"expected", {"first", "second"}}, {"replacement", {"second", "first"}}}, &error));
	ResourceCollection third;
	third.id = "third";
	third.name = "Third";
	app.resource_collections.push_back(third);
	UAM_ASSERT(!uam::ApplyOrganizationAction(app, {{"kind", "collectionOrder"}, {"expected", {"second", "first"}}, {"replacement", {"first", "second"}}}, &error));
	UAM_ASSERT_EQ(app.resource_collections.size(), std::size_t{3});
}

UAM_TEST(OrganizationWorkspaceOrderUndoPersistsAndRejectsStaleLists)
{
	TempDir temp("uam-organization-folders");
	uam::AppState app;
	app.data_root = temp.root;
	for (const std::string& id : {"a", "b"})
	{
		ChatFolder folder;
		folder.id = id;
		folder.title = id;
		app.folders.push_back(folder);
	}
	std::string error;
	const nlohmann::json operation = {{"kind", "folderOrder"}, {"expected", {"a", "b"}}, {"replacement", {"b", "a"}}};
	UAM_ASSERT(uam::ApplyOrganizationAction(app, operation, &error));
	UAM_ASSERT_EQ(ChatFolderStore::Load(temp.root).front().id, std::string("b"));
	UAM_ASSERT(!uam::ApplyOrganizationAction(app, operation, &error));
	UAM_ASSERT(uam::ApplyOrganizationAction(app, {{"kind", "folderOrder"}, {"expected", {"b", "a"}}, {"replacement", {"a", "b"}}}, &error));
	const fs::path blocked = temp.root / "blocked";
	UAM_ASSERT(uam::io::WriteTextFile(blocked, "Not a directory"));
	app.data_root = blocked;
	UAM_ASSERT(!uam::ApplyOrganizationAction(app, operation, &error));
	UAM_ASSERT_EQ(app.folders.front().id, std::string("a"));
}
UAM_TEST(OrganizationReferenceOrderUndoRetainsLabelsAndRejectsConcurrentGrowth)
{
	TempDir temp("uam-organization-references");
	uam::AppState app;
	app.data_root = temp.root;
	Setup(app);
	app.resource_collections.front().references.push_back(ResourceReference{"ref-two", "chat", "another-chat", "Second"});
	std::string error;
	UAM_ASSERT(uam::ApplyOrganizationAction(app, {{"kind", "referenceOrder"}, {"collectionId", "first"}, {"expected", {"ref", "ref-two"}}, {"replacement", {"ref-two", "ref"}}}, &error));
	UAM_ASSERT_EQ(app.resource_collections.front().references.front().label, std::string("Second"));
	app.resource_collections.front().references.push_back(ResourceReference{"ref-three", "chat", "new-chat", "New"});
	UAM_ASSERT(!uam::ApplyOrganizationAction(app, {{"kind", "referenceOrder"}, {"collectionId", "first"}, {"expected", {"ref-two", "ref"}}, {"replacement", {"ref", "ref-two"}}}, &error));
	UAM_ASSERT_EQ(app.resource_collections.front().references.size(), std::size_t{3});
}
