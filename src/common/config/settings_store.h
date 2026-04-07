#pragma once

#include "common/models/app_models.h"

#include <filesystem>

/// <summary>
/// Reads and writes persisted application settings.
/// </summary>
class SettingsStore
{
  public:
	/// <summary>Saves settings and center view mode to disk.</summary>
	static bool Save(const std::filesystem::path& settings_file, const AppSettings& settings, CenterViewMode center_view_mode);

	/// <summary>Loads settings and center view mode from disk.</summary>
	static void Load(const std::filesystem::path& settings_file, AppSettings& settings, CenterViewMode& center_view_mode);
};
