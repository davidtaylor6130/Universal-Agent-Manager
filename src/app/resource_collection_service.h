#pragma once

#include "common/state/app_state.h"

#include <filesystem>
#include <string>
#include <vector>

namespace uam
{

class ResourceCollectionService
{
  public:
	static std::vector<ResourceCollection> Load(const std::filesystem::path& data_root);
	static bool Save(const std::filesystem::path& data_root, const std::vector<ResourceCollection>& collections);

	static bool Create(AppState& app, const std::string& name, ResourceCollection* created, std::string* error_out);
	static bool Rename(AppState& app, const std::string& collection_id, const std::string& name, std::string* error_out);
	static bool Delete(AppState& app, const std::string& collection_id, std::string* error_out);
	static bool ToggleCollapsed(AppState& app, const std::string& collection_id, std::string* error_out);
	static bool ReorderCollections(AppState& app, const std::vector<std::string>& collection_ids, std::string* error_out);
	static bool AddReference(AppState& app, const std::string& collection_id, const std::string& type, const std::string& target, const std::string& label, ResourceReference* created, std::string* error_out);
	static bool RemoveReference(AppState& app, const std::string& collection_id, const std::string& reference_id, std::string* error_out);
	static bool ReorderReferences(AppState& app, const std::string& collection_id, const std::vector<std::string>& reference_ids, std::string* error_out);
};

} // namespace uam
