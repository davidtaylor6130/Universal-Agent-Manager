#include "ollama_engine/engine_api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

/// <summary>Prints available model names with one-based indices.</summary>
/// <param name="pVecSModels">Model names to display.</param>
void PrintModels(const std::vector<std::string>& pVecSModels) {
  std::cout << "Available models:\n";
  for (std::size_t liIndex = 0; liIndex < pVecSModels.size(); ++liIndex) {
    std::cout << "  [" << (liIndex + 1) << "] " << pVecSModels[liIndex] << "\n";
  }
}

/// <summary>Reads a full line from stdin after printing a prompt.</summary>
/// <param name="pSPrompt">Prompt text to display.</param>
/// <returns>User input line, or empty when EOF is reached.</returns>
std::string ReadLine(const std::string& pSPrompt) {
  std::cout << pSPrompt;
  std::cout.flush();
  std::string lSLine;
  std::getline(std::cin, lSLine);
  return lSLine;
}

/// <summary>Attempts to parse a positive one-based model index.</summary>
/// <param name="pSInput">Raw user input.</param>
/// <returns>Zero-based index on success; otherwise nullopt.</returns>
std::optional<std::size_t> ParseModelIndex(const std::string& pSInput) {
  if (pSInput.empty()) {
    return std::nullopt;
  }
  char* lPtrEnd = nullptr;
  const unsigned long lliValue = std::strtoul(pSInput.c_str(), &lPtrEnd, 10);
  if (lPtrEnd == nullptr || *lPtrEnd != '\0' || lliValue == 0UL) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(lliValue - 1UL);
}

/// <summary>Selects a model by one-based index or exact name.</summary>
/// <param name="pSInput">User selection text.</param>
/// <param name="pVecSModels">Available model names.</param>
/// <returns>Selected model name on success; otherwise nullopt.</returns>
std::optional<std::string> ResolveSelectedModel(const std::string& pSInput,
                                                const std::vector<std::string>& pVecSModels) {
  const std::optional<std::size_t> lOptModelIndex = ParseModelIndex(pSInput);
  if (lOptModelIndex.has_value() && *lOptModelIndex < pVecSModels.size()) {
    return pVecSModels[*lOptModelIndex];
  }

  for (const std::string& lSModelName : pVecSModels) {
    if (lSModelName == pSInput) {
      return lSModelName;
    }
  }
  return std::nullopt;
}

/// <summary>Reads full file content into a string.</summary>
/// <param name="pPathFile">Path to read.</param>
/// <returns>File content on success, otherwise nullopt.</returns>
std::optional<std::string> ReadTextFile(const std::filesystem::path& pPathFile) {
  std::ifstream lFileIn(pPathFile, std::ios::in | std::ios::binary);
  if (!lFileIn.good()) {
    return std::nullopt;
  }
  std::ostringstream lBuffer;
  lBuffer << lFileIn.rdbuf();
  return lBuffer.str();
}

/// <summary>Trims whitespace from both sides of a string.</summary>
/// <param name="pSValue">Input string.</param>
/// <returns>Trimmed string.</returns>
std::string Trim(const std::string& pSValue) {
  std::size_t liBegin = 0;
  while (liBegin < pSValue.size() && std::isspace(static_cast<unsigned char>(pSValue[liBegin])) != 0) {
    ++liBegin;
  }
  std::size_t liEnd = pSValue.size();
  while (liEnd > liBegin && std::isspace(static_cast<unsigned char>(pSValue[liEnd - 1])) != 0) {
    --liEnd;
  }
  return pSValue.substr(liBegin, liEnd - liBegin);
}

/// <summary>
/// Normalizes text for loose comparison:
/// lowercase + keep alphanumeric + collapse others into spaces.
/// </summary>
/// <param name="pSText">Input text.</param>
/// <returns>Normalized text for matching.</returns>
std::string NormalizeForMatch(const std::string& pSText) {
  std::string lSOutput;
  lSOutput.reserve(pSText.size());
  bool lbPrevSpace = true;
  for (const unsigned char lCChar : pSText) {
    if (std::isalnum(lCChar) != 0) {
      lSOutput.push_back(static_cast<char>(std::tolower(lCChar)));
      lbPrevSpace = false;
      continue;
    }
    if (!lbPrevSpace) {
      lSOutput.push_back(' ');
      lbPrevSpace = true;
    }
  }
  return Trim(lSOutput);
}

