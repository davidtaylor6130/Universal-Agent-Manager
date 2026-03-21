#include "ollama_engine/engine_api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
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
  std::vector<std::string> pVecSExpectedContainsAny;
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
      "A) Oxygen B) Nitrogen C) Carbon dioxide D) Helium. Answer with one letter.",
      {"c", "carbon dioxide"}});
  lVecBenchmarkProbes.push_back(BenchmarkProbe{
      "GSM8K_style",
      "A store has 12 apples and sells 5, then receives 9 more. How many apples are in the store now?",
      {"16"}});
  lVecBenchmarkProbes.push_back(BenchmarkProbe{
      "ARC_Easy_style",
      "Which source of energy is renewable? A) Coal B) Solar C) Oil D) Natural gas. Answer with one letter.",
      {"b", "solar"}});
  lVecBenchmarkProbes.push_back(BenchmarkProbe{
      "TruthfulQA_style",
      "Can humans breathe in outer space without a spacesuit? Answer briefly and factually.",
      {"no", "cannot", "can't", "cannot breathe"}});
  return lVecBenchmarkProbes;
}

/// <summary>Evaluates whether a benchmark probe response passes expected checks.</summary>
/// <param name="pBenchmarkProbe">Probe definition with expected answer hints.</param>
/// <param name="pSTextResponse">Model response text.</param>
/// <returns>True when a response roughly matches expectations.</returns>
bool IsBenchmarkProbePass(const BenchmarkProbe& pBenchmarkProbe, const std::string& pSTextResponse) {
  if (pBenchmarkProbe.pVecSExpectedContainsAny.empty()) {
    return !Trim(pSTextResponse).empty();
  }
  const std::string lSNormalizedActual = NormalizeForMatch(pSTextResponse);
  for (const std::string& lSExpectedHint : pBenchmarkProbe.pVecSExpectedContainsAny) {
    const std::string lSNormalizedExpected = NormalizeForMatch(lSExpectedHint);
    if (!lSNormalizedExpected.empty() && lSNormalizedActual.find(lSNormalizedExpected) != std::string::npos) {
      return true;
    }
  }
  return false;
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

enum class AutoTuneEvaluationMode {
  BuiltInBenchmarks,
  QuestionAnswerFolder
};

struct AutoTuneWizardConfig {
  std::string pSModelName;
  AutoTuneEvaluationMode pAutoTuneEvaluationMode = AutoTuneEvaluationMode::BuiltInBenchmarks;
  std::filesystem::path pPathQuestionAnswerFolder;
  int piRunsPerSetting = 3;
  double pdTargetTokensPerSecond = 0.0;
  double pdTargetLatencyMilliseconds = 0.0;
};

struct AutoTuneEvaluationSummary {
  bool pbOk = false;
  std::size_t piPassCount = 0;
  std::size_t piCaseCount = 0;
  double pdAverageScore = 0.0;
  double pdAverageTokensPerSecond = 0.0;
  double pdAverageTotalMilliseconds = 0.0;
  double pdAverageTtftMilliseconds = 0.0;
  std::string pSError;
};

struct AutoTuneTemplate {
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

/// <summary>Formats generation settings in plain language for non-AI developers.</summary>
/// <param name="pGenerationSettings">Generation settings payload.</param>
/// <returns>Human-readable settings line.</returns>
std::string DescribeGenerationSettings(const ollama_engine::GenerationSettings& pGenerationSettings) {
  std::ostringstream lStream;
  lStream << "temperature=" << pGenerationSettings.pfTemperature << ", top_p=" << pGenerationSettings.pfTopP
          << ", min_p=" << pGenerationSettings.pfMinP << ", top_k=" << pGenerationSettings.piTopK
          << ", repeat_penalty=" << pGenerationSettings.pfRepeatPenalty
          << ", seed=" << pGenerationSettings.piSeed;
  return lStream.str();
}

/// <summary>Parses a line as integer with fallback default.</summary>
/// <param name="pSInput">Input text.</param>
/// <param name="piDefaultValue">Default fallback.</param>
/// <returns>Parsed or default integer.</returns>
int ParseIntOrDefault(const std::string& pSInput, const int piDefaultValue) {
  const std::string lSTrimmed = Trim(pSInput);
  if (lSTrimmed.empty()) {
    return piDefaultValue;
  }
  char* lPtrEnd = nullptr;
  const long liValue = std::strtol(lSTrimmed.c_str(), &lPtrEnd, 10);
  if (lPtrEnd == nullptr || *lPtrEnd != '\0') {
    return piDefaultValue;
  }
  return static_cast<int>(liValue);
}

/// <summary>Parses a line as uint32 with fallback default.</summary>
/// <param name="pSInput">Input text.</param>
/// <param name="piDefaultValue">Default fallback.</param>
/// <returns>Parsed or default uint32.</returns>
std::uint32_t ParseUnsigned32OrDefault(const std::string& pSInput, const std::uint32_t piDefaultValue) {
  const std::string lSTrimmed = Trim(pSInput);
  if (lSTrimmed.empty()) {
    return piDefaultValue;
  }
  char* lPtrEnd = nullptr;
  const unsigned long long lliValue = std::strtoull(lSTrimmed.c_str(), &lPtrEnd, 10);
  if (lPtrEnd == nullptr || *lPtrEnd != '\0') {
    return piDefaultValue;
  }
  if (lliValue > static_cast<unsigned long long>(std::numeric_limits<std::uint32_t>::max())) {
    return piDefaultValue;
  }
  return static_cast<std::uint32_t>(lliValue);
}

/// <summary>Parses a line as double with fallback default.</summary>
/// <param name="pSInput">Input text.</param>
/// <param name="pdDefaultValue">Default fallback.</param>
/// <returns>Parsed or default double.</returns>
double ParseDoubleOrDefault(const std::string& pSInput, const double pdDefaultValue) {
  const std::string lSTrimmed = Trim(pSInput);
  if (lSTrimmed.empty()) {
    return pdDefaultValue;
  }
  char* lPtrEnd = nullptr;
  const double ldValue = std::strtod(lSTrimmed.c_str(), &lPtrEnd);
  if (lPtrEnd == nullptr || *lPtrEnd != '\0') {
    return pdDefaultValue;
  }
  return ldValue;
}

/// <summary>Reads a yes/no answer with default fallback.</summary>
/// <param name="pSPrompt">Prompt shown to user.</param>
/// <param name="pbDefault">Default value when input is empty/invalid.</param>
/// <returns>Parsed yes/no decision.</returns>
bool ReadYesNo(const std::string& pSPrompt, const bool pbDefault) {
  const std::string lSInput = Trim(ReadLine(pSPrompt));
  if (lSInput.empty()) {
    return pbDefault;
  }
  const std::string lSNormalized = NormalizeForMatch(lSInput);
  if (lSNormalized == "y" || lSNormalized == "yes" || lSNormalized == "true") {
    return true;
  }
  if (lSNormalized == "n" || lSNormalized == "no" || lSNormalized == "false") {
    return false;
  }
  return pbDefault;
}

/// <summary>Builds a portable OS label.</summary>
/// <returns>Operating system label.</returns>
std::string GetOsLabel() {
#if defined(_WIN32)
  return "Windows";
#elif defined(__APPLE__)
  return "macOS";
#elif defined(__linux__)
  return "Linux";
#else
  return "UnknownOS";
#endif
}

/// <summary>Builds a portable architecture label.</summary>
/// <returns>Architecture label.</returns>
std::string GetArchLabel() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x64";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#else
  return "unknown-arch";
#endif
}

/// <summary>Builds a hardware fingerprint suitable for template matching.</summary>
/// <returns>Fingerprint string.</returns>
std::string BuildHardwareFingerprint() {
  std::ostringstream lStream;
  lStream << GetOsLabel() << "-" << GetArchLabel() << "-threads" << std::thread::hardware_concurrency();
  return lStream.str();
}

/// <summary>Builds a human-readable hardware summary string.</summary>
/// <returns>Hardware summary.</returns>
std::string BuildHardwareSummary() {
  std::ostringstream lStream;
  lStream << "OS=" << GetOsLabel() << ", Arch=" << GetArchLabel()
          << ", Threads=" << std::thread::hardware_concurrency();
  return lStream.str();
}

/// <summary>Returns UTC timestamp in ISO-8601 format.</summary>
/// <returns>UTC timestamp text.</returns>
std::string BuildUtcIsoTimestamp() {
  const std::time_t liNow = std::time(nullptr);
  std::tm lUtcTm{};
#if defined(_WIN32)
  gmtime_s(&lUtcTm, &liNow);
#else
  gmtime_r(&liNow, &lUtcTm);
#endif
  std::ostringstream lStream;
  lStream << std::put_time(&lUtcTm, "%Y-%m-%dT%H:%M:%SZ");
  return lStream.str();
}

/// <summary>Sanitizes user text to a filesystem-safe template base name.</summary>
/// <param name="pSInput">Raw template name input.</param>
/// <returns>Sanitized template name.</returns>
std::string SanitizeTemplateName(const std::string& pSInput) {
  std::string lSName;
  lSName.reserve(pSInput.size());
  for (const unsigned char lCChar : pSInput) {
    if (std::isalnum(lCChar) != 0 || lCChar == '-' || lCChar == '_') {
      lSName.push_back(static_cast<char>(lCChar));
    } else if (std::isspace(lCChar) != 0) {
      lSName.push_back('_');
    }
  }
  if (lSName.empty()) {
    lSName = "autotune_template";
  }
  return lSName;
}

/// <summary>Returns directory where `.xml` autotune templates are persisted.</summary>
/// <param name="pPathModelFolder">Model folder path.</param>
/// <returns>Template directory path.</returns>
std::filesystem::path GetAutoTuneTemplateDirectory(const std::filesystem::path& pPathModelFolder) {
  return pPathModelFolder / "autotune_templates";
}

/// <summary>Returns directory where finalized `.ole` profiles are persisted.</summary>
/// <param name="pPathModelFolder">Model folder path.</param>
/// <returns>Finalized profile directory path.</returns>
std::filesystem::path GetAutoTuneFinalProfileDirectory(const std::filesystem::path& pPathModelFolder) {
  return pPathModelFolder / "autotune_final_profiles";
}

/// <summary>Escapes text for safe inclusion in XML element values.</summary>
/// <param name="pSValue">Raw text.</param>
/// <returns>Escaped XML text.</returns>
std::string EscapeXmlText(const std::string& pSValue) {
  std::string lSOutput;
  lSOutput.reserve(pSValue.size());
  for (const char lCChar : pSValue) {
    switch (lCChar) {
      case '&':
        lSOutput += "&amp;";
        break;
      case '<':
        lSOutput += "&lt;";
        break;
      case '>':
        lSOutput += "&gt;";
        break;
      case '"':
        lSOutput += "&quot;";
        break;
      case '\'':
        lSOutput += "&apos;";
        break;
      default:
        lSOutput.push_back(lCChar);
        break;
    }
  }
  return lSOutput;
}

/// <summary>Unescapes simple XML entities from element text.</summary>
/// <param name="pSValue">Escaped XML text.</param>
/// <returns>Unescaped text.</returns>
std::string UnescapeXmlText(std::string pSValue) {
  auto ReplaceAll = [](std::string* pSInOut, const std::string& pSFrom, const std::string& pSTo) {
    if (pSInOut == nullptr || pSFrom.empty()) {
      return;
    }
    std::size_t liPosition = 0;
    while (true) {
      liPosition = pSInOut->find(pSFrom, liPosition);
      if (liPosition == std::string::npos) {
        break;
      }
      pSInOut->replace(liPosition, pSFrom.size(), pSTo);
      liPosition += pSTo.size();
    }
  };

  ReplaceAll(&pSValue, "&lt;", "<");
  ReplaceAll(&pSValue, "&gt;", ">");
  ReplaceAll(&pSValue, "&quot;", "\"");
  ReplaceAll(&pSValue, "&apos;", "'");
  ReplaceAll(&pSValue, "&amp;", "&");
  return pSValue;
}

/// <summary>Extracts one XML element text value from a simple flat document.</summary>
/// <param name="pSXmlText">Full XML text.</param>
/// <param name="pSTagName">Tag to read.</param>
/// <returns>Element value when tag exists.</returns>
std::optional<std::string> ExtractXmlTagValue(const std::string& pSXmlText, const std::string& pSTagName) {
  const std::string lSStartTag = "<" + pSTagName + ">";
  const std::string lSEndTag = "</" + pSTagName + ">";
  const std::size_t liStart = pSXmlText.find(lSStartTag);
  if (liStart == std::string::npos) {
    return std::nullopt;
  }
  const std::size_t liContentStart = liStart + lSStartTag.size();
  const std::size_t liEnd = pSXmlText.find(lSEndTag, liContentStart);
  if (liEnd == std::string::npos) {
    return std::nullopt;
  }
  return UnescapeXmlText(pSXmlText.substr(liContentStart, liEnd - liContentStart));
}

/// <summary>Saves an autotune template as a simple `.xml` file.</summary>
/// <param name="pAutoTuneTemplate">Template payload to persist.</param>
/// <param name="pPathDirectory">Output directory.</param>
/// <param name="pSErrorOut">Optional output pointer for error details.</param>
/// <returns>True on successful write.</returns>
bool SaveAutoTuneTemplateXmlFile(const AutoTuneTemplate& pAutoTuneTemplate,
                                 const std::filesystem::path& pPathDirectory, std::string* pSErrorOut) {
  std::error_code lErrorCode;
  std::filesystem::create_directories(pPathDirectory, lErrorCode);
  if (lErrorCode) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Failed to create template directory: " + pPathDirectory.string();
    }
    return false;
  }

  const std::filesystem::path lPathTemplateFile = pPathDirectory / (pAutoTuneTemplate.pSName + ".xml");
  std::ofstream lFileOut(lPathTemplateFile, std::ios::out | std::ios::trunc);
  if (!lFileOut.good()) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Failed to open template file for writing: " + lPathTemplateFile.string();
    }
    return false;
  }

  lFileOut << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  lFileOut << "<AutoTuneTemplate version=\"1\">\n";
  lFileOut << "  <TemplateName>" << EscapeXmlText(pAutoTuneTemplate.pSName) << "</TemplateName>\n";
  lFileOut << "  <CreatedUtc>" << EscapeXmlText(pAutoTuneTemplate.pSCreatedUtc) << "</CreatedUtc>\n";
  lFileOut << "  <ModelName>" << EscapeXmlText(pAutoTuneTemplate.pSModelName) << "</ModelName>\n";
  lFileOut << "  <HardwareFingerprint>" << EscapeXmlText(pAutoTuneTemplate.pSHardwareFingerprint)
           << "</HardwareFingerprint>\n";
  lFileOut << "  <HardwareSummary>" << EscapeXmlText(pAutoTuneTemplate.pSHardwareSummary) << "</HardwareSummary>\n";
  lFileOut << "  <Temperature>" << pAutoTuneTemplate.pGenerationSettings.pfTemperature << "</Temperature>\n";
  lFileOut << "  <TopP>" << pAutoTuneTemplate.pGenerationSettings.pfTopP << "</TopP>\n";
  lFileOut << "  <MinP>" << pAutoTuneTemplate.pGenerationSettings.pfMinP << "</MinP>\n";
  lFileOut << "  <TopK>" << pAutoTuneTemplate.pGenerationSettings.piTopK << "</TopK>\n";
  lFileOut << "  <RepeatPenalty>" << pAutoTuneTemplate.pGenerationSettings.pfRepeatPenalty << "</RepeatPenalty>\n";
  lFileOut << "  <Seed>" << pAutoTuneTemplate.pGenerationSettings.piSeed << "</Seed>\n";
  lFileOut << "  <RunsPerSetting>" << pAutoTuneTemplate.piRunsPerSetting << "</RunsPerSetting>\n";
  lFileOut << "  <EvaluationMode>" << EscapeXmlText(pAutoTuneTemplate.pSEvaluationMode) << "</EvaluationMode>\n";
  lFileOut << "  <EvaluationSource>" << EscapeXmlText(pAutoTuneTemplate.pSEvaluationSource)
           << "</EvaluationSource>\n";
  lFileOut << "  <AverageScore>" << pAutoTuneTemplate.pdAverageScore << "</AverageScore>\n";
  lFileOut << "  <AverageTokPerSec>" << pAutoTuneTemplate.pdAverageTokensPerSecond << "</AverageTokPerSec>\n";
  lFileOut << "  <AverageTotalMs>" << pAutoTuneTemplate.pdAverageTotalMilliseconds << "</AverageTotalMs>\n";
  lFileOut << "  <AverageTtftMs>" << pAutoTuneTemplate.pdAverageTtftMilliseconds << "</AverageTtftMs>\n";
  lFileOut << "</AutoTuneTemplate>\n";
  return true;
}

