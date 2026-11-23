#pragma once

#include "common/state/app_state.h"
#include "common/utils/store_load_fence.h"

#include <filesystem>
#include <string>
#include <vector>

namespace uam
{

class ResourceCollectionService
{
  public:
	/// Falls back to the validated backup. An unreadable or invalid file with no valid backup reports
	/// Unrecovered, loads no collections and fences every later save of this data root's collections.
	static std::vector<ResourceCollection> Load(const std::filesystem::path& data_root, uam::io::StoreLoadResult* result = nullptr);
	/// True while a failed load fences saves of this data root's collections.
	static bool IsSaveFenced(const std::filesystem::path& data_root);
	/// Refused while the store is fenced.
	static bool Save(const std::filesystem::path& data_root, const std::vector<ResourceCollection>& collections);

	/// Nesting is organisational only; collections are at most kMaxCollectionDepth levels deep.
	static constexpr std::size_t kMaxCollectionDepth = 5;

	static bool Create(AppState& app, const std::string& name, ResourceCollection* created, std::string* error_out, const std::string& parent_id = {});
	/// Moves a collection, with its references and child collections, under parent_id (empty for top level).
	static bool Move(AppState& app, const std::string& collection_id, const std::string& parent_id, std::string* error_out);
	static bool Rename(AppState& app, const std::string& collection_id, const std::string& name, std::string* error_out);
	/// Child collections and references move to the parent unless delete_children removes the subtree. A top-level collection has no parent, so its children become top level and its references go; workspaces become ungrouped. Referenced targets are never deleted.
	static bool Delete(AppState& app, const std::string& collection_id, std::string* error_out, bool delete_children = false);
	static bool ToggleCollapsed(AppState& app, const std::string& collection_id, std::string* error_out);
	static bool ReorderCollections(AppState& app, const std::vector<std::string>& collection_ids, std::string* error_out);
	static bool AddReference(AppState& app, const std::string& collection_id, const std::string& type, const std::string& target, const std::string& label, ResourceReference* created, std::string* error_out);
	static bool RemoveReference(AppState& app, const std::string& collection_id, const std::string& reference_id, std::string* error_out);
	static bool ReorderReferences(AppState& app, const std::string& collection_id, const std::vector<std::string>& reference_ids, std::string* error_out);
};

} // namespace uam
