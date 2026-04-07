#pragma once

#include "common/models/app_models.h"

#include <filesystem>
#include <vector>

/// <summary>
/// Loads and saves chat folder metadata.
/// </summary>
class ChatFolderStore
{
  public:
	/// <summary>Loads folder definitions from the data root.</summary>
	static std::vector<ChatFolder> Load(const std::filesystem::path& data_root);
	/// <summary>Saves folder definitions into the data root.</summary>
	static bool Save(const std::filesystem::path& data_root, const std::vector<ChatFolder>& folders);
};