/// <summary>Saves finalized best settings as `.ole` profile for cross-project hardware matching.</summary>
/// <param name="pAutoTuneTemplate">Finalized profile payload to persist.</param>
/// <param name="pPathDirectory">Output directory.</param>
/// <param name="pSErrorOut">Optional output pointer for error details.</param>
/// <returns>True on successful write.</returns>
bool SaveAutoTuneFinalProfileOleFile(const AutoTuneTemplate& pAutoTuneTemplate,
                                     const std::filesystem::path& pPathDirectory, std::string* pSErrorOut) {
  std::error_code lErrorCode;
  std::filesystem::create_directories(pPathDirectory, lErrorCode);
  if (lErrorCode) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Failed to create final profile directory: " + pPathDirectory.string();
    }
    return false;
  }

  const std::filesystem::path lPathTemplateFile = pPathDirectory / (pAutoTuneTemplate.pSName + ".ole");
  std::ofstream lFileOut(lPathTemplateFile, std::ios::out | std::ios::trunc);
  if (!lFileOut.good()) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Failed to open final profile file for writing: " + lPathTemplateFile.string();
    }
    return false;
  }

  lFileOut << "OLE_VERSION=1\n";
  lFileOut << "TemplateName=" << pAutoTuneTemplate.pSName << "\n";
  lFileOut << "CreatedUtc=" << pAutoTuneTemplate.pSCreatedUtc << "\n";
  lFileOut << "ModelName=" << pAutoTuneTemplate.pSModelName << "\n";
  lFileOut << "HardwareFingerprint=" << pAutoTuneTemplate.pSHardwareFingerprint << "\n";
  lFileOut << "HardwareSummary=" << pAutoTuneTemplate.pSHardwareSummary << "\n";
  lFileOut << "Temperature=" << pAutoTuneTemplate.pGenerationSettings.pfTemperature << "\n";
  lFileOut << "TopP=" << pAutoTuneTemplate.pGenerationSettings.pfTopP << "\n";
  lFileOut << "MinP=" << pAutoTuneTemplate.pGenerationSettings.pfMinP << "\n";
  lFileOut << "TopK=" << pAutoTuneTemplate.pGenerationSettings.piTopK << "\n";
  lFileOut << "RepeatPenalty=" << pAutoTuneTemplate.pGenerationSettings.pfRepeatPenalty << "\n";
  lFileOut << "Seed=" << pAutoTuneTemplate.pGenerationSettings.piSeed << "\n";
  lFileOut << "RunsPerSetting=" << pAutoTuneTemplate.piRunsPerSetting << "\n";
  lFileOut << "EvaluationMode=" << pAutoTuneTemplate.pSEvaluationMode << "\n";
  lFileOut << "EvaluationSource=" << pAutoTuneTemplate.pSEvaluationSource << "\n";
  lFileOut << "AverageScore=" << pAutoTuneTemplate.pdAverageScore << "\n";
  lFileOut << "AverageTokPerSec=" << pAutoTuneTemplate.pdAverageTokensPerSecond << "\n";
  lFileOut << "AverageTotalMs=" << pAutoTuneTemplate.pdAverageTotalMilliseconds << "\n";
  lFileOut << "AverageTtftMs=" << pAutoTuneTemplate.pdAverageTtftMilliseconds << "\n";
  return true;
}