/// <summary>Counts whitespace-delimited output tokens for throughput metrics.</summary>
/// <param name="pSText">Output text.</param>
/// <returns>Token count.</returns>
std::size_t CountTokens(const std::string& pSText) {
  std::istringstream lStream(pSText);
  std::size_t liCount = 0;
  std::string lSToken;
  while (lStream >> lSToken) {
    ++liCount;
  }
  return liCount;
}

struct PromptRunResult {
  ollama_engine::SendMessageResponse pSendMessageResponse;
  double pdTotalMilliseconds = 0.0;
  double pdTimeToFirstTokenMilliseconds = 0.0;
  std::size_t piOutputTokenCount = 0;
  double pdTokensPerSecond = 0.0;
};

/// <summary>
/// Runs a prompt and collects latency metrics.
/// TTFT here is a proxy measured as time until the engine enters Thinking/Running/Finished.
/// </summary>
/// <param name="pPtrEngine">Engine pointer.</param>
/// <param name="pSPrompt">Prompt text.</param>
/// <returns>Response plus timing metrics.</returns>
PromptRunResult RunPromptWithMetrics(ollama_engine::EngineInterface* pPtrEngine, const std::string& pSPrompt) {
  PromptRunResult lPromptRunResult;
  std::atomic<bool> lbDone{false};
  std::optional<std::chrono::steady_clock::time_point> lOptFirstTokenPoint;
  const std::chrono::steady_clock::time_point lStart = std::chrono::steady_clock::now();

  std::thread lWorker([&]() {
    lPromptRunResult.pSendMessageResponse = pPtrEngine->SendMessage(pSPrompt);
    lbDone.store(true);
  });

  while (!lbDone.load()) {
    const ollama_engine::CurrentStateResponse lCurrentStateResponse = pPtrEngine->QueryCurrentState();
    if (!lOptFirstTokenPoint.has_value() &&
        (lCurrentStateResponse.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Thinking ||
         lCurrentStateResponse.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Running ||
         lCurrentStateResponse.pEngineLifecycleState == ollama_engine::EngineLifecycleState::Finished)) {
      lOptFirstTokenPoint = std::chrono::steady_clock::now();
    }
    std::this_thread::sleep_for(std::chrono::microseconds(250));
  }
  lWorker.join();

  const std::chrono::steady_clock::time_point lEnd = std::chrono::steady_clock::now();
  if (!lOptFirstTokenPoint.has_value()) {
    lOptFirstTokenPoint = lEnd;
  }

  const std::chrono::duration<double, std::milli> lTotalDuration = lEnd - lStart;
  const std::chrono::duration<double, std::milli> lFirstTokenDuration = *lOptFirstTokenPoint - lStart;
  lPromptRunResult.pdTotalMilliseconds = lTotalDuration.count();
  lPromptRunResult.pdTimeToFirstTokenMilliseconds = lFirstTokenDuration.count();
  lPromptRunResult.piOutputTokenCount = CountTokens(lPromptRunResult.pSendMessageResponse.pSText);
  const double ldTotalSeconds = lPromptRunResult.pdTotalMilliseconds / 1000.0;
  lPromptRunResult.pdTokensPerSecond = (ldTotalSeconds > 0.0)
                                           ? static_cast<double>(lPromptRunResult.piOutputTokenCount) / ldTotalSeconds
                                           : 0.0;
  return lPromptRunResult;
}

/// <summary>Prints prompt execution metrics.</summary>
/// <param name="pPromptRunResult">Metrics payload.</param>
void PrintMetrics(const PromptRunResult& pPromptRunResult) {
  std::cout << "metrics> ttft_ms=" << pPromptRunResult.pdTimeToFirstTokenMilliseconds
            << " total_ms=" << pPromptRunResult.pdTotalMilliseconds
            << " output_tokens=" << pPromptRunResult.piOutputTokenCount
            << " tok/s=" << pPromptRunResult.pdTokensPerSecond << "\n";
}

struct QuestionAnswerCase {
  int piQuestionNumber = 0;
  std::filesystem::path pPathQuestionFile;
  std::filesystem::path pPathAnswerFile;
  std::string pSQuestionText;
  std::string pSAnswerText;
};

