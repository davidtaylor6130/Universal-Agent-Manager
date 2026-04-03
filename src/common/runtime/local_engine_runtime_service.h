#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct LocalEngineResponse
{
	bool ok = false;
	std::string text;
	std::string error;
};

class LocalEngineRuntimeService
{
  public:
	std::vector<std::string> ListModels(const std::filesystem::path& model_folder) const;
	bool LoadModelIfNeeded(const std::filesystem::path& model_folder,
	                       const std::string& selected_model_id,
	                       std::string& loaded_model_id_in_out,
	                       std::string* error_out) const;
	LocalEngineResponse SendPrompt(const std::filesystem::path& model_folder, const std::string& prompt) const;
};