/// <summary>Loads one `.xml` autotune template file.</summary>
/// <param name="pPathTemplateFile">Template file path.</param>
/// <returns>Parsed template, or nullopt on parse failure.</returns>
std::optional<AutoTuneTemplate> LoadAutoTuneTemplateXmlFile(const std::filesystem::path& pPathTemplateFile) {
  std::ifstream lFileIn(pPathTemplateFile, std::ios::in);
  if (!lFileIn.good()) {
    return std::nullopt;
  }

  std::ostringstream lBuffer;
  lBuffer << lFileIn.rdbuf();
  const std::string lSXmlText = lBuffer.str();
  if (lSXmlText.empty()) {
    return std::nullopt;
  }

  AutoTuneTemplate lAutoTuneTemplate;
  lAutoTuneTemplate.pSName =
      ExtractXmlTagValue(lSXmlText, "TemplateName").value_or(pPathTemplateFile.stem().string());
  lAutoTuneTemplate.pSCreatedUtc = ExtractXmlTagValue(lSXmlText, "CreatedUtc").value_or("");
  lAutoTuneTemplate.pSModelName = ExtractXmlTagValue(lSXmlText, "ModelName").value_or("");
  lAutoTuneTemplate.pSHardwareFingerprint = ExtractXmlTagValue(lSXmlText, "HardwareFingerprint").value_or("");
  lAutoTuneTemplate.pSHardwareSummary = ExtractXmlTagValue(lSXmlText, "HardwareSummary").value_or("");
  lAutoTuneTemplate.pGenerationSettings.pfTemperature =
      static_cast<float>(ParseDoubleOrDefault(ExtractXmlTagValue(lSXmlText, "Temperature").value_or(""), 0.8));
  lAutoTuneTemplate.pGenerationSettings.pfTopP =
      static_cast<float>(ParseDoubleOrDefault(ExtractXmlTagValue(lSXmlText, "TopP").value_or(""), 0.95));
  lAutoTuneTemplate.pGenerationSettings.pfMinP =
      static_cast<float>(ParseDoubleOrDefault(ExtractXmlTagValue(lSXmlText, "MinP").value_or(""), 0.05));
  lAutoTuneTemplate.pGenerationSettings.piTopK =
      ParseIntOrDefault(ExtractXmlTagValue(lSXmlText, "TopK").value_or(""), 40);
  lAutoTuneTemplate.pGenerationSettings.pfRepeatPenalty =
      static_cast<float>(ParseDoubleOrDefault(ExtractXmlTagValue(lSXmlText, "RepeatPenalty").value_or(""), 1.0));
  lAutoTuneTemplate.pGenerationSettings.piSeed =
      ParseUnsigned32OrDefault(ExtractXmlTagValue(lSXmlText, "Seed").value_or(""), 4294967295U);
  lAutoTuneTemplate.piRunsPerSetting =
      ParseIntOrDefault(ExtractXmlTagValue(lSXmlText, "RunsPerSetting").value_or(""), 3);
  lAutoTuneTemplate.pSEvaluationMode = ExtractXmlTagValue(lSXmlText, "EvaluationMode").value_or("");
  lAutoTuneTemplate.pSEvaluationSource = ExtractXmlTagValue(lSXmlText, "EvaluationSource").value_or("");
  lAutoTuneTemplate.pdAverageScore =
      ParseDoubleOrDefault(ExtractXmlTagValue(lSXmlText, "AverageScore").value_or(""), 0.0);
  lAutoTuneTemplate.pdAverageTokensPerSecond =
      ParseDoubleOrDefault(ExtractXmlTagValue(lSXmlText, "AverageTokPerSec").value_or(""), 0.0);
  lAutoTuneTemplate.pdAverageTotalMilliseconds =
      ParseDoubleOrDefault(ExtractXmlTagValue(lSXmlText, "AverageTotalMs").value_or(""), 0.0);
  lAutoTuneTemplate.pdAverageTtftMilliseconds =
      ParseDoubleOrDefault(ExtractXmlTagValue(lSXmlText, "AverageTtftMs").value_or(""), 0.0);
  return lAutoTuneTemplate;
}

