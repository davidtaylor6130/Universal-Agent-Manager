#pragma once

#include "ollama_engine/engine_api.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ollama_engine_cli::common {

struct PromptRunResult {
  ollama_engine::SendMessageResponse pSendMessageResponse;
  double pdTotalMilliseconds = 0.0;
  double pdTimeToFirstTokenMilliseconds = 0.0;
  std::size_t piOutputTokenCount = 0;
  double pdTokensPerSecond = 0.0;
};

struct CliSession {
  std::filesystem::path pPathModelFolder;
  std::unique_ptr<ollama_engine::EngineInterface> pPtrEngine;
  std::vector<std::string> pVecSModels;
  std::string pSLoadedModelName;
};

void PrintModels(const std::vector<std::string>& pVecSModels);
std::string ReadLine(const std::string& pSPrompt);
std::optional<std::size_t> ParseModelIndex(const std::string& pSInput);
std::optional<std::string> ResolveSelectedModel(const std::string& pSInput,
                                                const std::vector<std::string>& pVecSModels);
std::optional<std::string> ReadTextFile(const std::filesystem::path& pPathFile);
std::string Trim(const std::string& pSValue);
std::string NormalizeForMatch(const std::string& pSText);
PromptRunResult RunPromptWithMetrics(ollama_engine::EngineInterface* pPtrEngine, const std::string& pSPrompt);
void PrintMetrics(const PromptRunResult& pPromptRunResult);

bool BootstrapCliSession(int argc, char** argv, const std::string& pSPromptPrefix, CliSession* pCliSessionOut,
                         int* piExitCodeOut);

}  // namespace ollama_engine_cli::common
