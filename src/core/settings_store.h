#pragma once

#include "app_models.h"

#include <filesystem>

class SettingsStore {
 public:
  static bool Save(const std::filesystem::path& settings_file,
                   const AppSettings& settings,
                   CenterViewMode center_view_mode);

  static void Load(const std::filesystem::path& settings_file,
                   AppSettings& settings,
                   CenterViewMode& center_view_mode);
};