/// <summary>Loads one finalized `.ole` profile file.</summary>
/// <param name="pPathTemplateFile">Profile file path.</param>
/// <returns>Parsed profile, or nullopt on parse failure.</returns>
std::optional<AutoTuneTemplate> LoadAutoTuneFinalProfileOleFile(const std::filesystem::path& pPathTemplateFile) {
  std::ifstream lFileIn(pPathTemplateFile, std::ios::in);
  if (!lFileIn.good()) {
    return std::nullopt;
  }

  std::unordered_map<std::string, std::string> lMapValues;
  std::string lSLine;
  while (std::getline(lFileIn, lSLine)) {
    const std::size_t liSeparator = lSLine.find('=');
    if (liSeparator == std::string::npos) {
      continue;
    }
    const std::string lSKey = Trim(lSLine.substr(0, liSeparator));
    const std::string lSValue = Trim(lSLine.substr(liSeparator + 1));
    if (!lSKey.empty()) {
      lMapValues[lSKey] = lSValue;
    }
  }
  if (lMapValues.empty()) {
    return std::nullopt;
  }

  AutoTuneTemplate lAutoTuneTemplate;
  lAutoTuneTemplate.pSName =
      lMapValues.count("TemplateName") > 0 ? lMapValues["TemplateName"] : pPathTemplateFile.stem().string();
  lAutoTuneTemplate.pSCreatedUtc = lMapValues["CreatedUtc"];
  lAutoTuneTemplate.pSModelName = lMapValues["ModelName"];
  lAutoTuneTemplate.pSHardwareFingerprint = lMapValues["HardwareFingerprint"];
  lAutoTuneTemplate.pSHardwareSummary = lMapValues["HardwareSummary"];
  lAutoTuneTemplate.pGenerationSettings.pfTemperature =
      static_cast<float>(ParseDoubleOrDefault(lMapValues["Temperature"], 0.8));
  lAutoTuneTemplate.pGenerationSettings.pfTopP =
      static_cast<float>(ParseDoubleOrDefault(lMapValues["TopP"], 0.95));
  lAutoTuneTemplate.pGenerationSettings.pfMinP =
      static_cast<float>(ParseDoubleOrDefault(lMapValues["MinP"], 0.05));
  lAutoTuneTemplate.pGenerationSettings.piTopK = ParseIntOrDefault(lMapValues["TopK"], 40);
  lAutoTuneTemplate.pGenerationSettings.pfRepeatPenalty =
      static_cast<float>(ParseDoubleOrDefault(lMapValues["RepeatPenalty"], 1.0));
  lAutoTuneTemplate.pGenerationSettings.piSeed = ParseUnsigned32OrDefault(lMapValues["Seed"], 4294967295U);
  lAutoTuneTemplate.piRunsPerSetting = ParseIntOrDefault(lMapValues["RunsPerSetting"], 3);
  lAutoTuneTemplate.pSEvaluationMode = lMapValues["EvaluationMode"];
  lAutoTuneTemplate.pSEvaluationSource = lMapValues["EvaluationSource"];
  lAutoTuneTemplate.pdAverageScore = ParseDoubleOrDefault(lMapValues["AverageScore"], 0.0);
  lAutoTuneTemplate.pdAverageTokensPerSecond = ParseDoubleOrDefault(lMapValues["AverageTokPerSec"], 0.0);
  lAutoTuneTemplate.pdAverageTotalMilliseconds = ParseDoubleOrDefault(lMapValues["AverageTotalMs"], 0.0);
  lAutoTuneTemplate.pdAverageTtftMilliseconds = ParseDoubleOrDefault(lMapValues["AverageTtftMs"], 0.0);
  return lAutoTuneTemplate;
}

/// <summary>Loads all `.xml` templates from a directory.</summary>
/// <param name="pPathDirectory">Directory containing template files.</param>
/// <returns>Sorted loaded templates.</returns>
std::vector<AutoTuneTemplate> LoadAutoTuneTemplates(const std::filesystem::path& pPathDirectory) {
  std::vector<AutoTuneTemplate> lVecAutoTuneTemplates;
  std::error_code lErrorCode;
  if (!std::filesystem::exists(pPathDirectory, lErrorCode) ||
      !std::filesystem::is_directory(pPathDirectory, lErrorCode)) {
    return lVecAutoTuneTemplates;
  }

  for (const std::filesystem::directory_entry& lDirectoryEntry :
       std::filesystem::directory_iterator(pPathDirectory, lErrorCode)) {
    if (lErrorCode || !lDirectoryEntry.is_regular_file(lErrorCode)) {
      continue;
    }
    std::string lSExtension = lDirectoryEntry.path().extension().string();
    std::transform(lSExtension.begin(), lSExtension.end(), lSExtension.begin(),
                   [](unsigned char pCChar) { return static_cast<char>(std::tolower(pCChar)); });
    if (lSExtension != ".xml") {
      continue;
    }
    const std::optional<AutoTuneTemplate> lOptTemplate = LoadAutoTuneTemplateXmlFile(lDirectoryEntry.path());
    if (lOptTemplate.has_value()) {
      lVecAutoTuneTemplates.push_back(*lOptTemplate);
    }
  }

  std::sort(lVecAutoTuneTemplates.begin(), lVecAutoTuneTemplates.end(),
            [](const AutoTuneTemplate& pLhs, const AutoTuneTemplate& pRhs) {
              if (pLhs.pSName != pRhs.pSName) {
                return pLhs.pSName < pRhs.pSName;
              }
              return pLhs.pSCreatedUtc < pRhs.pSCreatedUtc;
            });
  return lVecAutoTuneTemplates;
}

/// <summary>Loads all finalized `.ole` profiles from a directory.</summary>
/// <param name="pPathDirectory">Directory containing profile files.</param>
/// <returns>Sorted loaded finalized profiles.</returns>
std::vector<AutoTuneTemplate> LoadAutoTuneFinalProfiles(const std::filesystem::path& pPathDirectory) {
  std::vector<AutoTuneTemplate> lVecAutoTuneProfiles;
  std::error_code lErrorCode;
  if (!std::filesystem::exists(pPathDirectory, lErrorCode) ||
      !std::filesystem::is_directory(pPathDirectory, lErrorCode)) {
    return lVecAutoTuneProfiles;
  }

  for (const std::filesystem::directory_entry& lDirectoryEntry :
       std::filesystem::directory_iterator(pPathDirectory, lErrorCode)) {
    if (lErrorCode || !lDirectoryEntry.is_regular_file(lErrorCode)) {
      continue;
    }
    std::string lSExtension = lDirectoryEntry.path().extension().string();
    std::transform(lSExtension.begin(), lSExtension.end(), lSExtension.begin(),
                   [](unsigned char pCChar) { return static_cast<char>(std::tolower(pCChar)); });
    if (lSExtension != ".ole") {
      continue;
    }
    const std::optional<AutoTuneTemplate> lOptProfile = LoadAutoTuneFinalProfileOleFile(lDirectoryEntry.path());
    if (lOptProfile.has_value()) {
      lVecAutoTuneProfiles.push_back(*lOptProfile);
    }
  }

  std::sort(lVecAutoTuneProfiles.begin(), lVecAutoTuneProfiles.end(),
            [](const AutoTuneTemplate& pLhs, const AutoTuneTemplate& pRhs) {
              if (pLhs.pSName != pRhs.pSName) {
                return pLhs.pSName < pRhs.pSName;
              }
              return pLhs.pSCreatedUtc < pRhs.pSCreatedUtc;
            });
  return lVecAutoTuneProfiles;
}

/// <summary>Finds best matching template for model + current hardware fingerprint.</summary>
/// <param name="pVecAutoTuneTemplates">Template list.</param>
/// <param name="pSModelName">Loaded model name.</param>
/// <param name="pSHardwareFingerprint">Current hardware fingerprint.</param>
/// <returns>Best matching template, or nullopt when no match exists.</returns>
std::optional<AutoTuneTemplate> FindBestMatchingTemplate(const std::vector<AutoTuneTemplate>& pVecAutoTuneTemplates,
                                                         const std::string& pSModelName,
                                                         const std::string& pSHardwareFingerprint) {
  std::optional<AutoTuneTemplate> lOptBestTemplate;
  for (const AutoTuneTemplate& lAutoTuneTemplate : pVecAutoTuneTemplates) {
    if (lAutoTuneTemplate.pSModelName != pSModelName) {
      continue;
    }
    if (lAutoTuneTemplate.pSHardwareFingerprint != pSHardwareFingerprint) {
      continue;
    }
    if (!lOptBestTemplate.has_value() || lAutoTuneTemplate.pSCreatedUtc > lOptBestTemplate->pSCreatedUtc) {
      lOptBestTemplate = lAutoTuneTemplate;
    }
  }
  return lOptBestTemplate;
}

/// <summary>Builds autotune candidate generation settings.</summary>
/// <returns>Candidate settings list.</returns>
std::vector<ollama_engine::GenerationSettings> BuildAutoTuneCandidates() {
  std::vector<ollama_engine::GenerationSettings> lVecGenerationSettings;
  const std::vector<float> lVecfTemperature = {0.3f, 0.6f, 0.9f, 1.1f};
  const std::vector<float> lVecfTopP = {0.90f, 0.95f};
  const std::vector<float> lVecfMinP = {0.02f, 0.05f};
  const std::vector<float> lVecfRepeatPenalty = {1.0f, 1.1f};
  for (const float lfTemperature : lVecfTemperature) {
    for (const float lfTopP : lVecfTopP) {
      for (const float lfMinP : lVecfMinP) {
        for (const float lfRepeatPenalty : lVecfRepeatPenalty) {
          ollama_engine::GenerationSettings lGenerationSettings;
          lGenerationSettings.pfTemperature = lfTemperature;
          lGenerationSettings.pfTopP = lfTopP;
          lGenerationSettings.pfMinP = lfMinP;
          lGenerationSettings.piTopK = 40;
          lGenerationSettings.pfRepeatPenalty = lfRepeatPenalty;
          lGenerationSettings.piSeed = 4294967295U;
          lVecGenerationSettings.push_back(lGenerationSettings);
        }
      }
    }
  }
  return lVecGenerationSettings;
}

