#pragma once

#include "common/models/app_models.h"
#include "common/utils/store_load_fence.h"

#include <filesystem>
#include <vector>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>

struct PreparedFolderSave
{
	std::filesystem::path destination;
	std::filesystem::path temporary;
	~PreparedFolderSave();
};

/// <summary>
/// Loads and saves chat folder metadata.
/// </summary>
class ChatFolderStore
{
  public:
	/// <summary>Allocate a collision-checked ID independently of names and folder order.</summary>
	static std::string AllocateId(const std::unordered_set<std::string>& used_ids);
	static std::string AllocateId(const std::vector<ChatFolder>& folders);
	static std::string Serialize(const std::vector<ChatFolder>& folders);
	/// Admission only: retain external/remote workspace references; refuse local bindings into a stopped source profile.
	static bool ValidateDataFolderReferences(std::string_view text, const std::filesystem::path& source, std::string& error);
	static std::shared_ptr<PreparedFolderSave> PrepareSave(const std::filesystem::path& data_root, const std::vector<ChatFolder>& folders);
	static bool PublishPreparedSave(const std::shared_ptr<PreparedFolderSave>& prepared);
	/// <summary>Loads folder definitions from the data root, falling back to the validated backup.</summary>
	/// An unreadable or invalid file with no valid backup reports Unrecovered and fences every later
	/// save to this data root for the rest of the process, so partial state cannot overwrite it.
	static std::vector<ChatFolder> Load(const std::filesystem::path& data_root, uam::io::StoreLoadResult* result = nullptr);
	/// <summary>True while a failed load fences saves of this data root's folder file.</summary>
	static bool IsSaveFenced(const std::filesystem::path& data_root);
	/// <summary>Saves folder definitions into the data root; refused while the store is fenced.</summary>
	static bool Save(const std::filesystem::path& data_root, const std::vector<ChatFolder>& folders);
};
