#pragma once

#include "common/models/app_models.h"

#include <filesystem>
#include <vector>
#include <memory>
#include <string>
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
	static std::shared_ptr<PreparedFolderSave> PrepareSave(const std::filesystem::path& data_root, const std::vector<ChatFolder>& folders);
	static bool PublishPreparedSave(const std::shared_ptr<PreparedFolderSave>& prepared);
	/// <summary>Loads folder definitions from the data root.</summary>
	static std::vector<ChatFolder> Load(const std::filesystem::path& data_root);
	/// <summary>Saves folder definitions into the data root.</summary>
	static bool Save(const std::filesystem::path& data_root, const std::vector<ChatFolder>& folders);
};