/// <summary>Evaluates one setting set against configured autotune workload.</summary>
/// <param name="pPtrEngine">Engine pointer.</param>
/// <param name="pAutoTuneWizardConfig">Wizard configuration.</param>
/// <returns>Aggregated evaluation summary.</returns>
AutoTuneEvaluationSummary EvaluateAutoTuneSettings(ollama_engine::EngineInterface* pPtrEngine,
                                                   const AutoTuneWizardConfig& pAutoTuneWizardConfig) {
  AutoTuneEvaluationSummary lAutoTuneEvaluationSummary;
  if (pPtrEngine == nullptr) {
    lAutoTuneEvaluationSummary.pSError = "Engine pointer is null.";
    return lAutoTuneEvaluationSummary;
  }

  std::size_t liPassCount = 0;
  std::size_t liCaseCount = 0;
  double ldTotalMs = 0.0;
  double ldTotalTtftMs = 0.0;
  double ldTotalTokPerSec = 0.0;

  if (pAutoTuneWizardConfig.pAutoTuneEvaluationMode == AutoTuneEvaluationMode::BuiltInBenchmarks) {
    const std::vector<BenchmarkProbe> lVecBenchmarkProbes = BuildStandardOpenSourceBenchmarkProbes();
    if (lVecBenchmarkProbes.empty()) {
      lAutoTuneEvaluationSummary.pSError = "No built-in benchmark probes are available.";
      return lAutoTuneEvaluationSummary;
    }
    for (int liRun = 0; liRun < pAutoTuneWizardConfig.piRunsPerSetting; ++liRun) {
      for (const BenchmarkProbe& lBenchmarkProbe : lVecBenchmarkProbes) {
        const PromptRunResult lPromptRunResult = RunPromptWithMetrics(pPtrEngine, lBenchmarkProbe.pSPrompt);
        ++liCaseCount;
        ldTotalMs += lPromptRunResult.pdTotalMilliseconds;
        ldTotalTtftMs += lPromptRunResult.pdTimeToFirstTokenMilliseconds;
        ldTotalTokPerSec += lPromptRunResult.pdTokensPerSecond;
        if (lPromptRunResult.pSendMessageResponse.pbOk &&
            IsBenchmarkProbePass(lBenchmarkProbe, lPromptRunResult.pSendMessageResponse.pSText)) {
          ++liPassCount;
        }
      }
    }
  } else {
    const std::vector<QuestionAnswerCase> lVecQuestionAnswerCases =
        LoadQuestionAnswerCases(pAutoTuneWizardConfig.pPathQuestionAnswerFolder);
    if (lVecQuestionAnswerCases.empty()) {
      lAutoTuneEvaluationSummary.pSError =
          "No Question_N test pairs found in " + pAutoTuneWizardConfig.pPathQuestionAnswerFolder.string();
      return lAutoTuneEvaluationSummary;
    }
    for (int liRun = 0; liRun < pAutoTuneWizardConfig.piRunsPerSetting; ++liRun) {
      for (const QuestionAnswerCase& lQuestionAnswerCase : lVecQuestionAnswerCases) {
        const PromptRunResult lPromptRunResult = RunPromptWithMetrics(pPtrEngine, lQuestionAnswerCase.pSQuestionText);
        ++liCaseCount;
        ldTotalMs += lPromptRunResult.pdTotalMilliseconds;
        ldTotalTtftMs += lPromptRunResult.pdTimeToFirstTokenMilliseconds;
        ldTotalTokPerSec += lPromptRunResult.pdTokensPerSecond;

        const std::string lSActual = NormalizeForMatch(lPromptRunResult.pSendMessageResponse.pSText);
        const std::string lSExpected = NormalizeForMatch(lQuestionAnswerCase.pSAnswerText);
        const bool lbPass = lPromptRunResult.pSendMessageResponse.pbOk && !lSExpected.empty() &&
                            (lSActual.find(lSExpected) != std::string::npos);
        if (lbPass) {
          ++liPassCount;
        }
      }
    }
  }

  if (liCaseCount == 0) {
    lAutoTuneEvaluationSummary.pSError = "No autotune evaluation cases were executed.";
    return lAutoTuneEvaluationSummary;
  }

  lAutoTuneEvaluationSummary.pbOk = true;
  lAutoTuneEvaluationSummary.piPassCount = liPassCount;
  lAutoTuneEvaluationSummary.piCaseCount = liCaseCount;
  lAutoTuneEvaluationSummary.pdAverageScore = static_cast<double>(liPassCount) / static_cast<double>(liCaseCount);
  lAutoTuneEvaluationSummary.pdAverageTotalMilliseconds = ldTotalMs / static_cast<double>(liCaseCount);
  lAutoTuneEvaluationSummary.pdAverageTtftMilliseconds = ldTotalTtftMs / static_cast<double>(liCaseCount);
  lAutoTuneEvaluationSummary.pdAverageTokensPerSecond = ldTotalTokPerSec / static_cast<double>(liCaseCount);
  return lAutoTuneEvaluationSummary;
}

/// <summary>Computes objective score from quality/speed/latency priorities.</summary>
/// <param name="pAutoTuneEvaluationSummary">Evaluation result payload.</param>
/// <param name="pAutoTuneWizardConfig">Wizard targets.</param>
/// <returns>Composite score in approximately [0,1].</returns>
double ComputeAutoTuneObjectiveScore(const AutoTuneEvaluationSummary& pAutoTuneEvaluationSummary,
                                     const AutoTuneWizardConfig& pAutoTuneWizardConfig) {
  if (!pAutoTuneEvaluationSummary.pbOk) {
    return 0.0;
  }
  const double ldQualityScore = pAutoTuneEvaluationSummary.pdAverageScore;
  const double ldSpeedScore = (pAutoTuneWizardConfig.pdTargetTokensPerSecond > 0.0)
                                  ? std::clamp(pAutoTuneEvaluationSummary.pdAverageTokensPerSecond /
                                                   pAutoTuneWizardConfig.pdTargetTokensPerSecond,
                                               0.0, 1.0)
                                  : 0.5;
  const double ldLatencyScore = (pAutoTuneWizardConfig.pdTargetLatencyMilliseconds > 0.0)
                                    ? std::clamp(pAutoTuneWizardConfig.pdTargetLatencyMilliseconds /
                                                     std::max(1.0, pAutoTuneEvaluationSummary.pdAverageTotalMilliseconds),
                                                 0.0, 1.0)
                                    : 0.5;
  return (ldQualityScore * 0.70) + (ldSpeedScore * 0.15) + (ldLatencyScore * 0.15);
}

/// <summary>Prints available autotune templates.</summary>
/// <param name="pVecAutoTuneTemplates">Template list.</param>
/// <param name="pSLoadedModelName">Current loaded model name.</param>
/// <param name="pSHardwareFingerprint">Current hardware fingerprint.</param>
void PrintAutoTuneTemplates(const std::vector<AutoTuneTemplate>& pVecAutoTuneTemplates,
                            const std::string& pSLoadedModelName, const std::string& pSHardwareFingerprint) {
  if (pVecAutoTuneTemplates.empty()) {
    std::cout << "autotune> no templates found.\n";
    return;
  }
  std::cout << "autotune> templates:\n";
  for (std::size_t liIndex = 0; liIndex < pVecAutoTuneTemplates.size(); ++liIndex) {
    const AutoTuneTemplate& lAutoTuneTemplate = pVecAutoTuneTemplates[liIndex];
    const bool lbModelMatch = lAutoTuneTemplate.pSModelName == pSLoadedModelName;
    const bool lbHardwareMatch = lAutoTuneTemplate.pSHardwareFingerprint == pSHardwareFingerprint;
    std::cout << "  [" << (liIndex + 1) << "] " << lAutoTuneTemplate.pSName << " model=" << lAutoTuneTemplate.pSModelName
              << " hardware=" << lAutoTuneTemplate.pSHardwareFingerprint << " created=" << lAutoTuneTemplate.pSCreatedUtc
              << " match(model=" << (lbModelMatch ? "yes" : "no") << ", hardware="
              << (lbHardwareMatch ? "yes" : "no") << ")\n";
  }
}