/// <summary>
/// Discovers question/answer files in a directory.
/// Supported formats:
/// - Question_1.txt + Question_1_Answer.txt
/// - Question_1.txt + Question_1_Anser.txt
/// </summary>
/// <param name="pPathDirectory">Directory to scan.</param>
/// <returns>Discovered sorted question/answer cases.</returns>
std::vector<QuestionAnswerCase> LoadQuestionAnswerCases(const std::filesystem::path& pPathDirectory) {
  std::vector<QuestionAnswerCase> lVecQuestionAnswerCases;
  const std::regex lQuestionPattern("^Question_([0-9]+)\\.txt$", std::regex::icase);
  std::error_code lErrorCode;
  if (!std::filesystem::exists(pPathDirectory, lErrorCode) || !std::filesystem::is_directory(pPathDirectory, lErrorCode)) {
    return lVecQuestionAnswerCases;
  }

  for (const std::filesystem::directory_entry& lDirectoryEntry :
       std::filesystem::directory_iterator(pPathDirectory, lErrorCode)) {
    if (lErrorCode || !lDirectoryEntry.is_regular_file(lErrorCode)) {
      continue;
    }
    const std::string lSFileName = lDirectoryEntry.path().filename().string();
    std::smatch lMatch;
    if (!std::regex_match(lSFileName, lMatch, lQuestionPattern) || lMatch.size() != 2) {
      continue;
    }

    const int liQuestionNumber = std::atoi(lMatch[1].str().c_str());
    if (liQuestionNumber <= 0) {
      continue;
    }

    const std::filesystem::path lPathAnswerFile =
        pPathDirectory / ("Question_" + std::to_string(liQuestionNumber) + "_Answer.txt");
    const std::filesystem::path lPathAnserFile =
        pPathDirectory / ("Question_" + std::to_string(liQuestionNumber) + "_Anser.txt");

    std::filesystem::path lPathResolvedAnswerFile;
    if (std::filesystem::exists(lPathAnswerFile, lErrorCode) && !lErrorCode) {
      lPathResolvedAnswerFile = lPathAnswerFile;
    } else if (std::filesystem::exists(lPathAnserFile, lErrorCode) && !lErrorCode) {
      lPathResolvedAnswerFile = lPathAnserFile;
    } else {
      continue;
    }

    const std::optional<std::string> lOptQuestionText = ReadTextFile(lDirectoryEntry.path());
    const std::optional<std::string> lOptAnswerText = ReadTextFile(lPathResolvedAnswerFile);
    if (!lOptQuestionText.has_value() || !lOptAnswerText.has_value()) {
      continue;
    }

    QuestionAnswerCase lQuestionAnswerCase;
    lQuestionAnswerCase.piQuestionNumber = liQuestionNumber;
    lQuestionAnswerCase.pPathQuestionFile = lDirectoryEntry.path();
    lQuestionAnswerCase.pPathAnswerFile = lPathResolvedAnswerFile;
    lQuestionAnswerCase.pSQuestionText = *lOptQuestionText;
    lQuestionAnswerCase.pSAnswerText = *lOptAnswerText;
    lVecQuestionAnswerCases.push_back(std::move(lQuestionAnswerCase));
  }

  std::sort(lVecQuestionAnswerCases.begin(), lVecQuestionAnswerCases.end(),
            [](const QuestionAnswerCase& pLhs, const QuestionAnswerCase& pRhs) {
              return pLhs.piQuestionNumber < pRhs.piQuestionNumber;
            });
  return lVecQuestionAnswerCases;
}

/// <summary>
/// Runs Q/A test cases and reports pass/fail based on normalized contains-match.
/// </summary>
/// <param name="pPtrEngine">Engine pointer.</param>
/// <param name="pPathDirectory">Directory holding Question_N files.</param>
/// <returns>True when all discovered cases pass.</returns>
bool RunQuestionAnswerTests(ollama_engine::EngineInterface* pPtrEngine, const std::filesystem::path& pPathDirectory) {
  const std::vector<QuestionAnswerCase> lVecQuestionAnswerCases = LoadQuestionAnswerCases(pPathDirectory);
  if (lVecQuestionAnswerCases.empty()) {
    std::cout << "tests> no Question_N.txt + Question_N_Answer.txt pairs found in " << pPathDirectory << "\n";
    return false;
  }

  std::size_t liPassCount = 0;
  std::size_t liFailCount = 0;
  std::cout << "tests> running " << lVecQuestionAnswerCases.size() << " test case(s)\n";

  for (const QuestionAnswerCase& lQuestionAnswerCase : lVecQuestionAnswerCases) {
    const PromptRunResult lPromptRunResult = RunPromptWithMetrics(pPtrEngine, lQuestionAnswerCase.pSQuestionText);
    const std::string lSActual = NormalizeForMatch(lPromptRunResult.pSendMessageResponse.pSText);
    const std::string lSExpected = NormalizeForMatch(lQuestionAnswerCase.pSAnswerText);
    const bool lbPass = lPromptRunResult.pSendMessageResponse.pbOk && !lSExpected.empty() &&
                        (lSActual.find(lSExpected) != std::string::npos);

    std::cout << "tests> Question_" << lQuestionAnswerCase.piQuestionNumber
              << (lbPass ? " PASS" : " FAIL") << "\n";
    PrintMetrics(lPromptRunResult);
    if (!lbPass) {
      std::cout << "tests> expected~ " << Trim(lQuestionAnswerCase.pSAnswerText) << "\n";
      std::cout << "tests> actual~   " << Trim(lPromptRunResult.pSendMessageResponse.pSText) << "\n";
      ++liFailCount;
    } else {
      ++liPassCount;
    }
  }

  std::cout << "tests> summary: pass=" << liPassCount << " fail=" << liFailCount << "\n";
  return liFailCount == 0;
}

