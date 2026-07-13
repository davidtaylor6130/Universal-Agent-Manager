#include "app/resource_collection_service.h"

#include "common/chat/chat_ids.h"
#include "common/paths/path_utils.h"
#include "common/utils/io_utils.h"
#include "common/utils/string_utils.h"
#include "common/utils/time_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace uam
{
	namespace
	{
		constexpr std::string_view kFileName = "resource_collections.json";
		constexpr std::size_t kMaxCollections = 200;
		constexpr std::size_t kMaxReferencesPerCollection = 500;
		constexpr std::size_t kMaxNameBytes = 160;
		constexpr std::size_t kMaxLabelBytes = 256;
		constexpr std::size_t kMaxTargetBytes = 4096;

		std::filesystem::path FilePath(const std::filesystem::path& data_root)
		{
			return data_root / kFileName;
		}

		bool IsReferenceType(std::string_view type)
		{
			return type == "workspace-folder" || type == "chat" || type == "file" || type == "website" || type == "desktop-app";
		}

		bool IsValidId(std::string_view id)
		{
			return uam::chat_ids::IsSafeStorageChatId(id);
		}

		void SetError(std::string* error_out, const std::string& message)
		{
			if (error_out != nullptr)
			{
				*error_out = message;
			}
		}

		std::string JsonString(const nlohmann::json& object, std::string_view key)
		{
			const auto found = object.find(key);
			return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
		}

		bool JsonBool(const nlohmann::json& object, std::string_view key, bool fallback)
		{
			const auto found = object.find(key);
			return found != object.end() && found->is_boolean() ? found->get<bool>() : fallback;
		}

		bool NormalizeName(const std::string& value, std::string* normalized, std::string* error_out)
		{
			*normalized = uam::strings::Trim(value);
			if (normalized->empty() || normalized->size() > kMaxNameBytes)
			{
				SetError(error_out, "Resource collection name must be between 1 and 160 bytes.");
				return false;
			}
			return true;
		}

		bool ValidateReferenceTarget(const AppState& app, const std::string& type, const std::string& target, std::string* error_out)
		{
			if (target.empty() || target.size() > kMaxTargetBytes)
			{
				SetError(error_out, "Resource target must be between 1 and 4096 bytes.");
				return false;
			}
			if (type == "workspace-folder")
			{
				const bool found = std::ranges::any_of(app.folders, [&](const ChatFolder& folder) { return folder.id == target; });
				if (!found)
				{
					SetError(error_out, "Workspace folder not found: " + target);
					return false;
				}
			}
			else if (type == "chat")
			{
				const bool found = std::ranges::any_of(app.chats, [&](const ChatSession& chat) { return chat.id == target; });
				if (!found)
				{
					SetError(error_out, "Chat not found: " + target);
					return false;
				}
			}
			else if (type == "website" && !uam::strings::StartsWith(target, "https://") && !uam::strings::StartsWith(target, "http://"))
			{
				SetError(error_out, "Website resources must use an http or https URL.");
				return false;
			}
			return true;
		}

		nlohmann::json ReferenceJson(const ResourceReference& reference)
		{
			return {{"id", reference.id}, {"type", reference.type}, {"target", reference.target}, {"label", reference.label}};
		}

		nlohmann::json CollectionJson(const ResourceCollection& collection)
		{
			nlohmann::json references = nlohmann::json::array();
			for (const ResourceReference& reference : collection.references)
			{
				references.push_back(ReferenceJson(reference));
			}
			return {{"id", collection.id}, {"name", collection.name}, {"collapsed", collection.collapsed}, {"references", std::move(references)}};
		}

		std::optional<std::vector<ResourceCollection>> ParseCollections(const std::string& text)
		{
			const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
			if (!root.is_object() || !root.contains("collections") || !root["collections"].is_array())
			{
				return std::nullopt;
			}
			std::vector<ResourceCollection> collections;
			std::unordered_set<std::string> collection_ids;
			for (const nlohmann::json& value : root["collections"])
			{
				if (!value.is_object() || collections.size() >= kMaxCollections)
				{
					continue;
				}
				ResourceCollection collection;
				collection.id = uam::strings::Trim(JsonString(value, "id"));
				collection.name = uam::strings::Trim(JsonString(value, "name"));
				collection.collapsed = JsonBool(value, "collapsed", false);
				if (!IsValidId(collection.id) || collection.name.empty() || collection.name.size() > kMaxNameBytes || !collection_ids.insert(collection.id).second)
				{
					continue;
				}
				std::unordered_set<std::string> reference_ids;
				std::unordered_set<std::string> resource_targets;
				if (value.contains("references") && value["references"].is_array())
				{
					for (const nlohmann::json& item : value["references"])
					{
						if (!item.is_object() || collection.references.size() >= kMaxReferencesPerCollection)
						{
							continue;
						}
						ResourceReference reference;
						reference.id = uam::strings::Trim(JsonString(item, "id"));
						reference.type = uam::strings::Trim(JsonString(item, "type"));
						reference.target = uam::strings::Trim(JsonString(item, "target"));
						reference.label = uam::strings::Trim(JsonString(item, "label"));
						const std::string resource_target = reference.type + "\n" + reference.target;
						if (!IsValidId(reference.id) || !IsReferenceType(reference.type) || reference.target.empty() || reference.target.size() > kMaxTargetBytes || reference.label.size() > kMaxLabelBytes || !reference_ids.insert(reference.id).second || !resource_targets.insert(resource_target).second)
						{
							continue;
						}
						collection.references.push_back(std::move(reference));
					}
				}
				collections.push_back(std::move(collection));
			}
			return collections;
		}

		ResourceCollection* FindCollection(std::vector<ResourceCollection>& collections, const std::string& collection_id)
		{
			const std::string id = uam::strings::Trim(collection_id);
			const auto found = std::ranges::find_if(collections, [&](const ResourceCollection& collection) { return collection.id == id; });
			return found == collections.end() ? nullptr : &*found;
		}

		bool Persist(AppState& app, std::vector<ResourceCollection> next, std::string* error_out)
		{
			if (!ResourceCollectionService::Save(app.data_root, next))
			{
				SetError(error_out, "Failed to persist resource collections.");
				return false;
			}
			app.resource_collections = std::move(next);
			return true;
		}

		template <typename Item>
		bool ReorderExact(std::vector<Item>* items, const std::vector<std::string>& ids, std::string* error_out)
		{
			if (ids.size() != items->size())
			{
				SetError(error_out, "Reorder ids must include every item exactly once.");
				return false;
			}
			std::unordered_map<std::string, Item> by_id;
			for (const Item& item : *items)
			{
				by_id.emplace(item.id, item);
			}
			std::unordered_set<std::string> seen;
			std::vector<Item> reordered;
			reordered.reserve(items->size());
			for (const std::string& raw_id : ids)
			{
				const std::string id = uam::strings::Trim(raw_id);
				const auto found = by_id.find(id);
				if (found == by_id.end() || !seen.insert(id).second)
				{
					SetError(error_out, "Reorder ids must include every item exactly once.");
					return false;
				}
				reordered.push_back(found->second);
			}
			*items = std::move(reordered);
			return true;
		}

		std::string NewId(std::string_view prefix)
		{
			static std::atomic_uint64_t sequence{0};
			return std::string(prefix) + "-" + uam::time::SystemEpochMicrosecondsTokenNow() + "-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
		}
	} // namespace

	std::vector<ResourceCollection> ResourceCollectionService::Load(const std::filesystem::path& data_root)
	{
		const std::filesystem::path file = FilePath(data_root);
		if (uam::paths::PathExistsNoThrow(file))
		{
			if (const auto parsed = ParseCollections(uam::io::ReadTextFile(file)); parsed.has_value())
			{
				return *parsed;
			}
		}
		const std::filesystem::path backup = uam::io::MakeBackupPath(file);
		if (uam::paths::PathExistsNoThrow(backup))
		{
			if (const auto parsed = ParseCollections(uam::io::ReadTextFile(backup)); parsed.has_value())
			{
				return *parsed;
			}
		}
		return {};
	}

	bool ResourceCollectionService::Save(const std::filesystem::path& data_root, const std::vector<ResourceCollection>& collections)
	{
		if (collections.size() > kMaxCollections)
		{
			return false;
		}
		nlohmann::json values = nlohmann::json::array();
		std::unordered_set<std::string> collection_ids;
		for (const ResourceCollection& collection : collections)
		{
			if (!IsValidId(collection.id) || collection.name.empty() || collection.name.size() > kMaxNameBytes || collection.references.size() > kMaxReferencesPerCollection || !collection_ids.insert(collection.id).second)
			{
				return false;
			}
			std::unordered_set<std::string> reference_ids;
			std::unordered_set<std::string> resource_targets;
			for (const ResourceReference& reference : collection.references)
			{
				const std::string resource_target = reference.type + "\n" + reference.target;
				if (!IsValidId(reference.id) || !IsReferenceType(reference.type) || reference.target.empty() || reference.target.size() > kMaxTargetBytes || reference.label.size() > kMaxLabelBytes || !reference_ids.insert(reference.id).second || !resource_targets.insert(resource_target).second)
				{
					return false;
				}
			}
			values.push_back(CollectionJson(collection));
		}
		return uam::io::WriteTextFile(FilePath(data_root), nlohmann::json{{"version", 1}, {"collections", std::move(values)}}.dump(2));
	}

	bool ResourceCollectionService::Create(AppState& app, const std::string& name, ResourceCollection* created, std::string* error_out)
	{
		std::string normalized;
		if (!NormalizeName(name, &normalized, error_out) || app.resource_collections.size() >= kMaxCollections)
		{
			if (app.resource_collections.size() >= kMaxCollections)
			{
				SetError(error_out, "Resource collection limit reached.");
			}
			return false;
		}
		ResourceCollection collection;
		collection.id = NewId("resource-collection");
		collection.name = normalized;
		auto next = app.resource_collections;
		next.push_back(collection);
		if (!Persist(app, std::move(next), error_out))
		{
			return false;
		}
		if (created != nullptr)
		{
			*created = collection;
		}
		return true;
	}

	bool ResourceCollectionService::Rename(AppState& app, const std::string& collection_id, const std::string& name, std::string* error_out)
	{
		std::string normalized;
		if (!NormalizeName(name, &normalized, error_out))
		{
			return false;
		}
		auto next = app.resource_collections;
		ResourceCollection* collection = FindCollection(next, collection_id);
		if (collection == nullptr)
		{
			SetError(error_out, "Resource collection not found: " + uam::strings::Trim(collection_id));
			return false;
		}
		collection->name = normalized;
		return Persist(app, std::move(next), error_out);
	}

	bool ResourceCollectionService::Delete(AppState& app, const std::string& collection_id, std::string* error_out)
	{
		auto next = app.resource_collections;
		const std::string id = uam::strings::Trim(collection_id);
		const auto found = std::ranges::find_if(next, [&](const ResourceCollection& collection) { return collection.id == id; });
		if (found == next.end())
		{
			SetError(error_out, "Resource collection not found: " + id);
			return false;
		}
		next.erase(found);
		return Persist(app, std::move(next), error_out);
	}

	bool ResourceCollectionService::ToggleCollapsed(AppState& app, const std::string& collection_id, std::string* error_out)
	{
		auto next = app.resource_collections;
		ResourceCollection* collection = FindCollection(next, collection_id);
		if (collection == nullptr)
		{
			SetError(error_out, "Resource collection not found: " + uam::strings::Trim(collection_id));
			return false;
		}
		collection->collapsed = !collection->collapsed;
		return Persist(app, std::move(next), error_out);
	}

	bool ResourceCollectionService::ReorderCollections(AppState& app, const std::vector<std::string>& collection_ids, std::string* error_out)
	{
		auto next = app.resource_collections;
		if (!ReorderExact(&next, collection_ids, error_out))
		{
			return false;
		}
		return Persist(app, std::move(next), error_out);
	}

	bool ResourceCollectionService::AddReference(AppState& app, const std::string& collection_id, const std::string& type, const std::string& target, const std::string& label, ResourceReference* created, std::string* error_out)
	{
		const std::string normalized_type = uam::strings::Trim(type);
		const std::string normalized_target = uam::strings::Trim(target);
		const std::string normalized_label = uam::strings::Trim(label);
		if (!IsReferenceType(normalized_type))
		{
			SetError(error_out, "Unsupported resource type: " + normalized_type);
			return false;
		}
		if (normalized_label.size() > kMaxLabelBytes)
		{
			SetError(error_out, "Resource label exceeds 256 bytes.");
			return false;
		}
		if (!ValidateReferenceTarget(app, normalized_type, normalized_target, error_out))
		{
			return false;
		}
		auto next = app.resource_collections;
		ResourceCollection* collection = FindCollection(next, collection_id);
		if (collection == nullptr)
		{
			SetError(error_out, "Resource collection not found: " + uam::strings::Trim(collection_id));
			return false;
		}
		if (collection->references.size() >= kMaxReferencesPerCollection)
		{
			SetError(error_out, "Resource reference limit reached.");
			return false;
		}
		if (std::ranges::any_of(collection->references, [&](const ResourceReference& reference) { return reference.type == normalized_type && reference.target == normalized_target; }))
		{
			SetError(error_out, "Resource is already in this collection.");
			return false;
		}
		ResourceReference reference;
		reference.id = NewId("resource-reference");
		reference.type = normalized_type;
		reference.target = normalized_target;
		reference.label = normalized_label;
		collection->references.push_back(reference);
		if (!Persist(app, std::move(next), error_out))
		{
			return false;
		}
		if (created != nullptr)
		{
			*created = reference;
		}
		return true;
	}

	bool ResourceCollectionService::RemoveReference(AppState& app, const std::string& collection_id, const std::string& reference_id, std::string* error_out)
	{
		auto next = app.resource_collections;
		ResourceCollection* collection = FindCollection(next, collection_id);
		if (collection == nullptr)
		{
			SetError(error_out, "Resource collection not found: " + uam::strings::Trim(collection_id));
			return false;
		}
		const std::string id = uam::strings::Trim(reference_id);
		const auto found = std::ranges::find_if(collection->references, [&](const ResourceReference& reference) { return reference.id == id; });
		if (found == collection->references.end())
		{
			SetError(error_out, "Resource reference not found: " + id);
			return false;
		}
		collection->references.erase(found);
		return Persist(app, std::move(next), error_out);
	}

	bool ResourceCollectionService::ReorderReferences(AppState& app, const std::string& collection_id, const std::vector<std::string>& reference_ids, std::string* error_out)
	{
		auto next = app.resource_collections;
		ResourceCollection* collection = FindCollection(next, collection_id);
		if (collection == nullptr)
		{
			SetError(error_out, "Resource collection not found: " + uam::strings::Trim(collection_id));
			return false;
		}
		if (!ReorderExact(&collection->references, reference_ids, error_out))
		{
			return false;
		}
		return Persist(app, std::move(next), error_out);
	}

} // namespace uam