/// <summary>Prints available finalized autotune profiles.</summary>
/// <param name="pVecAutoTuneProfiles">Finalized profile list.</param>
/// <param name="pSLoadedModelName">Current loaded model name.</param>
/// <param name="pSHardwareFingerprint">Current hardware fingerprint.</param>
void PrintAutoTuneFinalProfiles(const std::vector<AutoTuneTemplate>& pVecAutoTuneProfiles,
                                const std::string& pSLoadedModelName, const std::string& pSHardwareFingerprint) {
  if (pVecAutoTuneProfiles.empty()) {
    std::cout << "autotune> no finalized profiles found.\n";
    return;
  }
  std::cout << "autotune> finalized .ole profiles:\n";
  for (std::size_t liIndex = 0; liIndex < pVecAutoTuneProfiles.size(); ++liIndex) {
    const AutoTuneTemplate& lAutoTuneProfile = pVecAutoTuneProfiles[liIndex];
    const bool lbModelMatch = lAutoTuneProfile.pSModelName == pSLoadedModelName;
    const bool lbHardwareMatch = lAutoTuneProfile.pSHardwareFingerprint == pSHardwareFingerprint;
    std::cout << "  [" << (liIndex + 1) << "] " << lAutoTuneProfile.pSName << " model=" << lAutoTuneProfile.pSModelName
              << " hardware=" << lAutoTuneProfile.pSHardwareFingerprint << " created=" << lAutoTuneProfile.pSCreatedUtc
              << " match(model=" << (lbModelMatch ? "yes" : "no") << ", hardware="
              << (lbHardwareMatch ? "yes" : "no") << ")\n";
  }
}

/// <summary>Resolves an autotune template by one-based index or exact name.</summary>
/// <param name="pSInput">Selection text.</param>
/// <param name="pVecAutoTuneTemplates">Template list.</param>
/// <returns>Matching template, or nullopt.</returns>
std::optional<AutoTuneTemplate> ResolveSelectedAutoTuneTemplate(const std::string& pSInput,
                                                                const std::vector<AutoTuneTemplate>& pVecAutoTuneTemplates) {
  const std::optional<std::size_t> lOptTemplateIndex = ParseModelIndex(Trim(pSInput));
  if (lOptTemplateIndex.has_value() && *lOptTemplateIndex < pVecAutoTuneTemplates.size()) {
    return pVecAutoTuneTemplates[*lOptTemplateIndex];
  }
  for (const AutoTuneTemplate& lAutoTuneTemplate : pVecAutoTuneTemplates) {
    if (lAutoTuneTemplate.pSName == Trim(pSInput)) {
      return lAutoTuneTemplate;
    }
  }
  return std::nullopt;
}

/// <summary>Applies a template by loading its model (if needed) and generation settings.</summary>
/// <param name="pPtrEngine">Engine pointer.</param>
/// <param name="pAutoTuneTemplate">Template payload.</param>
/// <param name="pSLoadedModelNameInOut">Current loaded model name, updated when model is switched.</param>
/// <param name="pSErrorOut">Optional output pointer for error details.</param>
/// <returns>True when template is fully applied.</returns>
bool ApplyAutoTuneTemplate(ollama_engine::EngineInterface* pPtrEngine, const AutoTuneTemplate& pAutoTuneTemplate,
                           std::string* pSLoadedModelNameInOut, std::string* pSErrorOut) {
  if (pPtrEngine == nullptr) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Engine pointer is null.";
    }
    return false;
  }
  if (pSLoadedModelNameInOut == nullptr) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Loaded model pointer is null.";
    }
    return false;
  }

  if (*pSLoadedModelNameInOut != pAutoTuneTemplate.pSModelName) {
    std::string lSLoadError;
    if (!pPtrEngine->Load(pAutoTuneTemplate.pSModelName, &lSLoadError)) {
      if (pSErrorOut != nullptr) {
        *pSErrorOut = "Failed to load template model '" + pAutoTuneTemplate.pSModelName + "': " + lSLoadError;
      }
      return false;
    }
    *pSLoadedModelNameInOut = pAutoTuneTemplate.pSModelName;
  }

  std::string lSSettingsError;
  if (!pPtrEngine->SetGenerationSettings(pAutoTuneTemplate.pGenerationSettings, &lSSettingsError)) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Failed to apply template generation settings: " + lSSettingsError;
    }
    return false;
  }
  return true;
}

/// <summary>Prints plain-language explanation of what autotune optimizes.</summary>
void PrintAutoTunePrimer() {
  std::cout << "autotune> What this means in plain language:\n";
  std::cout << "autotune> - Quality score: how often answers pass your checks.\n";
  std::cout << "autotune> - tok/s target: response speed target (higher is faster).\n";
  std::cout << "autotune> - latency target: full-response time target in milliseconds (lower is better).\n";
  std::cout << "autotune> - runs per setting: how many repeats to average so results are stable.\n";
  std::cout << "autotune> The tuner varies randomness/repetition controls and keeps the best average setup.\n";
}