struct BenchmarkProbe {
  std::string pSName;
  std::string pSPrompt;
};

/// <summary>
/// Returns a small benchmark probe set based on common open-source benchmark styles.
/// These are lightweight probes, not official full benchmark suites.
/// </summary>
/// <returns>Benchmark probes.</returns>
std::vector<BenchmarkProbe> BuildStandardOpenSourceBenchmarkProbes() {
  std::vector<BenchmarkProbe> lVecBenchmarkProbes;
  lVecBenchmarkProbes.push_back(BenchmarkProbe{
      "MMLU_style",
      "Multiple choice: Which gas do plants absorb from the atmosphere? "
      "A) Oxygen B) Nitrogen C) Carbon dioxide D) Helium. Answer with one letter."});
  lVecBenchmarkProbes.push_back(BenchmarkProbe{
      "GSM8K_style",
      "A store has 12 apples and sells 5, then receives 9 more. How many apples are in the store now?"});
  lVecBenchmarkProbes.push_back(BenchmarkProbe{
      "ARC_Easy_style",
      "Which source of energy is renewable? A) Coal B) Solar C) Oil D) Natural gas. Answer with one letter."});
  lVecBenchmarkProbes.push_back(BenchmarkProbe{
      "TruthfulQA_style",
      "Can humans breathe in outer space without a spacesuit? Answer briefly and factually."});
  return lVecBenchmarkProbes;
}

/// <summary>
/// Runs the built-in benchmark probes and prints aggregate latency stats.
/// </summary>
/// <param name="pPtrEngine">Engine pointer.</param>
void RunStandardOpenSourceBenchmarks(ollama_engine::EngineInterface* pPtrEngine) {
  const std::vector<BenchmarkProbe> lVecBenchmarkProbes = BuildStandardOpenSourceBenchmarkProbes();
  if (lVecBenchmarkProbes.empty()) {
    return;
  }

  std::cout << "bench> running " << lVecBenchmarkProbes.size()
            << " standard open-source benchmark probes (mini, non-official)\n";
  double ldTotalMs = 0.0;
  double ldTotalTtftMs = 0.0;
  double ldTotalTokPerSec = 0.0;
  for (const BenchmarkProbe& lBenchmarkProbe : lVecBenchmarkProbes) {
    const PromptRunResult lPromptRunResult = RunPromptWithMetrics(pPtrEngine, lBenchmarkProbe.pSPrompt);
    std::cout << "bench> " << lBenchmarkProbe.pSName << "\n";
    if (!lPromptRunResult.pSendMessageResponse.pbOk) {
      std::cout << "bench> error: " << lPromptRunResult.pSendMessageResponse.pSError << "\n";
      continue;
    }
    PrintMetrics(lPromptRunResult);
    ldTotalMs += lPromptRunResult.pdTotalMilliseconds;
    ldTotalTtftMs += lPromptRunResult.pdTimeToFirstTokenMilliseconds;
    ldTotalTokPerSec += lPromptRunResult.pdTokensPerSecond;
  }

  const double ldCount = static_cast<double>(lVecBenchmarkProbes.size());
  if (ldCount > 0.0) {
    std::cout << "bench> avg_total_ms=" << (ldTotalMs / ldCount)
              << " avg_ttft_ms=" << (ldTotalTtftMs / ldCount)
              << " avg_tok/s=" << (ldTotalTokPerSec / ldCount) << "\n";
  }
}

