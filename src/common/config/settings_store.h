#pragma once

#include "common/models/app_models.h"

#include <nlohmann/json_fwd.hpp>
#include <filesystem>
#include <string>

struct SettingsLoadResult
{
	bool loaded = false;
	bool recovered_from_backup = false;
	bool unrecovered_error = false;
	std::string warning;
};

/// <summary>
/// Reads and writes persisted application settings.
/// </summary>
class SettingsStore
{
  public:
	static nlohmann::json SerializeExecutionRoutingSettings(const ExecutionRoutingSettings& settings);
	static bool ParseExecutionRoutingSettings(const nlohmann::json& value, ExecutionRoutingSettings& settings, std::string& error);
	/// <summary>Saves settings to disk.</summary>
	static bool Save(const std::filesystem::path& settings_file, const AppSettings& settings);

	/// <summary>Loads settings from disk.</summary>
	static SettingsLoadResult Load(const std::filesystem::path& settings_file, AppSettings& settings);
};
