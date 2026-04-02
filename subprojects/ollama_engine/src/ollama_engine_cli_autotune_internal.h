#pragma once

#include "ollama_engine/engine_api.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ollama_engine_cli::autotune
{

	struct AutoTuneTemplate
	{
		std::string pSName;
		std::string pSModelName;
		std::string pSCreatedUtc;
		std::string pSHardwareFingerprint;
		std::string pSHardwareSummary;
		ollama_engine::GenerationSettings pGenerationSettings;
		int piRunsPerSetting = 0;
		std::string pSEvaluationMode;
		std::string pSEvaluationSource;
		double pdAverageScore = 0.0;
		double pdAverageTokensPerSecond = 0.0;
		double pdAverageTotalMilliseconds = 0.0;
		double pdAverageTtftMilliseconds = 0.0;
	};

	std::string DescribeGenerationSettings(const ollama_engine::GenerationSettings& pGenerationSettings);
	std::string BuildHardwareFingerprint();

	std::filesystem::path GetAutoTuneTemplateDirectory(const std::filesystem::path& pPathModelFolder);
	std::filesystem::path GetAutoTuneFinalProfileDirectory(const std::filesystem::path& pPathModelFolder);
	std::vector<AutoTuneTemplate> LoadAutoTuneTemplates(const std::filesystem::path& pPathDirectory);
	std::vector<AutoTuneTemplate> LoadAutoTuneFinalProfiles(const std::filesystem::path& pPathDirectory);
	std::optional<AutoTuneTemplate> FindBestMatchingTemplate(const std::vector<AutoTuneTemplate>& pVecAutoTuneTemplates, const std::string& pSModelName, const std::string& pSHardwareFingerprint);
	bool ApplyAutoTuneTemplate(ollama_engine::EngineInterface* pPtrEngine, const AutoTuneTemplate& pAutoTuneTemplate, std::string* pSLoadedModelNameInOut, std::string* pSErrorOut);
	void RunAutoTuneWizard(ollama_engine::EngineInterface* pPtrEngine, const std::vector<std::string>& pVecSModels, const std::filesystem::path& pPathModelFolder, std::string* pSLoadedModelNameInOut);

} // namespace ollama_engine_cli::autotune