/// <summary>Prints CLI slash commands.</summary>
void PrintHelp() {
  std::cout << "Commands:\n";
  std::cout << "  /quit                     Exit the CLI\n";
  std::cout << "  /help                     Show command help\n";
  std::cout << "  /run_benchmarks           Run built-in open-source benchmark probes\n";
  std::cout << "  /run_tests <directory>    Run Question_N + Answer/Anser test pairs from directory\n";
}

}  // namespace

/// <summary>
/// Basic interactive CLI for the local Ollama engine wrapper.
/// Workflow:
/// 1) List models from the selected model folder.
/// 2) Let the user pick a model by number or exact file name.
/// 3) Load the model.
/// 4) Run prompt loop until "/quit".
/// </summary>
/// <param name="argc">Argument count.</param>
/// <param name="argv">Argument values. argv[1] may override model folder path.</param>
/// <returns>0 on success; non-zero on configuration or runtime errors.</returns>
int main(int argc, char** argv) {
  std::filesystem::path lPathModelFolder = std::filesystem::current_path() / "models";
  if (argc > 1 && argv[1] != nullptr && std::string(argv[1]).empty() == false) {
    lPathModelFolder = std::filesystem::path(argv[1]);
  }

  ollama_engine::EngineOptions lEngineOptions;
  lEngineOptions.pPathModelFolder = lPathModelFolder;
  lEngineOptions.piEmbeddingDimensions = 256;

  std::unique_ptr<ollama_engine::EngineInterface> lPtrEngine = ollama_engine::CreateEngine(lEngineOptions);
  const std::vector<std::string> lVecSModels = lPtrEngine->ListModels();
  if (lVecSModels.empty()) {
    std::cerr << "No models found in folder: " << lPathModelFolder << "\n";
    return 1;
  }

  std::cout << "Model folder: " << lPathModelFolder << "\n";
  PrintModels(lVecSModels);

  std::string lSModelName;
  while (true) {
    const std::string lSInput = ReadLine("Select model (number or exact name, /quit to exit): ");
    if (!std::cin.good() || lSInput == "/quit") {
      return 0;
    }
    const std::optional<std::string> lOptSelectedModel = ResolveSelectedModel(lSInput, lVecSModels);
    if (!lOptSelectedModel.has_value()) {
      std::cout << "Invalid selection. Try again.\n";
      continue;
    }

    std::string lSError;
    if (!lPtrEngine->Load(*lOptSelectedModel, &lSError)) {
      std::cout << "Failed to load model '" << *lOptSelectedModel << "': " << lSError << "\n";
      continue;
    }
    lSModelName = *lOptSelectedModel;
    break;
  }

  std::cout << "Loaded model: " << lSModelName << "\n";
  std::cout << "Chat is ready. Type /quit to exit.\n";
  PrintHelp();

  while (true) {
    const std::string lSPrompt = ReadLine("you> ");
    if (!std::cin.good() || lSPrompt == "/quit") {
      break;
    }
    if (lSPrompt == "/help") {
      PrintHelp();
      continue;
    }
    if (lSPrompt == "/run_benchmarks") {
      RunStandardOpenSourceBenchmarks(lPtrEngine.get());
      continue;
    }
    if (lSPrompt.rfind("/run_tests", 0) == 0) {
      const std::string lSTail = Trim(lSPrompt.substr(std::string("/run_tests").size()));
      if (lSTail.empty()) {
        std::cout << "tests> usage: /run_tests <directory>\n";
        continue;
      }
      const bool lbAllPassed = RunQuestionAnswerTests(lPtrEngine.get(), std::filesystem::path(lSTail));
      std::cout << "tests> overall: " << (lbAllPassed ? "PASS" : "FAIL") << "\n";
      continue;
    }
    if (lSPrompt.empty()) {
      continue;
    }

    const PromptRunResult lPromptRunResult = RunPromptWithMetrics(lPtrEngine.get(), lSPrompt);
    if (!lPromptRunResult.pSendMessageResponse.pbOk) {
      std::cout << "engine> error: " << lPromptRunResult.pSendMessageResponse.pSError << "\n";
      continue;
    }
    std::cout << "engine> " << lPromptRunResult.pSendMessageResponse.pSText << "\n";
    PrintMetrics(lPromptRunResult);
  }

  return 0;
}