/// <summary>Runs an interactive autotune wizard and applies best discovered settings.</summary>
/// <param name="pPtrEngine">Engine pointer.</param>
/// <param name="pVecSModels">Model list.</param>
/// <param name="pPathModelFolder">Model directory path.</param>
/// <param name="pSLoadedModelNameInOut">Current loaded model name (updated when switched).</param>
void RunAutoTuneWizard(ollama_engine::EngineInterface* pPtrEngine, const std::vector<std::string>& pVecSModels,
                       const std::filesystem::path& pPathModelFolder, std::string* pSLoadedModelNameInOut) {
  if (pPtrEngine == nullptr) {
    std::cout << "autotune> engine is not available.\n";
    return;
  }
  if (pVecSModels.empty()) {
    std::cout << "autotune> no models found.\n";
    return;
  }

  AutoTuneWizardConfig lAutoTuneWizardConfig;
  lAutoTuneWizardConfig.pSModelName = (pSLoadedModelNameInOut != nullptr) ? *pSLoadedModelNameInOut : pVecSModels.front();

  std::cout << "autotune> Wizard (plain language)\n";
  std::cout << "autotune> We will test multiple generation settings and pick the best average result.\n";
  PrintAutoTunePrimer();
  PrintModels(pVecSModels);
  const std::string lSModelSelectionInput = ReadLine("autotune> pick model (number/name, Enter=current): ");
  if (!Trim(lSModelSelectionInput).empty()) {
    const std::optional<std::string> lOptSelectedModel = ResolveSelectedModel(Trim(lSModelSelectionInput), pVecSModels);
    if (!lOptSelectedModel.has_value()) {
      std::cout << "autotune> invalid model selection.\n";
      return;
    }
    lAutoTuneWizardConfig.pSModelName = *lOptSelectedModel;
  }

  if (pSLoadedModelNameInOut == nullptr || *pSLoadedModelNameInOut != lAutoTuneWizardConfig.pSModelName) {
    std::string lSError;
    if (!pPtrEngine->Load(lAutoTuneWizardConfig.pSModelName, &lSError)) {
      std::cout << "autotune> failed to load model '" << lAutoTuneWizardConfig.pSModelName << "': " << lSError << "\n";
      return;
    }
    if (pSLoadedModelNameInOut != nullptr) {
      *pSLoadedModelNameInOut = lAutoTuneWizardConfig.pSModelName;
    }
  }

  std::cout << "autotune> Evaluation target:\n";
  std::cout << "  1) Built-in open-source mini probes (fast start)\n";
  std::cout << "  2) Custom Question_N test files (best for your domain)\n";
  const int liEvaluationChoice = std::clamp(ParseIntOrDefault(ReadLine("autotune> choose 1 or 2 [1]: "), 1), 1, 2);
  if (liEvaluationChoice == 2) {
    lAutoTuneWizardConfig.pAutoTuneEvaluationMode = AutoTuneEvaluationMode::QuestionAnswerFolder;
    const std::string lSQuestionFolderPath = Trim(ReadLine("autotune> path to Question_N files: "));
    if (lSQuestionFolderPath.empty()) {
      std::cout << "autotune> custom mode requires a folder path.\n";
      return;
    }
    lAutoTuneWizardConfig.pPathQuestionAnswerFolder = std::filesystem::path(lSQuestionFolderPath);
  } else {
    lAutoTuneWizardConfig.pAutoTuneEvaluationMode = AutoTuneEvaluationMode::BuiltInBenchmarks;
  }

  lAutoTuneWizardConfig.pdTargetTokensPerSecond =
      std::max(0.0, ParseDoubleOrDefault(ReadLine("autotune> target speed tok/s (0=ignore) [0]: "), 0.0));
  lAutoTuneWizardConfig.pdTargetLatencyMilliseconds =
      std::max(0.0, ParseDoubleOrDefault(ReadLine("autotune> target latency ms (0=ignore) [0]: "), 0.0));
  lAutoTuneWizardConfig.piRunsPerSetting =
      std::clamp(ParseIntOrDefault(ReadLine("autotune> runs per setting for average [3]: "), 3), 1, 10);

  const std::vector<ollama_engine::GenerationSettings> lVecGenerationSettingsCandidates = BuildAutoTuneCandidates();
  std::cout << "autotune> candidate setting count: " << lVecGenerationSettingsCandidates.size() << "\n";
  if (!ReadYesNo("autotune> start tuning now? [Y/n]: ", true)) {
    std::cout << "autotune> cancelled.\n";
    return;
  }

  double ldBestObjectiveScore = -1.0;
  AutoTuneEvaluationSummary lBestAutoTuneEvaluationSummary;
  ollama_engine::GenerationSettings lBestGenerationSettings;

  for (std::size_t liCandidateIndex = 0; liCandidateIndex < lVecGenerationSettingsCandidates.size(); ++liCandidateIndex) {
    const ollama_engine::GenerationSettings& lGenerationSettings = lVecGenerationSettingsCandidates[liCandidateIndex];
    std::string lSError;
    if (!pPtrEngine->SetGenerationSettings(lGenerationSettings, &lSError)) {
      std::cout << "autotune> candidate " << (liCandidateIndex + 1) << " skipped: " << lSError << "\n";
      continue;
    }

    const AutoTuneEvaluationSummary lAutoTuneEvaluationSummary =
        EvaluateAutoTuneSettings(pPtrEngine, lAutoTuneWizardConfig);
    if (!lAutoTuneEvaluationSummary.pbOk) {
      std::cout << "autotune> candidate " << (liCandidateIndex + 1)
                << " evaluation failed: " << lAutoTuneEvaluationSummary.pSError << "\n";
      continue;
    }

    const double ldObjectiveScore = ComputeAutoTuneObjectiveScore(lAutoTuneEvaluationSummary, lAutoTuneWizardConfig);
    std::cout << "autotune> candidate " << (liCandidateIndex + 1) << "/" << lVecGenerationSettingsCandidates.size()
              << " score=" << ldObjectiveScore << " pass=" << lAutoTuneEvaluationSummary.piPassCount << "/"
              << lAutoTuneEvaluationSummary.piCaseCount << " avg_ms=" << lAutoTuneEvaluationSummary.pdAverageTotalMilliseconds
              << " tok/s=" << lAutoTuneEvaluationSummary.pdAverageTokensPerSecond << " settings: "
              << DescribeGenerationSettings(lGenerationSettings) << "\n";

    if (ldObjectiveScore > ldBestObjectiveScore) {
      ldBestObjectiveScore = ldObjectiveScore;
      lBestAutoTuneEvaluationSummary = lAutoTuneEvaluationSummary;
      lBestGenerationSettings = lGenerationSettings;
    }
  }

  if (ldBestObjectiveScore < 0.0) {
    std::cout << "autotune> no successful candidate evaluations.\n";
    return;
  }

  std::string lSError;
  if (!pPtrEngine->SetGenerationSettings(lBestGenerationSettings, &lSError)) {
    std::cout << "autotune> failed to apply best settings: " << lSError << "\n";
    return;
  }

  std::cout << "autotune> best settings applied.\n";
  std::cout << "autotune> score=" << ldBestObjectiveScore << " pass=" << lBestAutoTuneEvaluationSummary.piPassCount << "/"
            << lBestAutoTuneEvaluationSummary.piCaseCount << " avg_ms="
            << lBestAutoTuneEvaluationSummary.pdAverageTotalMilliseconds << " avg_ttft_ms="
            << lBestAutoTuneEvaluationSummary.pdAverageTtftMilliseconds << " avg_tok/s="
            << lBestAutoTuneEvaluationSummary.pdAverageTokensPerSecond << "\n";
  std::cout << "autotune> " << DescribeGenerationSettings(lBestGenerationSettings) << "\n";

  const std::string lSDefaultTemplateName = SanitizeTemplateName(lAutoTuneWizardConfig.pSModelName + "_autotune");
  std::string lSTemplateName = Trim(ReadLine("autotune> profile/template base name [" + lSDefaultTemplateName + "]: "));
  if (lSTemplateName.empty()) {
    lSTemplateName = lSDefaultTemplateName;
  }
  lSTemplateName = SanitizeTemplateName(lSTemplateName);

  AutoTuneTemplate lAutoTuneTemplate;
  lAutoTuneTemplate.pSName = lSTemplateName;
  lAutoTuneTemplate.pSModelName = lAutoTuneWizardConfig.pSModelName;
  lAutoTuneTemplate.pSCreatedUtc = BuildUtcIsoTimestamp();
  lAutoTuneTemplate.pSHardwareFingerprint = BuildHardwareFingerprint();
  lAutoTuneTemplate.pSHardwareSummary = BuildHardwareSummary();
  lAutoTuneTemplate.pGenerationSettings = lBestGenerationSettings;
  lAutoTuneTemplate.piRunsPerSetting = lAutoTuneWizardConfig.piRunsPerSetting;
  lAutoTuneTemplate.pSEvaluationMode =
      (lAutoTuneWizardConfig.pAutoTuneEvaluationMode == AutoTuneEvaluationMode::BuiltInBenchmarks) ? "builtin"
                                                                                                     : "question_folder";
  lAutoTuneTemplate.pSEvaluationSource = lAutoTuneWizardConfig.pPathQuestionAnswerFolder.string();
  lAutoTuneTemplate.pdAverageScore = lBestAutoTuneEvaluationSummary.pdAverageScore;
  lAutoTuneTemplate.pdAverageTokensPerSecond = lBestAutoTuneEvaluationSummary.pdAverageTokensPerSecond;
  lAutoTuneTemplate.pdAverageTotalMilliseconds = lBestAutoTuneEvaluationSummary.pdAverageTotalMilliseconds;
  lAutoTuneTemplate.pdAverageTtftMilliseconds = lBestAutoTuneEvaluationSummary.pdAverageTtftMilliseconds;

  bool lbSavedSomething = false;
  if (ReadYesNo("autotune> save reusable template (.xml)? [Y/n]: ", true)) {
    std::string lSSaveTemplateError;
    const std::filesystem::path lPathTemplateDirectory = GetAutoTuneTemplateDirectory(pPathModelFolder);
    if (!SaveAutoTuneTemplateXmlFile(lAutoTuneTemplate, lPathTemplateDirectory, &lSSaveTemplateError)) {
      std::cout << "autotune> failed to save template: " << lSSaveTemplateError << "\n";
    } else {
      std::cout << "autotune> saved template: " << (lPathTemplateDirectory / (lAutoTuneTemplate.pSName + ".xml")) << "\n";
      lbSavedSomething = true;
    }
  }

  if (ReadYesNo("autotune> save finalized finetune profile (.ole) for hardware auto-apply? [Y/n]: ", true)) {
    std::string lSSaveFinalError;
    const std::filesystem::path lPathFinalProfileDirectory = GetAutoTuneFinalProfileDirectory(pPathModelFolder);
    if (!SaveAutoTuneFinalProfileOleFile(lAutoTuneTemplate, lPathFinalProfileDirectory, &lSSaveFinalError)) {
      std::cout << "autotune> failed to save finalized profile: " << lSSaveFinalError << "\n";
    } else {
      std::cout << "autotune> saved finalized profile: "
                << (lPathFinalProfileDirectory / (lAutoTuneTemplate.pSName + ".ole")) << "\n";
      lbSavedSomething = true;
    }
  }

  if (!lbSavedSomething) {
    std::cout << "autotune> no file saved.\n";
  }
}

