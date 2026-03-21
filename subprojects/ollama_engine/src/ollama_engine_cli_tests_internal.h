#pragma once

#include "ollama_engine/engine_api.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ollama_engine_cli::tests {

struct QuestionAnswerCase {
  int piQuestionNumber = 0;
  std::filesystem::path pPathQuestionFile;
  std::filesystem::path pPathAnswerFile;
  std::string pSQuestionText;
  std::string pSAnswerText;
};

struct BenchmarkProbe {
  std::string pSName;
  std::string pSPrompt;
  std::vector<std::string> pVecSExpectedContainsAny;
};

std::vector<QuestionAnswerCase> LoadQuestionAnswerCases(const std::filesystem::path& pPathDirectory);
bool RunQuestionAnswerTests(ollama_engine::EngineInterface* pPtrEngine, const std::filesystem::path& pPathDirectory);

std::vector<BenchmarkProbe> BuildStandardOpenSourceBenchmarkProbes();
bool IsBenchmarkProbePass(const BenchmarkProbe& pBenchmarkProbe, const std::string& pSTextResponse);
void RunStandardOpenSourceBenchmarks(ollama_engine::EngineInterface* pPtrEngine);

}  // namespace ollama_engine_cli::tests
