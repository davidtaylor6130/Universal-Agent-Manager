#include "app/organization_service.h"
#include "app/resource_collection_service.h"
#include "common/chat/chat_ids.h"
#include "common/chat/chat_folder_store.h"
#include "common/config/custom_icon.h"
#include <algorithm>
#include <unordered_set>

namespace uam
{
namespace
{
	nlohmann::json Memberships(const AppState& app, const std::string& type, const std::string& target)
	{
		nlohmann::json result = nlohmann::json::array();
		for (const ResourceCollection& collection : app.resource_collections)
		{
			for (std::size_t index = 0; index < collection.references.size(); ++index)
			{
				const ResourceReference& reference = collection.references[index];
				if (reference.type != type || reference.target != target) continue;
				result.push_back({{"collectionId", collection.id}, {"index", index}, {"reference", {
				    {"id", reference.id}, {"type", type}, {"target", target}, {"label", reference.label}, {"customIcon", icons::Serialize(reference.custom_icon)}}}});
			}
		}
		return result;
	}
}

bool ApplyOrganizationAction(AppState& app, const nlohmann::json& payload, std::string* error_out)
{
	const auto reject = [&](const std::string& error)
	{
		if (error_out != nullptr) *error_out = error;
		return false;
	};
	if (!payload.is_object() || !payload.contains("kind") || !payload["kind"].is_string() ||
	    !payload.contains("expected") || !payload.contains("replacement")) return reject("Invalid organization action.");
	const std::string kind = payload["kind"].get<std::string>();
	if (kind == "collectionOrder" || kind == "folderOrder" || kind == "referenceOrder")
	{
		std::vector<std::string> current;
		std::string collection_id;
		if (kind == "folderOrder")
			for (const ChatFolder& folder : app.folders) current.push_back(folder.id);
		else if (kind == "collectionOrder")
			for (const ResourceCollection& collection : app.resource_collections) current.push_back(collection.id);
		else
		{
			if (!payload.contains("collectionId") || !payload["collectionId"].is_string()) return reject("Invalid collection identity.");
			collection_id = payload["collectionId"].get<std::string>();
			const auto collection = std::find_if(app.resource_collections.begin(), app.resource_collections.end(), [&](const ResourceCollection& entry) { return entry.id == collection_id; });
			if (collection == app.resource_collections.end()) return reject("Collection no longer exists.");
			for (const ResourceReference& reference : collection->references) current.push_back(reference.id);
		}
		if (payload["expected"] != nlohmann::json(current)) return reject("Organization order changed. Refresh before undoing.");
		if (!payload["replacement"].is_array() || payload["replacement"].size() != current.size() ||
		    std::ranges::any_of(payload["replacement"], [](const nlohmann::json& id) { return !id.is_string(); }))
			return reject("Invalid organization order.");
		const std::vector<std::string> ordered = payload["replacement"].get<std::vector<std::string>>();
		std::unordered_set<std::string> remaining(current.begin(), current.end());
		for (const std::string& id : ordered)
			if (remaining.erase(id) != 1) return reject("Organization IDs must match the current list exactly.");
		if (kind == "collectionOrder") return ResourceCollectionService::ReorderCollections(app, ordered, error_out);
		if (kind == "referenceOrder") return ResourceCollectionService::ReorderReferences(app, collection_id, ordered, error_out);
		std::vector<ChatFolder> folders;
		for (const std::string& id : ordered)
			folders.push_back(*std::find_if(app.folders.begin(), app.folders.end(), [&](const ChatFolder& folder) { return folder.id == id; }));
		if (!ChatFolderStore::Save(app.data_root, folders)) return reject("Could not persist workspace order.");
		app.folders = std::move(folders);
		return true;
	}
	if (kind != "memberships" || !payload.contains("type") || !payload["type"].is_string() ||
	    !payload.contains("target") || !payload["target"].is_string()) return reject("Invalid resource organization action.");
	const std::string type = payload["type"].get<std::string>();
	const std::string target = payload["target"].get<std::string>();
	if (type != "chat" && type != "workspace-folder" && type != "file" && type != "website" && type != "desktop-app")
		return reject("Invalid resource type.");
	if (target.empty() || target.size() > 4096) return reject("Invalid resource target.");
	if (type == "chat" && std::ranges::none_of(app.chats, [&](const ChatSession& chat) { return chat.id == target; })) return reject("Chat no longer exists.");
	if (type == "workspace-folder" && std::ranges::none_of(app.folders, [&](const ChatFolder& folder) { return folder.id == target; })) return reject("Workspace no longer exists.");
	if (Memberships(app, type, target) != payload["expected"]) return reject("Resource membership changed. Refresh before undoing.");
	if (!payload["replacement"].is_array() || payload["replacement"].size() > 200) return reject("Invalid resource memberships.");
	std::vector<ResourceCollection> changed = app.resource_collections;
	for (ResourceCollection& collection : changed)
		std::erase_if(collection.references, [&](const ResourceReference& reference) { return reference.type == type && reference.target == target; });
	std::unordered_set<std::string> ids;
	for (const ResourceCollection& collection : changed)
		for (const ResourceReference& reference : collection.references) ids.insert(reference.id);
	for (const nlohmann::json& item : payload["replacement"])
	{
		if (!item.is_object() || !item.contains("collectionId") || !item["collectionId"].is_string() ||
		    !item.contains("index") || (!item["index"].is_number_integer() || item["index"] < 0 || item["index"] > 500) || !item.contains("reference") || !item["reference"].is_object())
			return reject("Invalid resource membership.");
		const nlohmann::json& json = item["reference"];
		if (!json.contains("id") || !json["id"].is_string() || !json.contains("label") || !json["label"].is_string() ||
		    json.value("type", "") != type || json.value("target", "") != target) return reject("Invalid resource reference.");
		ResourceReference reference;
		reference.id = json["id"].get<std::string>();
		reference.type = type;
		reference.target = target;
		reference.label = json["label"].get<std::string>();
		reference.custom_icon = icons::Parse(json.value("customIcon", nlohmann::json(nullptr)));
		if (!chat_ids::IsSafeStorageChatId(reference.id) || !ids.insert(reference.id).second || reference.label.size() > 256)
			return reject("Invalid resource identity or label.");
		const auto collection = std::find_if(changed.begin(), changed.end(), [&](const ResourceCollection& entry) { return entry.id == item["collectionId"].get<std::string>(); });
		if (collection == changed.end() || collection->references.size() >= 500) return reject("Destination collection is missing or full.");
		const std::size_t index = item["index"].get<std::size_t>();
		if (index > collection->references.size()) return reject("Collection contents changed. Refresh before undoing.");
		collection->references.insert(collection->references.begin() + static_cast<std::ptrdiff_t>(index), std::move(reference));
	}
	if (!ResourceCollectionService::Save(app.data_root, changed)) return reject("Could not persist resource organization.");
	app.resource_collections = std::move(changed);
	return true;
}
}