/// <summary>Runs top-level autotune menu so wizard and templates are first-class choices.</summary>
/// <param name="pPtrEngine">Engine pointer.</param>
/// <param name="pVecSModels">Available model list.</param>
/// <param name="pPathModelFolder">Model folder path.</param>
/// <param name="pSLoadedModelNameInOut">Current loaded model name (mutable).</param>
void RunAutoTuneMenu(ollama_engine::EngineInterface* pPtrEngine, const std::vector<std::string>& pVecSModels,
                     const std::filesystem::path& pPathModelFolder, std::string* pSLoadedModelNameInOut) {
  if (pPtrEngine == nullptr || pSLoadedModelNameInOut == nullptr) {
    std::cout << "autotune> engine is not available.\n";
    return;
  }

  const std::filesystem::path lPathTemplateDirectory = GetAutoTuneTemplateDirectory(pPathModelFolder);
  const std::vector<AutoTuneTemplate> lVecAutoTuneTemplates = LoadAutoTuneTemplates(lPathTemplateDirectory);
  const std::string lSHardwareFingerprint = BuildHardwareFingerprint();

  std::cout << "autotune> choose mode:\n";
  std::cout << "  1) Wizard setup (find best settings)\n";
  std::cout << "  2) Use saved template (.xml)\n";
  std::cout << "  3) List templates\n";
  const int liMenuChoice = std::clamp(ParseIntOrDefault(ReadLine("autotune> choose 1, 2, or 3 [1]: "), 1), 1, 3);

  if (liMenuChoice == 1) {
    RunAutoTuneWizard(pPtrEngine, pVecSModels, pPathModelFolder, pSLoadedModelNameInOut);
    return;
  }

  if (liMenuChoice == 3) {
    PrintAutoTuneTemplates(lVecAutoTuneTemplates, *pSLoadedModelNameInOut, lSHardwareFingerprint);
    return;
  }

  if (lVecAutoTuneTemplates.empty()) {
    std::cout << "autotune> no templates found.\n";
    return;
  }

  PrintAutoTuneTemplates(lVecAutoTuneTemplates, *pSLoadedModelNameInOut, lSHardwareFingerprint);
  const std::string lSSelection = Trim(ReadLine("autotune> pick template (number or exact name): "));
  if (lSSelection.empty()) {
    std::cout << "autotune> no template selected.\n";
    return;
  }
  const std::optional<AutoTuneTemplate> lOptTemplate =
      ResolveSelectedAutoTuneTemplate(lSSelection, lVecAutoTuneTemplates);
  if (!lOptTemplate.has_value()) {
    std::cout << "autotune> template not found.\n";
    return;
  }

  if (lOptTemplate->pSHardwareFingerprint != lSHardwareFingerprint) {
    std::cout << "autotune> note: template hardware differs from current machine.\n";
    std::cout << "autotune> current=" << lSHardwareFingerprint
              << " template=" << lOptTemplate->pSHardwareFingerprint << "\n";
  }

  std::string lSError;
  if (!ApplyAutoTuneTemplate(pPtrEngine, *lOptTemplate, pSLoadedModelNameInOut, &lSError)) {
    std::cout << "autotune> failed to apply template: " << lSError << "\n";
    return;
  }

  std::cout << "autotune> applied template '" << lOptTemplate->pSName << "'\n";
  std::cout << "autotune> active model: " << *pSLoadedModelNameInOut << "\n";
  std::cout << "autotune> " << DescribeGenerationSettings(pPtrEngine->GetGenerationSettings()) << "\n";
}

/// <summary>Prints CLI slash commands.</summary>
void PrintHelp() {
  std::cout << "Commands:\n";
  std::cout << "  /quit                     Exit the CLI\n";
  std::cout << "  /help                     Show command help\n";
  std::cout << "  /run_benchmarks           Run built-in open-source benchmark probes\n";
  std::cout << "  /run_tests <directory>    Run Question_N + Answer/Anser test pairs from directory\n";
  std::cout << "  /autotune                 Run tuning wizard for speed/latency/quality targets\n";
  std::cout << "  /autotune_templates       List saved .xml autotune templates\n";
  std::cout << "  /autotune_use <name|idx>  Apply a saved .xml template now\n";
  std::cout << "  /autotune_profiles        List finalized .ole autotune profiles\n";
  std::cout << "  /autotune_use_profile <name|idx>  Apply a finalized .ole profile now\n";
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
  const std::filesystem::path lPathFinalProfileDirectory = GetAutoTuneFinalProfileDirectory(lPathModelFolder);
  const std::vector<AutoTuneTemplate> lVecAutoTuneFinalProfiles = LoadAutoTuneFinalProfiles(lPathFinalProfileDirectory);
  const std::string lSHardwareFingerprint = BuildHardwareFingerprint();
  const std::optional<AutoTuneTemplate> lOptAutoProfile =
      FindBestMatchingTemplate(lVecAutoTuneFinalProfiles, lSModelName, lSHardwareFingerprint);
  if (lOptAutoProfile.has_value()) {
    std::string lSError;
    if (ApplyAutoTuneTemplate(lPtrEngine.get(), *lOptAutoProfile, &lSModelName, &lSError)) {
      std::cout << "autotune> auto-applied finalized profile '" << lOptAutoProfile->pSName << "'\n";
      std::cout << "autotune> " << DescribeGenerationSettings(lPtrEngine->GetGenerationSettings()) << "\n";
    } else {
      std::cout << "autotune> found matching finalized profile '" << lOptAutoProfile->pSName
                << "' but failed to apply: " << lSError << "\n";
    }
  } else {
    std::cout << "autotune> no hardware/model-matching finalized profile found. Run /autotune to create one.\n";
  }

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
    if (lSPrompt == "/autotune") {
      RunAutoTuneMenu(lPtrEngine.get(), lVecSModels, lPathModelFolder, &lSModelName);
      continue;
    }
    if (lSPrompt == "/autotune_templates") {
      PrintAutoTuneTemplates(LoadAutoTuneTemplates(GetAutoTuneTemplateDirectory(lPathModelFolder)), lSModelName,
                             BuildHardwareFingerprint());
      continue;
    }
    if (lSPrompt == "/autotune_profiles") {
      PrintAutoTuneFinalProfiles(LoadAutoTuneFinalProfiles(GetAutoTuneFinalProfileDirectory(lPathModelFolder)),
                                 lSModelName, BuildHardwareFingerprint());
      continue;
    }
    if (lSPrompt.rfind("/autotune_use_profile", 0) == 0) {
      const std::string lSTail = Trim(lSPrompt.substr(std::string("/autotune_use_profile").size()));
      if (lSTail.empty()) {
        std::cout << "autotune> usage: /autotune_use_profile <name|idx>\n";
        continue;
      }
      const std::vector<AutoTuneTemplate> lVecAutoTuneProfiles =
          LoadAutoTuneFinalProfiles(GetAutoTuneFinalProfileDirectory(lPathModelFolder));
      if (lVecAutoTuneProfiles.empty()) {
        std::cout << "autotune> no finalized profiles found.\n";
        continue;
      }
      const std::optional<AutoTuneTemplate> lOptProfile =
          ResolveSelectedAutoTuneTemplate(lSTail, lVecAutoTuneProfiles);
      if (!lOptProfile.has_value()) {
        std::cout << "autotune> finalized profile not found. Use /autotune_profiles to list options.\n";
        continue;
      }
      const std::string lSHardwareNow = BuildHardwareFingerprint();
      if (lOptProfile->pSHardwareFingerprint != lSHardwareNow) {
        std::cout << "autotune> note: profile hardware differs from current machine.\n";
        std::cout << "autotune> current=" << lSHardwareNow
                  << " profile=" << lOptProfile->pSHardwareFingerprint << "\n";
      }
      std::string lSError;
      if (!ApplyAutoTuneTemplate(lPtrEngine.get(), *lOptProfile, &lSModelName, &lSError)) {
        std::cout << "autotune> failed to apply finalized profile: " << lSError << "\n";
        continue;
      }
      std::cout << "autotune> applied finalized profile '" << lOptProfile->pSName << "'\n";
      std::cout << "autotune> active model: " << lSModelName << "\n";
      std::cout << "autotune> " << DescribeGenerationSettings(lPtrEngine->GetGenerationSettings()) << "\n";
      continue;
    }
    if (lSPrompt.rfind("/autotune_use", 0) == 0) {
      const std::string lSTail = Trim(lSPrompt.substr(std::string("/autotune_use").size()));
      if (lSTail.empty()) {
        std::cout << "autotune> usage: /autotune_use <name|idx>\n";
        continue;
      }
      const std::vector<AutoTuneTemplate> lVecAutoTuneTemplates =
          LoadAutoTuneTemplates(GetAutoTuneTemplateDirectory(lPathModelFolder));
      if (lVecAutoTuneTemplates.empty()) {
        std::cout << "autotune> no templates found.\n";
        continue;
      }
      const std::optional<AutoTuneTemplate> lOptTemplate =
          ResolveSelectedAutoTuneTemplate(lSTail, lVecAutoTuneTemplates);
      if (!lOptTemplate.has_value()) {
        std::cout << "autotune> template not found. Use /autotune_templates to list options.\n";
        continue;
      }
      const std::string lSHardwareNow = BuildHardwareFingerprint();
      if (lOptTemplate->pSHardwareFingerprint != lSHardwareNow) {
        std::cout << "autotune> note: template hardware differs from current machine.\n";
        std::cout << "autotune> current=" << lSHardwareNow
                  << " template=" << lOptTemplate->pSHardwareFingerprint << "\n";
      }
      std::string lSError;
      if (!ApplyAutoTuneTemplate(lPtrEngine.get(), *lOptTemplate, &lSModelName, &lSError)) {
        std::cout << "autotune> failed to apply template: " << lSError << "\n";
        continue;
      }
      std::cout << "autotune> applied template '" << lOptTemplate->pSName << "'\n";
      std::cout << "autotune> active model: " << lSModelName << "\n";
      std::cout << "autotune> " << DescribeGenerationSettings(lPtrEngine->GetGenerationSettings()) << "\n";
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
