#include "ollama_engine/engine_api.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#endif

namespace {
namespace fs = std::filesystem;

enum class EvalCategory {
  Retrieval,
  Explanation,
  Optimization,
  Patch,
  RagSanity,
};

struct RepoSpec {
  std::string pSName;
  std::string pSRemoteUrl;
  std::vector<std::string> pVecSCorpusPaths;
};

struct RepoMaterialized {
  RepoSpec pRepoSpec;
  fs::path pPathRepository;
  fs::path pPathCorpus;
  std::string pSCommitHash;
  std::string pSCommitHashShort;
  bool pbFetchOk = false;
  bool pbCorpusPrepared = false;
  std::string pSNote;
};

struct CommandResult {
  int piExitCode = -1;
  std::string pSOutput;
};

struct TextRule {
  std::vector<std::string> pVecSMustContainAll;
  std::vector<std::string> pVecSMustContainAny;
  std::vector<std::string> pVecSMustNotContainAny;
  std::size_t piMinAnyMatches = 1;
};

struct EvalTestDefinition {
  int piId = 0;
  std::string pSTitle;
  EvalCategory pCategory = EvalCategory::Retrieval;

  std::string pSSourceRepo;
  std::string pSSourceFile;
  std::string pSSourceSymbol;

  std::string pSPrompt;

  std::vector<std::string> pVecSExpectedRepoTokens;
  std::vector<std::string> pVecSExpectedFileFragments;
  std::vector<std::string> pVecSExpectedRetrievalKeywords;
  std::size_t piMinRetrievalKeywordMatches = 1;

  bool pbRequiresModel = false;
  bool pbRequirePatchLikeOutput = false;
  TextRule pResponseRule;

  std::string pSRagSanityOperation;
};

struct EvalTestResult {
  int piId = 0;
  std::string pSTitle;
  EvalCategory pCategory = EvalCategory::Retrieval;
  bool pbPass = false;

  std::string pSPrompt;
  std::string pSResponse;
  std::vector<std::string> pVecSSnippets;
  std::string pSNotes;
};

struct ScanReport {
  bool pbStarted = false;
  bool pbRunningSeen = false;
  bool pbFinishedSeen = false;
  bool pbTimedOut = false;
  bool pbError = false;

  std::size_t piVectorDatabaseSize = 0;
  std::size_t piFilesProcessed = 0;
  std::size_t piTotalFiles = 0;

  std::string pSError;
};

struct RetrievalCheckOutcome {
  bool pbPass = false;
  bool pbNonEmpty = false;
  bool pbRepoMatched = false;
  bool pbFileMatched = false;
  std::size_t piKeywordMatches = 0;
  std::string pSNotes;
};

struct RunnerPaths {
  fs::path pPathModelFolder;
  fs::path pPathEvalRoot;
  fs::path pPathReferenceRepos;
  fs::path pPathIndexCorpus;
  fs::path pPathResults;
};

struct RunContext {
  RunnerPaths pRunnerPaths;
  std::unordered_map<std::string, fs::path> pMapSRepoNameToPath;
  std::unordered_map<std::string, fs::path> pMapSRepoNameToCorpusPath;
  std::vector<std::string> pVecSDatabaseNames;

  bool pbModelLoaded = false;
  std::string pSLoadedModelName;
  bool pbInitialDatabaseLoadOk = false;
  std::string pSInitialDatabaseLoadError;

  bool pbSanityPrimaryRunDone = false;
  ScanReport pSanityPrimaryScan;
  std::string pSSanityDatabaseName;
};

std::string ToLowerAscii(std::string pSValue) {
  std::transform(pSValue.begin(), pSValue.end(), pSValue.begin(),
                 [](const unsigned char pCChar) { return static_cast<char>(std::tolower(pCChar)); });
  return pSValue;
}

std::string TrimAscii(const std::string& pSValue) {
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

bool ContainsCaseInsensitive(const std::string& pSHaystack, const std::string& pSNeedle) {
  if (pSNeedle.empty()) {
    return true;
  }
  const std::string lSHaystackLower = ToLowerAscii(pSHaystack);
  const std::string lSNeedleLower = ToLowerAscii(pSNeedle);
  return lSHaystackLower.find(lSNeedleLower) != std::string::npos;
}

std::string EscapeJson(const std::string& pSValue) {
  std::string lSOut;
  lSOut.reserve(pSValue.size() + 16);
  for (const unsigned char lCChar : pSValue) {
    switch (lCChar) {
      case '"':
        lSOut += "\\\"";
        break;
      case '\\':
        lSOut += "\\\\";
        break;
      case '\b':
        lSOut += "\\b";
        break;
      case '\f':
        lSOut += "\\f";
        break;
      case '\n':
        lSOut += "\\n";
        break;
      case '\r':
        lSOut += "\\r";
        break;
      case '\t':
        lSOut += "\\t";
        break;
      default:
        if (lCChar < 0x20) {
          static constexpr std::array<char, 16> kArrCHex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                             '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
          lSOut += "\\u00";
          lSOut.push_back(kArrCHex[(lCChar >> 4U) & 0x0F]);
          lSOut.push_back(kArrCHex[lCChar & 0x0F]);
        } else {
          lSOut.push_back(static_cast<char>(lCChar));
        }
        break;
    }
  }
  return lSOut;
}

std::string CategoryToString(const EvalCategory pCategory) {
  switch (pCategory) {
    case EvalCategory::Retrieval:
      return "retrieval";
    case EvalCategory::Explanation:
      return "explanation";
    case EvalCategory::Optimization:
      return "optimization";
    case EvalCategory::Patch:
      return "patch";
    case EvalCategory::RagSanity:
      return "rag_sanity";
    default:
      return "unknown";
  }
}

std::string ShellQuote(const std::string& pSValue) {
  std::string lSQuoted = "'";
  for (const char lCChar : pSValue) {
    if (lCChar == '\'') {
      lSQuoted += "'\"'\"'";
    } else {
      lSQuoted.push_back(lCChar);
    }
  }
  lSQuoted.push_back('\'');
  return lSQuoted;
}

CommandResult RunShellCommand(const std::string& pSCommand) {
  CommandResult lCommandResult;
  std::array<char, 4096> lArrCBuffer{};
  const std::string lSCommandWithErr = pSCommand + " 2>&1";

  FILE* lPtrPipe = popen(lSCommandWithErr.c_str(), "r");
  if (lPtrPipe == nullptr) {
    lCommandResult.piExitCode = -1;
    lCommandResult.pSOutput = "Failed to open process pipe.";
    return lCommandResult;
  }

  while (std::fgets(lArrCBuffer.data(), static_cast<int>(lArrCBuffer.size()), lPtrPipe) != nullptr) {
    lCommandResult.pSOutput += lArrCBuffer.data();
  }

  const int liStatus = pclose(lPtrPipe);
  lCommandResult.piExitCode = liStatus;
  return lCommandResult;
}

std::string SanitizeDatabaseName(const std::string& pSValue) {
  std::string lSOut;
  lSOut.reserve(pSValue.size() + 8);
  for (const char lCChar : pSValue) {
    const unsigned char lCUChar = static_cast<unsigned char>(lCChar);
    if (std::isalnum(lCUChar) != 0 || lCChar == '_' || lCChar == '-' || lCChar == '.') {
      lSOut.push_back(lCChar);
    } else {
      lSOut.push_back('_');
    }
  }
  if (lSOut.empty()) {
    lSOut = "eval_db";
  }
  return lSOut;
}

bool ReadTextFile(const fs::path& pPathFile, std::string* pSTextOut) {
  if (pSTextOut == nullptr) {
    return false;
  }
  std::ifstream lFileIn(pPathFile, std::ios::in | std::ios::binary);
  if (!lFileIn.good()) {
    return false;
  }
  std::ostringstream lBuffer;
  lBuffer << lFileIn.rdbuf();
  *pSTextOut = lBuffer.str();
  return true;
}

bool WriteTextFile(const fs::path& pPathFile, const std::string& pSText) {
  std::error_code lErrorCode;
  fs::create_directories(pPathFile.parent_path(), lErrorCode);
  if (lErrorCode) {
    return false;
  }
  std::ofstream lFileOut(pPathFile, std::ios::out | std::ios::trunc | std::ios::binary);
  if (!lFileOut.good()) {
    return false;
  }
  lFileOut << pSText;
  return lFileOut.good();
}

std::vector<std::string> SplitLines(const std::string& pSText) {
  std::vector<std::string> lVecSLines;
  std::stringstream lStream(pSText);
  std::string lSLine;
  while (std::getline(lStream, lSLine)) {
    lVecSLines.push_back(lSLine);
  }
  if (!pSText.empty() && pSText.back() == '\n') {
    lVecSLines.push_back("");
  }
  return lVecSLines;
}

std::string JoinLines(const std::vector<std::string>& pVecSLines,
                      const std::size_t piBegin,
                      const std::size_t piEnd,
                      const std::size_t piCharCap) {
  std::string lSOut;
  for (std::size_t liIndex = piBegin; liIndex < piEnd && liIndex < pVecSLines.size(); ++liIndex) {
    if (!lSOut.empty()) {
      lSOut.push_back('\n');
    }
    lSOut += pVecSLines[liIndex];
    if (lSOut.size() >= piCharCap) {
      break;
    }
  }
  if (lSOut.size() > piCharCap) {
    lSOut.resize(piCharCap);
  }
  return lSOut;
}

std::string BuildSourceExcerpt(const fs::path& pPathFile, const std::string& pSSymbol) {
  std::string lSContent;
  if (!ReadTextFile(pPathFile, &lSContent)) {
    return "";
  }

  const std::vector<std::string> lVecSLines = SplitLines(lSContent);
  if (lVecSLines.empty()) {
    return "";
  }

  std::size_t liStart = 0;
  std::size_t liEnd = std::min<std::size_t>(lVecSLines.size(), 80);

  if (!pSSymbol.empty()) {
    const std::string lSSymbolLower = ToLowerAscii(pSSymbol);
    for (std::size_t liIndex = 0; liIndex < lVecSLines.size(); ++liIndex) {
      if (ToLowerAscii(lVecSLines[liIndex]).find(lSSymbolLower) != std::string::npos) {
        liStart = (liIndex > 10) ? (liIndex - 10) : 0;
        liEnd = std::min<std::size_t>(lVecSLines.size(), liIndex + 36);
        break;
      }
    }
  }

  return JoinLines(lVecSLines, liStart, liEnd, 2400);
}

std::string JoinSnippets(const std::vector<std::string>& pVecSSnippets, const std::size_t piMaxSnippets) {
  std::ostringstream lStream;
  const std::size_t liCount = std::min(piMaxSnippets, pVecSSnippets.size());
  for (std::size_t liIndex = 0; liIndex < liCount; ++liIndex) {
    lStream << "[snippet " << (liIndex + 1) << "]\n";
    std::string lSSnippet = pVecSSnippets[liIndex];
    if (lSSnippet.size() > 1400) {
      lSSnippet = lSSnippet.substr(0, 1400);
      lSSnippet += "\n// ... truncated ...";
    }
    lStream << lSSnippet << "\n\n";
  }
  return lStream.str();
}

bool IsCppLikeSourceFile(const fs::path& pPathFile) {
  const std::string lSExt = ToLowerAscii(pPathFile.extension().string());
  static const std::set<std::string> kSetSExtensions = {
      ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ipp", ".tcc", ".tpp"};
  return kSetSExtensions.find(lSExt) != kSetSExtensions.end();
}

bool CopyFileAsCorpusEntry(const fs::path& pPathSourceFile,
                           const fs::path& pPathDestinationRoot,
                           const std::string& pSRelativePath,
                           std::string* pSErrorOut) {
  if (IsCppLikeSourceFile(pPathSourceFile)) {
    std::string lSContent;
    if (!ReadTextFile(pPathSourceFile, &lSContent)) {
      if (pSErrorOut != nullptr) {
        *pSErrorOut = "Failed to read source file: " + pPathSourceFile.string();
      }
      return false;
    }

    const fs::path lPathDestination = pPathDestinationRoot / (pSRelativePath + ".md");
    std::ostringstream lStream;
    lStream << "# Source File: " << pSRelativePath << "\n\n";
    lStream << "```cpp\n";
    lStream << lSContent;
    if (lSContent.empty() || lSContent.back() != '\n') {
      lStream << "\n";
    }
    lStream << "```\n";

    if (!WriteTextFile(lPathDestination, lStream.str())) {
      if (pSErrorOut != nullptr) {
        *pSErrorOut = "Failed to write corpus file: " + lPathDestination.string();
      }
      return false;
    }
    return true;
  }

  std::error_code lErrorCode;
  const fs::path lPathDestination = pPathDestinationRoot / pSRelativePath;
  fs::create_directories(lPathDestination.parent_path(), lErrorCode);
  if (lErrorCode) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Failed to create destination directory: " + lPathDestination.parent_path().string();
    }
    return false;
  }
  fs::copy_file(pPathSourceFile, lPathDestination, fs::copy_options::overwrite_existing, lErrorCode);
  if (lErrorCode) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Failed to copy source file: " + pPathSourceFile.string();
    }
    return false;
  }
  return true;
}

bool CopyPathTree(const fs::path& pPathSource,
                  const fs::path& pPathDestination,
                  std::string* pSErrorOut) {
  std::error_code lErrorCode;
  if (!fs::exists(pPathSource, lErrorCode)) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Missing source path: " + pPathSource.string();
    }
    return false;
  }

  if (fs::is_regular_file(pPathSource, lErrorCode)) {
    fs::create_directories(pPathDestination.parent_path(), lErrorCode);
    if (lErrorCode) {
      if (pSErrorOut != nullptr) {
        *pSErrorOut = "Failed to create destination directory: " + pPathDestination.parent_path().string();
      }
      return false;
    }
    fs::copy_file(pPathSource, pPathDestination, fs::copy_options::overwrite_existing, lErrorCode);
    if (lErrorCode) {
      if (pSErrorOut != nullptr) {
        *pSErrorOut = "Failed to copy file: " + pPathSource.string();
      }
      return false;
    }
    return true;
  }

  if (!fs::is_directory(pPathSource, lErrorCode)) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Path is not a file or directory: " + pPathSource.string();
    }
    return false;
  }

  fs::create_directories(pPathDestination, lErrorCode);
  if (lErrorCode) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Failed to create destination directory: " + pPathDestination.string();
    }
    return false;
  }

  fs::recursive_directory_iterator lIterator(pPathSource, fs::directory_options::skip_permission_denied, lErrorCode);
  const fs::recursive_directory_iterator lEndIterator;
  while (!lErrorCode && lIterator != lEndIterator) {
    const fs::directory_entry lDirectoryEntry = *lIterator;
    lIterator.increment(lErrorCode);
    if (lErrorCode) {
      lErrorCode.clear();
      continue;
    }

    const fs::path lPathRelative = fs::relative(lDirectoryEntry.path(), pPathSource, lErrorCode);
    if (lErrorCode) {
      lErrorCode.clear();
      continue;
    }

    const fs::path lPathTarget = pPathDestination / lPathRelative;
    if (lDirectoryEntry.is_directory(lErrorCode)) {
      lErrorCode.clear();
      fs::create_directories(lPathTarget, lErrorCode);
      lErrorCode.clear();
      continue;
    }

    if (!lDirectoryEntry.is_regular_file(lErrorCode) || lErrorCode) {
      lErrorCode.clear();
      continue;
    }

    fs::create_directories(lPathTarget.parent_path(), lErrorCode);
    if (lErrorCode) {
      if (pSErrorOut != nullptr) {
        *pSErrorOut = "Failed to create destination directory: " + lPathTarget.parent_path().string();
      }
      return false;
    }
    fs::copy_file(lDirectoryEntry.path(), lPathTarget, fs::copy_options::overwrite_existing, lErrorCode);
    if (lErrorCode) {
      if (pSErrorOut != nullptr) {
        *pSErrorOut = "Failed to copy file: " + lDirectoryEntry.path().string();
      }
      return false;
    }
  }

  return true;
}

bool EnsureRepository(const RepoSpec& pRepoSpec,
                      const fs::path& pPathReferenceRepos,
                      RepoMaterialized* pPtrRepoOut,
                      std::string* pSErrorOut) {
  if (pPtrRepoOut == nullptr) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Repository output pointer was null.";
    }
    return false;
  }

  std::error_code lErrorCode;
  fs::create_directories(pPathReferenceRepos, lErrorCode);

  RepoMaterialized lRepoMaterialized;
  lRepoMaterialized.pRepoSpec = pRepoSpec;
  lRepoMaterialized.pPathRepository = pPathReferenceRepos / pRepoSpec.pSName;

  const fs::path lPathGitDirectory = lRepoMaterialized.pPathRepository / ".git";
  if (!fs::exists(lPathGitDirectory, lErrorCode)) {
    const std::string lSCloneCommand = "git clone --depth 1 " + ShellQuote(pRepoSpec.pSRemoteUrl) + " " +
                                       ShellQuote(lRepoMaterialized.pPathRepository.string());
    const CommandResult lCloneResult = RunShellCommand(lSCloneCommand);
    if (lCloneResult.piExitCode != 0) {
      if (pSErrorOut != nullptr) {
        *pSErrorOut = "Failed to clone " + pRepoSpec.pSName + ": " + TrimAscii(lCloneResult.pSOutput);
      }
      return false;
    }
    lRepoMaterialized.pSNote = "cloned";
  } else {
    const CommandResult lStatusResult =
        RunShellCommand("git -C " + ShellQuote(lRepoMaterialized.pPathRepository.string()) + " status --porcelain");
    const bool lbClean = (lStatusResult.piExitCode == 0) && TrimAscii(lStatusResult.pSOutput).empty();
    if (lbClean) {
      RunShellCommand("git -C " + ShellQuote(lRepoMaterialized.pPathRepository.string()) +
                      " fetch --depth 1 --prune origin");
      RunShellCommand("git -C " + ShellQuote(lRepoMaterialized.pPathRepository.string()) +
                      " pull --ff-only --depth 1");
      lRepoMaterialized.pSNote = "updated_or_reused_clean";
    } else {
      lRepoMaterialized.pSNote = "reused_dirty_worktree";
    }
  }

  const CommandResult lCommitResult =
      RunShellCommand("git -C " + ShellQuote(lRepoMaterialized.pPathRepository.string()) + " rev-parse HEAD");
  if (lCommitResult.piExitCode != 0) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Failed to read commit for " + pRepoSpec.pSName + ": " + TrimAscii(lCommitResult.pSOutput);
    }
    return false;
  }

  lRepoMaterialized.pSCommitHash = TrimAscii(lCommitResult.pSOutput);
  lRepoMaterialized.pSCommitHashShort =
      (lRepoMaterialized.pSCommitHash.size() > 12) ? lRepoMaterialized.pSCommitHash.substr(0, 12)
                                                    : lRepoMaterialized.pSCommitHash;
  lRepoMaterialized.pbFetchOk = true;
  *pPtrRepoOut = std::move(lRepoMaterialized);
  return true;
}

bool PrepareRepoCorpus(RepoMaterialized* pPtrRepoMaterialized,
                       const fs::path& pPathIndexCorpus,
                       std::string* pSErrorOut) {
  if (pPtrRepoMaterialized == nullptr) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Corpus repo pointer was null.";
    }
    return false;
  }

  pPtrRepoMaterialized->pPathCorpus = pPathIndexCorpus / pPtrRepoMaterialized->pRepoSpec.pSName;
  std::error_code lErrorCode;
  fs::remove_all(pPtrRepoMaterialized->pPathCorpus, lErrorCode);
  lErrorCode.clear();
  fs::create_directories(pPtrRepoMaterialized->pPathCorpus, lErrorCode);

  std::size_t liCopiedPathCount = 0;
  for (const std::string& lSCorpusPath : pPtrRepoMaterialized->pRepoSpec.pVecSCorpusPaths) {
    const fs::path lPathSource = pPtrRepoMaterialized->pPathRepository / lSCorpusPath;

    if (!fs::exists(lPathSource, lErrorCode)) {
      lErrorCode.clear();
      continue;
    }

    if (fs::is_regular_file(lPathSource, lErrorCode)) {
      std::string lSCopyError;
      if (!CopyFileAsCorpusEntry(lPathSource, pPtrRepoMaterialized->pPathCorpus, lSCorpusPath, &lSCopyError)) {
        if (pSErrorOut != nullptr) {
          *pSErrorOut = "Failed to prepare corpus for " + pPtrRepoMaterialized->pRepoSpec.pSName + ": " + lSCopyError;
        }
        return false;
      }
      ++liCopiedPathCount;
      continue;
    }

    if (!fs::is_directory(lPathSource, lErrorCode)) {
      lErrorCode.clear();
      continue;
    }

    fs::recursive_directory_iterator lIterator(lPathSource, fs::directory_options::skip_permission_denied, lErrorCode);
    const fs::recursive_directory_iterator lEndIterator;
    while (!lErrorCode && lIterator != lEndIterator) {
      const fs::directory_entry lDirectoryEntry = *lIterator;
      lIterator.increment(lErrorCode);
      if (lErrorCode) {
        lErrorCode.clear();
        continue;
      }
      if (!lDirectoryEntry.is_regular_file(lErrorCode) || lErrorCode) {
        lErrorCode.clear();
        continue;
      }

      const fs::path lPathRelative = fs::relative(lDirectoryEntry.path(), pPtrRepoMaterialized->pPathRepository, lErrorCode);
      if (lErrorCode) {
        lErrorCode.clear();
        continue;
      }

      std::string lSCopyError;
      if (!CopyFileAsCorpusEntry(lDirectoryEntry.path(), pPtrRepoMaterialized->pPathCorpus, lPathRelative.generic_string(),
                                 &lSCopyError)) {
        if (pSErrorOut != nullptr) {
          *pSErrorOut = "Failed to prepare corpus for " + pPtrRepoMaterialized->pRepoSpec.pSName + ": " + lSCopyError;
        }
        return false;
      }
      ++liCopiedPathCount;
    }
  }

  if (liCopiedPathCount == 0) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "No corpus paths were copied for " + pPtrRepoMaterialized->pRepoSpec.pSName;
    }
    return false;
  }

  pPtrRepoMaterialized->pbCorpusPrepared = true;
  return true;
}

ScanReport RunScanAndWait(ollama_engine::EngineInterface* pPtrEngine,
                          const fs::path& pPathScanSource,
                          const std::string& pSDatabaseName,
                          const std::chrono::seconds pTimeout) {
  ScanReport lScanReport;
  if (pPtrEngine == nullptr) {
    lScanReport.pbError = true;
    lScanReport.pSError = "Engine pointer is null.";
    return lScanReport;
  }

  std::string lSError;
  if (!pPtrEngine->SetRagOutputDatabase(pSDatabaseName, &lSError)) {
    lScanReport.pbError = true;
    lScanReport.pSError = "SetRagOutputDatabase failed: " + lSError;
    return lScanReport;
  }

  if (!pPtrEngine->Scan(pPathScanSource.string(), &lSError)) {
    lScanReport.pbError = true;
    lScanReport.pSError = "Scan start failed: " + lSError;
    return lScanReport;
  }

  lScanReport.pbStarted = true;

  const auto lStart = std::chrono::steady_clock::now();
  while (true) {
    const ollama_engine::VectorisationStateResponse lState = pPtrEngine->Fetch_state();
    lScanReport.piVectorDatabaseSize = lState.piVectorDatabaseSize;
    lScanReport.piFilesProcessed = lState.piFilesProcessed;
    lScanReport.piTotalFiles = lState.piTotalFiles;

    if (lState.pVectorisationLifecycleState == ollama_engine::VectorisationLifecycleState::Running) {
      lScanReport.pbRunningSeen = true;
    } else if (lState.pVectorisationLifecycleState == ollama_engine::VectorisationLifecycleState::Finished) {
      lScanReport.pbFinishedSeen = true;
      break;
    }

    const auto lNow = std::chrono::steady_clock::now();
    if ((lNow - lStart) >= pTimeout) {
      lScanReport.pbTimedOut = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  if (!lScanReport.pbFinishedSeen && !lScanReport.pbTimedOut) {
    lScanReport.pbError = true;
    lScanReport.pSError = "Scan ended without finished state.";
  }

  if (lScanReport.pbFinishedSeen && lScanReport.piVectorDatabaseSize == 0U) {
    lScanReport.pbError = true;
    lScanReport.pSError = "Scan finished with zero indexed chunks.";
  }

  return lScanReport;
}

RetrievalCheckOutcome EvaluateRetrievalSignals(const EvalTestDefinition& pEvalTestDefinition,
                                               const std::vector<std::string>& pVecSSnippets) {
  RetrievalCheckOutcome lRetrievalCheckOutcome;
  lRetrievalCheckOutcome.pbNonEmpty = !pVecSSnippets.empty();

  std::ostringstream lSnippetStream;
  for (const std::string& lSSnippet : pVecSSnippets) {
    lSnippetStream << lSSnippet << "\n";
  }
  const std::string lSCombinedSnippets = lSnippetStream.str();

  if (pEvalTestDefinition.pVecSExpectedRepoTokens.empty()) {
    lRetrievalCheckOutcome.pbRepoMatched = true;
  } else {
    for (const std::string& lSExpectedRepoToken : pEvalTestDefinition.pVecSExpectedRepoTokens) {
      if (ContainsCaseInsensitive(lSCombinedSnippets, lSExpectedRepoToken)) {
        lRetrievalCheckOutcome.pbRepoMatched = true;
        break;
      }
    }
  }

  if (pEvalTestDefinition.pVecSExpectedFileFragments.empty()) {
    lRetrievalCheckOutcome.pbFileMatched = true;
  } else {
    for (const std::string& lSFileFragment : pEvalTestDefinition.pVecSExpectedFileFragments) {
      if (ContainsCaseInsensitive(lSCombinedSnippets, lSFileFragment)) {
        lRetrievalCheckOutcome.pbFileMatched = true;
        break;
      }
    }
  }

  for (const std::string& lSKeyword : pEvalTestDefinition.pVecSExpectedRetrievalKeywords) {
    if (ContainsCaseInsensitive(lSCombinedSnippets, lSKeyword)) {
      ++lRetrievalCheckOutcome.piKeywordMatches;
    }
  }

  const std::size_t liMinKeywordMatches =
      pEvalTestDefinition.pVecSExpectedRetrievalKeywords.empty()
          ? 0
          : std::min<std::size_t>(pEvalTestDefinition.piMinRetrievalKeywordMatches,
                                  pEvalTestDefinition.pVecSExpectedRetrievalKeywords.size());

  lRetrievalCheckOutcome.pbPass = lRetrievalCheckOutcome.pbNonEmpty && lRetrievalCheckOutcome.pbRepoMatched &&
                                  lRetrievalCheckOutcome.pbFileMatched &&
                                  (lRetrievalCheckOutcome.piKeywordMatches >= liMinKeywordMatches);

  std::ostringstream lNotes;
  if (!lRetrievalCheckOutcome.pbNonEmpty) {
    lNotes << "No snippets returned. ";
  }
  if (!lRetrievalCheckOutcome.pbRepoMatched) {
    lNotes << "Expected repo token missing. ";
  }
  if (!lRetrievalCheckOutcome.pbFileMatched) {
    lNotes << "Expected file fragment missing. ";
  }
  if (lRetrievalCheckOutcome.piKeywordMatches < liMinKeywordMatches) {
    lNotes << "Keyword hits " << lRetrievalCheckOutcome.piKeywordMatches << " < " << liMinKeywordMatches << ". ";
  }
  lRetrievalCheckOutcome.pSNotes = TrimAscii(lNotes.str());

  return lRetrievalCheckOutcome;
}

bool LooksLikePatchOutput(const std::string& pSResponseText) {
  const std::string lSResponseLower = ToLowerAscii(pSResponseText);
  if (lSResponseLower.find("```diff") != std::string::npos || lSResponseLower.find("diff --git") != std::string::npos) {
    return true;
  }
  if (lSResponseLower.find("@@") != std::string::npos && lSResponseLower.find("+++") != std::string::npos &&
      lSResponseLower.find("---") != std::string::npos) {
    return true;
  }
  return false;
}

bool EvaluateTextRule(const std::string& pSResponseText, const TextRule& pTextRule, std::string* pSNotesOut) {
  const std::string lSResponseLower = ToLowerAscii(pSResponseText);
  bool lbPass = true;
  std::ostringstream lNotes;

  for (const std::string& lSMustContain : pTextRule.pVecSMustContainAll) {
    if (ToLowerAscii(lSResponseLower).find(ToLowerAscii(lSMustContain)) == std::string::npos) {
      lbPass = false;
      lNotes << "Missing required term: '" << lSMustContain << "'. ";
    }
  }

  std::size_t liAnyMatches = 0;
  for (const std::string& lSAnyTerm : pTextRule.pVecSMustContainAny) {
    if (ToLowerAscii(lSResponseLower).find(ToLowerAscii(lSAnyTerm)) != std::string::npos) {
      ++liAnyMatches;
    }
  }

  if (!pTextRule.pVecSMustContainAny.empty() && liAnyMatches < pTextRule.piMinAnyMatches) {
    lbPass = false;
    lNotes << "Any-term matches " << liAnyMatches << " < " << pTextRule.piMinAnyMatches << ". ";
  }

  for (const std::string& lSBannedTerm : pTextRule.pVecSMustNotContainAny) {
    if (ToLowerAscii(lSResponseLower).find(ToLowerAscii(lSBannedTerm)) != std::string::npos) {
      lbPass = false;
      lNotes << "Found banned term: '" << lSBannedTerm << "'. ";
    }
  }

  if (pSNotesOut != nullptr) {
    *pSNotesOut = TrimAscii(lNotes.str());
  }

  return lbPass;
}

std::string BuildModelPrompt(const EvalTestDefinition& pEvalTestDefinition,
                             const std::string& pSSourceExcerpt,
                             const std::vector<std::string>& pVecSSnippets) {
  std::ostringstream lPrompt;
  lPrompt << "You are evaluating C++ code understanding and transformation ability.\n";
  lPrompt << "Repository: " << pEvalTestDefinition.pSSourceRepo << "\n";
  lPrompt << "File: " << pEvalTestDefinition.pSSourceFile << "\n";
  if (!pEvalTestDefinition.pSSourceSymbol.empty()) {
    lPrompt << "Target symbol: " << pEvalTestDefinition.pSSourceSymbol << "\n";
  }
  lPrompt << "Task: " << pEvalTestDefinition.pSPrompt << "\n\n";

  lPrompt << "Retrieved context:\n";
  lPrompt << JoinSnippets(pVecSSnippets, 2) << "\n";

  lPrompt << "Source excerpt:\n```cpp\n";
  lPrompt << pSSourceExcerpt << "\n```\n\n";

  if (pEvalTestDefinition.pCategory == EvalCategory::Patch) {
    lPrompt << "Return only a unified diff patch. Keep identifiers stable unless requested by the task.\n";
  } else if (pEvalTestDefinition.pCategory == EvalCategory::Optimization) {
    lPrompt << "Return concise optimization recommendations with specific rationale.\n";
  } else {
    lPrompt << "Return concise technical explanation points.\n";
  }

  return lPrompt.str();
}

EvalTestDefinition MakeRetrievalTest(int pId,
                                     const std::string& pSTitle,
                                     const std::string& pSSourceRepo,
                                     const std::string& pSSourceFile,
                                     const std::string& pSSourceSymbol,
                                     const std::string& pSPrompt,
                                     const std::vector<std::string>& pVecSExpectedRepoTokens,
                                     const std::vector<std::string>& pVecSExpectedFileFragments,
                                     const std::vector<std::string>& pVecSExpectedKeywords,
                                     const std::size_t piMinKeywordMatches) {
  EvalTestDefinition lEvalTestDefinition;
  lEvalTestDefinition.piId = pId;
  lEvalTestDefinition.pSTitle = pSTitle;
  lEvalTestDefinition.pCategory = EvalCategory::Retrieval;
  lEvalTestDefinition.pSSourceRepo = pSSourceRepo;
  lEvalTestDefinition.pSSourceFile = pSSourceFile;
  lEvalTestDefinition.pSSourceSymbol = pSSourceSymbol;
  lEvalTestDefinition.pSPrompt = pSPrompt;
  lEvalTestDefinition.pVecSExpectedRepoTokens = pVecSExpectedRepoTokens;
  lEvalTestDefinition.pVecSExpectedFileFragments = pVecSExpectedFileFragments;
  lEvalTestDefinition.pVecSExpectedRetrievalKeywords = pVecSExpectedKeywords;
  lEvalTestDefinition.piMinRetrievalKeywordMatches = piMinKeywordMatches;
  return lEvalTestDefinition;
}

EvalTestDefinition MakeLlmTest(int pId,
                               const EvalCategory pCategory,
                               const std::string& pSTitle,
                               const std::string& pSSourceRepo,
                               const std::string& pSSourceFile,
                               const std::string& pSSourceSymbol,
                               const std::string& pSPrompt,
                               const std::vector<std::string>& pVecSExpectedRepoTokens,
                               const std::vector<std::string>& pVecSExpectedFileFragments,
                               const std::vector<std::string>& pVecSExpectedRetrievalKeywords,
                               const std::size_t piMinRetrievalKeywordMatches,
                               const TextRule& pTextRule,
                               const bool pbRequirePatchLikeOutput) {
  EvalTestDefinition lEvalTestDefinition;
  lEvalTestDefinition.piId = pId;
  lEvalTestDefinition.pSTitle = pSTitle;
  lEvalTestDefinition.pCategory = pCategory;
  lEvalTestDefinition.pSSourceRepo = pSSourceRepo;
  lEvalTestDefinition.pSSourceFile = pSSourceFile;
  lEvalTestDefinition.pSSourceSymbol = pSSourceSymbol;
  lEvalTestDefinition.pSPrompt = pSPrompt;
  lEvalTestDefinition.pVecSExpectedRepoTokens = pVecSExpectedRepoTokens;
  lEvalTestDefinition.pVecSExpectedFileFragments = pVecSExpectedFileFragments;
  lEvalTestDefinition.pVecSExpectedRetrievalKeywords = pVecSExpectedRetrievalKeywords;
  lEvalTestDefinition.piMinRetrievalKeywordMatches = piMinRetrievalKeywordMatches;
  lEvalTestDefinition.pbRequiresModel = true;
  lEvalTestDefinition.pbRequirePatchLikeOutput = pbRequirePatchLikeOutput;
  lEvalTestDefinition.pResponseRule = pTextRule;
  return lEvalTestDefinition;
}

EvalTestDefinition MakeRagSanityTest(int pId,
                                     const std::string& pSTitle,
                                     const std::string& pSOperation) {
  EvalTestDefinition lEvalTestDefinition;
  lEvalTestDefinition.piId = pId;
  lEvalTestDefinition.pSTitle = pSTitle;
  lEvalTestDefinition.pCategory = EvalCategory::RagSanity;
  lEvalTestDefinition.pSRagSanityOperation = pSOperation;
  return lEvalTestDefinition;
}

std::vector<RepoSpec> BuildRepoSpecs() {
  std::vector<RepoSpec> lVecRepoSpecs;
  lVecRepoSpecs.push_back(RepoSpec{
      "benchmark",
      "https://github.com/google/benchmark",
      {"include/benchmark/registration.h", "include/benchmark/benchmark.h", "test/basic_test.cc", "test/fixture_test.cc",
       "test/register_benchmark_test.cc"}});
  lVecRepoSpecs.push_back(RepoSpec{
      "googletest",
      "https://github.com/google/googletest",
      {"googletest/include/gtest/gtest.h", "googletest/src/gtest.cc"}});
  lVecRepoSpecs.push_back(RepoSpec{
      "abseil-cpp",
      "https://github.com/abseil/abseil-cpp",
      {"absl/container/flat_hash_map.h", "absl/strings/string_view.h", "absl/memory/memory.h"}});
  lVecRepoSpecs.push_back(RepoSpec{
      "folly",
      "https://github.com/facebook/folly",
      {"folly/container/FBVector.h", "folly/AtomicHashMap.h", "folly/Optional.h", "folly/docs/FBVector.md", "folly/docs/AtomicHashMap.md"}});
  lVecRepoSpecs.push_back(RepoSpec{
      "nanobench",
      "https://github.com/martinus/nanobench",
      {"src/include/nanobench.h"}});
  lVecRepoSpecs.push_back(RepoSpec{
      "Catch2",
      "https://github.com/catchorg/Catch2",
      {"src/catch2/catch_test_macros.hpp", "src/catch2/benchmark/catch_benchmark.hpp", "docs/test-fixtures.md"}});
  lVecRepoSpecs.push_back(RepoSpec{
      "C-Plus-Plus",
      "https://github.com/TheAlgorithms/C-Plus-Plus",
      {"sorting/quick_sort.cpp", "graph/dijkstra.cpp", "others/lru_cache.cpp", "dynamic_programming/0_1_knapsack.cpp"}});
  return lVecRepoSpecs;
}

std::vector<EvalTestDefinition> BuildEvaluationDefinitions() {
  std::vector<EvalTestDefinition> lVecEvalTestDefinitions;
  lVecEvalTestDefinitions.reserve(50);

  lVecEvalTestDefinitions.push_back(MakeRetrievalTest(
      1, "Retrieval benchmark registration macro", "benchmark", "include/benchmark/registration.h", "BENCHMARK",
      "Find where Google Benchmark defines the BENCHMARK registration macro.", {"benchmark"},
      {"include/benchmark/registration.h"}, {"BENCHMARK", "RegisterBenchmarkInternal", "FunctionBenchmark"}, 2));

  lVecEvalTestDefinitions.push_back(MakeRetrievalTest(
      2, "Retrieval benchmark fixture registration", "benchmark", "include/benchmark/registration.h",
      "BENCHMARK_DEFINE_F", "Find BENCHMARK_DEFINE_F and BENCHMARK_REGISTER_F usage and declarations.", {"benchmark"},
      {"include/benchmark/registration.h", "test/fixture_test.cc"},
      {"BENCHMARK_DEFINE_F", "BENCHMARK_REGISTER_F", "BenchmarkCase"}, 2));

  lVecEvalTestDefinitions.push_back(MakeRetrievalTest(
      3, "Retrieval benchmark DoNotOptimize loops", "benchmark", "test/basic_test.cc", "DoNotOptimize",
      "Retrieve benchmark examples using State loops and DoNotOptimize.", {"benchmark"}, {"test/basic_test.cc"},
      {"State", "DoNotOptimize", "BENCHMARK"}, 2));

  lVecEvalTestDefinitions.push_back(MakeRetrievalTest(
      4, "Retrieval gtest fixture macros", "googletest", "googletest/include/gtest/gtest.h", "TEST_F",
      "Locate TEST_F fixture usage and SetUp/TearDown patterns in GoogleTest.", {"googletest", "gtest"},
      {"googletest/include/gtest/gtest.h"}, {"TEST_F", "SetUp", "TearDown"}, 2));

  lVecEvalTestDefinitions.push_back(MakeRetrievalTest(
      5, "Retrieval gtest assertion definitions", "googletest", "googletest/include/gtest/gtest.h", "ASSERT_EQ",
      "Find where ASSERT_EQ and EXPECT_EQ are defined in GoogleTest.", {"googletest", "gtest"},
      {"googletest/include/gtest/gtest.h"}, {"ASSERT_EQ", "EXPECT_EQ", "GTEST_ASSERT_EQ"}, 2));

  lVecEvalTestDefinitions.push_back(MakeRetrievalTest(
      6, "Retrieval absl flat_hash_map guidance", "abseil-cpp", "absl/container/flat_hash_map.h", "flat_hash_map",
      "Retrieve flat_hash_map performance and pointer-stability guidance.", {"abseil-cpp", "absl"},
      {"absl/container/flat_hash_map.h"}, {"flat_hash_map", "pointer stability", "rehash"}, 2));

  lVecEvalTestDefinitions.push_back(MakeRetrievalTest(
      7, "Retrieval absl string_view API", "abseil-cpp", "absl/strings/string_view.h", "string_view",
      "Find the absl string_view declaration and key semantics.", {"abseil-cpp", "absl"},
      {"absl/strings/string_view.h"}, {"string_view", "class", "view"}, 1));

  lVecEvalTestDefinitions.push_back(MakeRetrievalTest(
      8, "Retrieval folly FBVector growth", "folly", "folly/container/FBVector.h", "reserve_in_place",
      "Retrieve FBVector growth strategy and reserve_in_place behavior.", {"folly"}, {"folly/container/FBVector.h"},
      {"FBVector", "reserve_in_place", "growth"}, 2));

  lVecEvalTestDefinitions.push_back(MakeRetrievalTest(
      9, "Retrieval folly AtomicHashMap design", "folly", "folly/AtomicHashMap.h", "AtomicHashMap",
      "Find AtomicHashMap capacity and growth behavior in folly.", {"folly"}, {"folly/AtomicHashMap.h"},
      {"AtomicHashMap", "capacity", "growth"}, 2));

  lVecEvalTestDefinitions.push_back(MakeRetrievalTest(
      10, "Retrieval nanobench Bench run API", "nanobench", "src/include/nanobench.h", "Bench::run",
      "Retrieve nanobench Bench run() and doNotOptimizeAway usage.", {"nanobench"}, {"src/include/nanobench.h"},
      {"Bench", "run", "doNotOptimizeAway"}, 2));

  lVecEvalTestDefinitions.push_back(MakeRetrievalTest(
      11, "Retrieval Catch2 TEST_CASE macros", "Catch2", "src/catch2/catch_test_macros.hpp", "TEST_CASE",
      "Find Catch2 TEST_CASE, SECTION, and REQUIRE macro definitions.", {"catch2", "Catch2"},
      {"src/catch2/catch_test_macros.hpp"}, {"TEST_CASE", "SECTION", "REQUIRE"}, 2));

  lVecEvalTestDefinitions.push_back(MakeRetrievalTest(
      12, "Retrieval Catch2 BENCHMARK macros", "Catch2", "src/catch2/benchmark/catch_benchmark.hpp", "BENCHMARK",
      "Retrieve Catch2 BENCHMARK and BENCHMARK_ADVANCED macro definitions.", {"catch2", "Catch2"},
      {"src/catch2/benchmark/catch_benchmark.hpp"}, {"BENCHMARK", "BENCHMARK_ADVANCED", "INTERNAL_CATCH_BENCHMARK"},
      2));

  lVecEvalTestDefinitions.push_back(MakeRetrievalTest(
      13, "Retrieval quick_sort partition", "C-Plus-Plus", "sorting/quick_sort.cpp", "partition",
      "Find the quick_sort partition implementation in TheAlgorithms C++ repository.", {"C-Plus-Plus"},
      {"sorting/quick_sort.cpp"}, {"partition", "quick_sort", "pivot"}, 2));

  lVecEvalTestDefinitions.push_back(MakeRetrievalTest(
      14, "Retrieval dijkstra shortest path", "C-Plus-Plus", "graph/dijkstra.cpp", "dijkstra",
      "Find the dijkstra shortest path implementation in TheAlgorithms C++ repository.", {"C-Plus-Plus"},
      {"graph/dijkstra.cpp"}, {"dijkstra", "shortest", "adj"}, 2));

  lVecEvalTestDefinitions.push_back(MakeRetrievalTest(
      15, "Retrieval LRU cache list-map pattern", "C-Plus-Plus", "others/lru_cache.cpp", "LRUCache",
      "Retrieve the LRUCache implementation using list and unordered_map.", {"C-Plus-Plus"},
      {"others/lru_cache.cpp"}, {"LRUCache", "unordered_map", "list"}, 2));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      16, EvalCategory::Explanation, "Explain benchmark registration macro", "benchmark",
      "include/benchmark/registration.h", "BENCHMARK",
      "Explain how BENCHMARK registration macros connect a function to benchmark execution.", {"benchmark"},
      {"include/benchmark/registration.h"}, {"BENCHMARK", "RegisterBenchmarkInternal"}, 1,
      TextRule{{}, {"macro", "register", "benchmark", "function"}, {"i do not know"}, 2}, false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      17, EvalCategory::Explanation, "Explain benchmark State loop semantics", "benchmark", "test/basic_test.cc",
      "State", "Explain what the benchmark::State loop and DoNotOptimize are doing.", {"benchmark"},
      {"test/basic_test.cc"}, {"State", "DoNotOptimize"}, 1,
      TextRule{{}, {"state", "loop", "iteration", "donotoptimize"}, {"i do not know"}, 2}, false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      18, EvalCategory::Explanation, "Explain gtest fixture lifecycle", "googletest",
      "googletest/include/gtest/gtest.h", "TEST_F",
      "Explain TEST_F fixtures and the role of SetUp and TearDown.", {"googletest", "gtest"},
      {"googletest/include/gtest/gtest.h"}, {"TEST_F", "SetUp", "TearDown"}, 1,
      TextRule{{}, {"fixture", "setup", "teardown", "test_f"}, {"i do not know"}, 2}, false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      19, EvalCategory::Explanation, "Explain ASSERT_EQ versus EXPECT_EQ", "googletest",
      "googletest/include/gtest/gtest.h", "ASSERT_EQ",
      "Explain the behavior difference between ASSERT_EQ and EXPECT_EQ.", {"googletest", "gtest"},
      {"googletest/include/gtest/gtest.h"}, {"ASSERT_EQ", "EXPECT_EQ"}, 1,
      TextRule{{}, {"assert", "expect", "fatal", "non-fatal"}, {"i do not know"}, 2}, false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      20, EvalCategory::Explanation, "Explain absl flat_hash_map tradeoffs", "abseil-cpp",
      "absl/container/flat_hash_map.h", "flat_hash_map",
      "Explain flat_hash_map performance benefits and pointer-stability tradeoffs.", {"abseil-cpp", "absl"},
      {"absl/container/flat_hash_map.h"}, {"flat_hash_map", "pointer stability", "rehash"}, 1,
      TextRule{{}, {"hash", "performance", "pointer", "rehash", "capacity"}, {"i do not know"}, 2}, false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      21, EvalCategory::Explanation, "Explain absl string_view ownership", "abseil-cpp", "absl/strings/string_view.h",
      "string_view", "Explain ownership and lifetime expectations for string_view-like APIs.",
      {"abseil-cpp", "absl"}, {"absl/strings/string_view.h"}, {"string_view", "view"}, 1,
      TextRule{{}, {"non-owning", "lifetime", "view", "string"}, {"i do not know"}, 2}, false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      22, EvalCategory::Explanation, "Explain folly AtomicHashMap capacity model", "folly", "folly/AtomicHashMap.h",
      "AtomicHashMap", "Explain AtomicHashMap capacity, growth behavior, and constraints.", {"folly"},
      {"folly/AtomicHashMap.h"}, {"AtomicHashMap", "capacity", "growth"}, 1,
      TextRule{{}, {"atomic", "hash", "capacity", "growth", "concurrent"}, {"i do not know"}, 2}, false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      23, EvalCategory::Explanation, "Explain folly FBVector growth strategy", "folly", "folly/container/FBVector.h",
      "reserve_in_place", "Explain FBVector growth behavior and reserve_in_place intent.", {"folly"},
      {"folly/container/FBVector.h"}, {"FBVector", "reserve_in_place", "growth"}, 1,
      TextRule{{}, {"vector", "reserve", "growth", "allocation"}, {"i do not know"}, 2}, false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      24, EvalCategory::Explanation, "Explain nanobench run and doNotOptimizeAway", "nanobench",
      "src/include/nanobench.h", "run", "Explain how Bench::run and doNotOptimizeAway are used in nanobench.",
      {"nanobench"}, {"src/include/nanobench.h"}, {"Bench", "run", "doNotOptimizeAway"}, 1,
      TextRule{{}, {"benchmark", "run", "measurement", "donotoptimizeaway"}, {"i do not know"}, 2}, false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      25, EvalCategory::Explanation, "Explain LRU cache list and map coordination", "C-Plus-Plus",
      "others/lru_cache.cpp", "LRUCache", "Explain why LRUCache combines list and unordered_map.", {"C-Plus-Plus"},
      {"others/lru_cache.cpp"}, {"LRUCache", "unordered_map", "list"}, 1,
      TextRule{{}, {"least recently used", "list", "unordered_map", "evict"}, {"i do not know"}, 2}, false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      26, EvalCategory::Optimization, "Optimize quick_sort partition recommendations", "C-Plus-Plus",
      "sorting/quick_sort.cpp", "partition",
      "Recommend concrete performance optimizations for this quick_sort implementation.", {"C-Plus-Plus"},
      {"sorting/quick_sort.cpp"}, {"partition", "quick_sort", "pivot"}, 1,
      TextRule{{}, {"pivot", "copy", "recursion", "cache", "allocation", "iterative"}, {"i do not know"}, 2},
      false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      27, EvalCategory::Optimization, "Optimize dijkstra recommendations", "C-Plus-Plus", "graph/dijkstra.cpp",
      "dijkstra", "Recommend concrete performance optimizations for this dijkstra implementation.",
      {"C-Plus-Plus"}, {"graph/dijkstra.cpp"}, {"dijkstra", "adj"}, 1,
      TextRule{{}, {"priority", "queue", "adjacency", "reserve", "early"}, {"i do not know"}, 2}, false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      28, EvalCategory::Optimization, "Optimize LRU cache refer path", "C-Plus-Plus", "others/lru_cache.cpp", "refer",
      "Recommend performance improvements for refer() in this LRU cache implementation.", {"C-Plus-Plus"},
      {"others/lru_cache.cpp"}, {"LRUCache", "refer"}, 1,
      TextRule{{}, {"find", "unordered_map", "list", "erase", "splice"}, {"i do not know"}, 2}, false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      29, EvalCategory::Optimization, "Optimize benchmark setup recommendations", "benchmark", "test/basic_test.cc",
      "BM_pause_during", "Recommend improvements to isolate setup from measured benchmark time.", {"benchmark"},
      {"test/basic_test.cc"}, {"State", "DoNotOptimize"}, 1,
      TextRule{{}, {"setup", "loop", "pausetiming", "resumetiming", "measurement"}, {"i do not know"}, 2},
      false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      30, EvalCategory::Optimization, "Optimize gtest fixture usage", "googletest", "googletest/include/gtest/gtest.h",
      "TEST_F", "Recommend maintainability and runtime optimizations for fixture-based tests.",
      {"googletest", "gtest"}, {"googletest/include/gtest/gtest.h"}, {"TEST_F", "SetUp"}, 1,
      TextRule{{}, {"fixture", "setup", "teardown", "parameterized", "shared"}, {"i do not know"}, 2}, false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      31, EvalCategory::Optimization, "Optimize flat_hash_map usage guidance", "abseil-cpp",
      "absl/container/flat_hash_map.h", "flat_hash_map",
      "Recommend practical optimizations for production usage of absl::flat_hash_map.", {"abseil-cpp", "absl"},
      {"absl/container/flat_hash_map.h"}, {"flat_hash_map", "capacity"}, 1,
      TextRule{{}, {"reserve", "rehash", "emplace", "try_emplace", "allocation"}, {"i do not know"}, 2},
      false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      32, EvalCategory::Optimization, "Optimize FBVector allocation behavior", "folly", "folly/container/FBVector.h",
      "reserve", "Recommend optimization ideas for FBVector allocation and growth behavior.", {"folly"},
      {"folly/container/FBVector.h"}, {"FBVector", "reserve", "growth"}, 1,
      TextRule{{}, {"reserve", "capacity", "reallocation", "move", "allocation"}, {"i do not know"}, 2},
      false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      33, EvalCategory::Optimization, "Optimize AtomicHashMap deployment settings", "folly", "folly/AtomicHashMap.h",
      "AtomicHashMap", "Recommend optimization ideas for AtomicHashMap capacity planning and throughput.", {"folly"},
      {"folly/AtomicHashMap.h"}, {"AtomicHashMap", "capacity"}, 1,
      TextRule{{}, {"capacity", "contention", "growth", "load", "throughput"}, {"i do not know"}, 2}, false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      34, EvalCategory::Optimization, "Optimize nanobench harness design", "nanobench", "src/include/nanobench.h",
      "Bench", "Recommend benchmark harness optimizations using nanobench APIs.", {"nanobench"},
      {"src/include/nanobench.h"}, {"Bench", "run"}, 1,
      TextRule{{}, {"warmup", "batch", "setup", "measurement", "run"}, {"i do not know"}, 2}, false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      35, EvalCategory::Optimization, "Optimize Catch2 benchmark structure", "Catch2",
      "src/catch2/benchmark/catch_benchmark.hpp", "BENCHMARK",
      "Recommend improvements for writing low-noise Catch2 benchmarks and tests.", {"Catch2", "catch2"},
      {"src/catch2/benchmark/catch_benchmark.hpp"}, {"BENCHMARK", "TEST_CASE"}, 1,
      TextRule{{}, {"benchmark", "setup", "section", "allocation", "noise"}, {"i do not know"}, 2}, false));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      36, EvalCategory::Patch, "Patch quick_sort pointer-to-reference refactor", "C-Plus-Plus", "sorting/quick_sort.cpp",
      "partition", "Produce a unified diff that refactors quick_sort/partition signatures from pointer-based vectors to references.",
      {"C-Plus-Plus"}, {"sorting/quick_sort.cpp"}, {"partition", "quick_sort"}, 1,
      TextRule{{"partition", "quick_sort"}, {"std::vector", "&", "swap"}, {"i do not know"}, 2}, true));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      37, EvalCategory::Patch, "Patch LRU refer to reduce hash lookups", "C-Plus-Plus", "others/lru_cache.cpp", "refer",
      "Produce a unified diff that rewrites refer() to avoid repeated unordered_map lookups while preserving behavior.",
      {"C-Plus-Plus"}, {"others/lru_cache.cpp"}, {"refer", "pageMap"}, 1,
      TextRule{{"refer", "pageMap", "cache"}, {"find", "erase", "push_front", "iterator"}, {"i do not know"},
               2},
      true));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      38, EvalCategory::Patch, "Patch gtest fixture example modernization", "googletest",
      "googletest/include/gtest/gtest.h", "TEST_F",
      "Produce a unified diff introducing a concise TEST_F fixture example with SetUp and TearDown.",
      {"googletest", "gtest"}, {"googletest/include/gtest/gtest.h"}, {"TEST_F", "SetUp", "TearDown"}, 1,
      TextRule{{"TEST_F", "SetUp", "TearDown"}, {"ASSERT", "EXPECT", "fixture"}, {"i do not know"}, 1}, true));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      39, EvalCategory::Patch, "Patch benchmark setup timing isolation", "benchmark", "test/basic_test.cc", "State",
      "Produce a unified diff that uses PauseTiming/ResumeTiming to isolate setup work from measured work.",
      {"benchmark"}, {"test/basic_test.cc"}, {"State", "BENCHMARK"}, 1,
      TextRule{{"BENCHMARK"}, {"PauseTiming", "ResumeTiming", "State"}, {"i do not know"}, 2}, true));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      40, EvalCategory::Patch, "Patch flat_hash_map insertion style", "abseil-cpp", "absl/container/flat_hash_map.h",
      "flat_hash_map", "Produce a unified diff replacing operator[] insertion style with try_emplace style examples.",
      {"abseil-cpp", "absl"}, {"absl/container/flat_hash_map.h"}, {"flat_hash_map"}, 1,
      TextRule{{"flat_hash_map", "try_emplace"}, {"reserve", "emplace", "insert"}, {"i do not know"}, 1}, true));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      41, EvalCategory::Patch, "Patch nanobench example with doNotOptimizeAway", "nanobench", "src/include/nanobench.h",
      "Bench", "Produce a unified diff adding a benchmark example that uses Bench().run and doNotOptimizeAway correctly.",
      {"nanobench"}, {"src/include/nanobench.h"}, {"Bench", "run"}, 1,
      TextRule{{"Bench", "run", "doNotOptimizeAway"}, {"nanobench", "benchmark"}, {"i do not know"}, 1}, true));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      42, EvalCategory::Patch, "Patch Catch2 fixture test readability", "Catch2", "docs/test-fixtures.md", "TEST_CASE_METHOD",
      "Produce a unified diff with a compact Catch2 example using TEST_CASE and SECTION for readability.",
      {"Catch2", "catch2"}, {"docs/test-fixtures.md", "src/catch2/catch_test_macros.hpp"},
      {"TEST_CASE", "SECTION", "REQUIRE"}, 1,
      TextRule{{"TEST_CASE", "SECTION", "REQUIRE"}, {"fixture", "method"}, {"i do not know"}, 1}, true));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      43, EvalCategory::Patch, "Patch FBVector reserve-before-push refactor", "folly", "folly/container/FBVector.h",
      "reserve", "Produce a unified diff showing reserve-before-push_back refactor for FBVector-style usage.",
      {"folly"}, {"folly/container/FBVector.h"}, {"FBVector", "reserve"}, 1,
      TextRule{{"FBVector", "reserve", "push_back"}, {"capacity", "reallocation"}, {"i do not know"}, 1}, true));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      44, EvalCategory::Patch, "Patch AtomicHashMap sizing refactor", "folly", "folly/AtomicHashMap.h", "AtomicHashMap",
      "Produce a unified diff demonstrating improved AtomicHashMap finalSizeEst sizing and construction clarity.",
      {"folly"}, {"folly/AtomicHashMap.h"}, {"AtomicHashMap", "finalSizeEst"}, 1,
      TextRule{{"AtomicHashMap", "finalSizeEst"}, {"capacity", "size", "constructor"}, {"i do not know"}, 1},
      true));

  lVecEvalTestDefinitions.push_back(MakeLlmTest(
      45, EvalCategory::Patch, "Patch dijkstra helper extraction", "C-Plus-Plus", "graph/dijkstra.cpp", "dijkstra",
      "Produce a unified diff that extracts edge relaxation into a helper while preserving dijkstra behavior.",
      {"C-Plus-Plus"}, {"graph/dijkstra.cpp"}, {"dijkstra"}, 1,
      TextRule{{"dijkstra"}, {"helper", "relax", "priority_queue"}, {"i do not know"}, 1}, true));

  lVecEvalTestDefinitions.push_back(
      MakeRagSanityTest(46, "RAG scan starts and reports Running", "scan_start_running"));
  lVecEvalTestDefinitions.push_back(
      MakeRagSanityTest(47, "RAG scan reaches Finished with indexed chunks", "scan_finish_nonzero"));
  lVecEvalTestDefinitions.push_back(
      MakeRagSanityTest(48, "RAG retrieval returns non-empty snippets after load", "retrieve_after_load"));
  lVecEvalTestDefinitions.push_back(
      MakeRagSanityTest(49, "RAG rescan of existing corpus succeeds", "rescan_existing"));
  lVecEvalTestDefinitions.push_back(
      MakeRagSanityTest(50, "RAG load of multi-database set succeeds", "load_multi_database"));

  return lVecEvalTestDefinitions;
}

bool EnsureSequentialFifty(const std::vector<EvalTestDefinition>& pVecEvalTestDefinitions, std::string* pSErrorOut) {
  if (pVecEvalTestDefinitions.size() != 50) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Expected exactly 50 tests, got " + std::to_string(pVecEvalTestDefinitions.size());
    }
    return false;
  }
  for (std::size_t liIndex = 0; liIndex < pVecEvalTestDefinitions.size(); ++liIndex) {
    if (pVecEvalTestDefinitions[liIndex].piId != static_cast<int>(liIndex + 1)) {
      if (pSErrorOut != nullptr) {
        *pSErrorOut = "Test IDs are not sequential at index " + std::to_string(liIndex);
      }
      return false;
    }
  }
  return true;
}

EvalTestResult EvaluateRetrievalTest(const EvalTestDefinition& pEvalTestDefinition,
                                     ollama_engine::EngineInterface* pPtrEngine) {
  EvalTestResult lEvalTestResult;
  lEvalTestResult.piId = pEvalTestDefinition.piId;
  lEvalTestResult.pSTitle = pEvalTestDefinition.pSTitle;
  lEvalTestResult.pCategory = pEvalTestDefinition.pCategory;
  lEvalTestResult.pSPrompt = pEvalTestDefinition.pSPrompt;

  if (pPtrEngine == nullptr) {
    lEvalTestResult.pbPass = false;
    lEvalTestResult.pSNotes = "Engine is not available.";
    return lEvalTestResult;
  }

  lEvalTestResult.pVecSSnippets = pPtrEngine->Fetch_Relevant_Info(pEvalTestDefinition.pSPrompt, 8, 2);
  const RetrievalCheckOutcome lRetrievalCheckOutcome =
      EvaluateRetrievalSignals(pEvalTestDefinition, lEvalTestResult.pVecSSnippets);
  lEvalTestResult.pbPass = lRetrievalCheckOutcome.pbPass;
  lEvalTestResult.pSNotes = lRetrievalCheckOutcome.pSNotes;
  return lEvalTestResult;
}

EvalTestResult EvaluateModelDrivenTest(const EvalTestDefinition& pEvalTestDefinition,
                                       ollama_engine::EngineInterface* pPtrEngine,
                                       const RunContext& pRunContext) {
  EvalTestResult lEvalTestResult;
  lEvalTestResult.piId = pEvalTestDefinition.piId;
  lEvalTestResult.pSTitle = pEvalTestDefinition.pSTitle;
  lEvalTestResult.pCategory = pEvalTestDefinition.pCategory;
  lEvalTestResult.pSPrompt = pEvalTestDefinition.pSPrompt;

  if (pPtrEngine == nullptr) {
    lEvalTestResult.pbPass = false;
    lEvalTestResult.pSNotes = "Engine is not available.";
    return lEvalTestResult;
  }

  lEvalTestResult.pVecSSnippets = pPtrEngine->Fetch_Relevant_Info(pEvalTestDefinition.pSPrompt, 8, 2);
  const RetrievalCheckOutcome lRetrievalCheckOutcome =
      EvaluateRetrievalSignals(pEvalTestDefinition, lEvalTestResult.pVecSSnippets);

  if (!pRunContext.pbModelLoaded) {
    lEvalTestResult.pbPass = false;
    lEvalTestResult.pSNotes = "Model is not loaded for model-driven test.";
    return lEvalTestResult;
  }

  const auto lIterRepoPath = pRunContext.pMapSRepoNameToPath.find(pEvalTestDefinition.pSSourceRepo);
  std::string lSSourceExcerpt;
  if (lIterRepoPath != pRunContext.pMapSRepoNameToPath.end()) {
    const fs::path lPathSourceFile = lIterRepoPath->second / pEvalTestDefinition.pSSourceFile;
    lSSourceExcerpt = BuildSourceExcerpt(lPathSourceFile, pEvalTestDefinition.pSSourceSymbol);
  }
  if (lSSourceExcerpt.empty()) {
    lSSourceExcerpt = "// Source excerpt not found; rely on retrieved snippets.";
  }

  const std::string lSModelPrompt = BuildModelPrompt(pEvalTestDefinition, lSSourceExcerpt, lEvalTestResult.pVecSSnippets);
  const ollama_engine::SendMessageResponse lSendMessageResponse = pPtrEngine->SendMessage(lSModelPrompt);
  lEvalTestResult.pSResponse = lSendMessageResponse.pSText;

  if (!lSendMessageResponse.pbOk) {
    lEvalTestResult.pbPass = false;
    lEvalTestResult.pSNotes = "SendMessage failed: " + lSendMessageResponse.pSError;
    return lEvalTestResult;
  }

  std::string lSRuleNotes;
  const bool lbTextRulePass = EvaluateTextRule(lSendMessageResponse.pSText, pEvalTestDefinition.pResponseRule, &lSRuleNotes);

  bool lbPatchOk = true;
  if (pEvalTestDefinition.pbRequirePatchLikeOutput) {
    lbPatchOk = LooksLikePatchOutput(lSendMessageResponse.pSText);
  }

  lEvalTestResult.pbPass = lRetrievalCheckOutcome.pbPass && lbTextRulePass && lbPatchOk;

  std::ostringstream lNotes;
  if (!lRetrievalCheckOutcome.pbPass) {
    lNotes << "Retrieval precheck failed: " << lRetrievalCheckOutcome.pSNotes << " ";
  }
  if (!lbTextRulePass) {
    lNotes << "Text rule failed: " << lSRuleNotes << " ";
  }
  if (!lbPatchOk) {
    lNotes << "Patch-like structure not detected. ";
  }
  lEvalTestResult.pSNotes = TrimAscii(lNotes.str());
  return lEvalTestResult;
}

EvalTestResult EvaluateRagSanityTest(const EvalTestDefinition& pEvalTestDefinition,
                                     ollama_engine::EngineInterface* pPtrEngine,
                                     RunContext* pPtrRunContext) {
  EvalTestResult lEvalTestResult;
  lEvalTestResult.piId = pEvalTestDefinition.piId;
  lEvalTestResult.pSTitle = pEvalTestDefinition.pSTitle;
  lEvalTestResult.pCategory = pEvalTestDefinition.pCategory;
  lEvalTestResult.pSPrompt = pEvalTestDefinition.pSRagSanityOperation;

  if (pPtrEngine == nullptr || pPtrRunContext == nullptr) {
    lEvalTestResult.pbPass = false;
    lEvalTestResult.pSNotes = "Engine/context unavailable for RAG sanity test.";
    return lEvalTestResult;
  }

  const auto lIterBenchmarkCorpus = pPtrRunContext->pMapSRepoNameToCorpusPath.find("benchmark");
  if (lIterBenchmarkCorpus == pPtrRunContext->pMapSRepoNameToCorpusPath.end()) {
    lEvalTestResult.pbPass = false;
    lEvalTestResult.pSNotes = "Benchmark corpus path missing.";
    return lEvalTestResult;
  }

  const fs::path lPathBenchmarkCorpus = lIterBenchmarkCorpus->second;
  const std::string lSSanityDbName = pPtrRunContext->pSSanityDatabaseName.empty()
                                         ? std::string("eval_sanity_benchmark")
                                         : pPtrRunContext->pSSanityDatabaseName;

  auto EnsurePrimaryScan = [&]() {
    if (pPtrRunContext->pbSanityPrimaryRunDone) {
      return;
    }
    pPtrRunContext->pSanityPrimaryScan =
        RunScanAndWait(pPtrEngine, lPathBenchmarkCorpus, lSSanityDbName, std::chrono::seconds(180));
    pPtrRunContext->pbSanityPrimaryRunDone = true;
    pPtrRunContext->pSSanityDatabaseName = lSSanityDbName;
  };

  if (pEvalTestDefinition.pSRagSanityOperation == "scan_start_running") {
    EnsurePrimaryScan();
    lEvalTestResult.pbPass = pPtrRunContext->pSanityPrimaryScan.pbStarted &&
                             pPtrRunContext->pSanityPrimaryScan.pbRunningSeen;
    if (!lEvalTestResult.pbPass) {
      lEvalTestResult.pSNotes = "Running state was not observed during scan.";
    }
    return lEvalTestResult;
  }

  if (pEvalTestDefinition.pSRagSanityOperation == "scan_finish_nonzero") {
    EnsurePrimaryScan();
    lEvalTestResult.pbPass = pPtrRunContext->pSanityPrimaryScan.pbFinishedSeen &&
                             !pPtrRunContext->pSanityPrimaryScan.pbTimedOut &&
                             (pPtrRunContext->pSanityPrimaryScan.piVectorDatabaseSize > 0);
    if (!lEvalTestResult.pbPass) {
      lEvalTestResult.pSNotes = "Primary scan did not finish with non-zero vector rows.";
    }
    return lEvalTestResult;
  }

  if (pEvalTestDefinition.pSRagSanityOperation == "retrieve_after_load") {
    EnsurePrimaryScan();
    std::string lSError;
    const bool lbLoadOk = pPtrEngine->LoadRagDatabases({lSSanityDbName}, &lSError);
    lEvalTestResult.pVecSSnippets = pPtrEngine->Fetch_Relevant_Info("BENCHMARK macro register benchmark fixture", 6, 1);
    lEvalTestResult.pbPass = lbLoadOk && !lEvalTestResult.pVecSSnippets.empty();
    if (!lEvalTestResult.pbPass) {
      lEvalTestResult.pSNotes = "Load/query failed: " + lSError;
    }
    return lEvalTestResult;
  }

  if (pEvalTestDefinition.pSRagSanityOperation == "rescan_existing") {
    const ScanReport lRescanReport =
        RunScanAndWait(pPtrEngine, lPathBenchmarkCorpus, lSSanityDbName, std::chrono::seconds(180));
    lEvalTestResult.pbPass = lRescanReport.pbStarted && lRescanReport.pbFinishedSeen && !lRescanReport.pbTimedOut &&
                             (lRescanReport.piVectorDatabaseSize > 0);
    if (!lEvalTestResult.pbPass) {
      lEvalTestResult.pSNotes = "Rescan did not complete cleanly.";
    }
    return lEvalTestResult;
  }

  if (pEvalTestDefinition.pSRagSanityOperation == "load_multi_database") {
    std::vector<std::string> lVecSAllDatabases = pPtrRunContext->pVecSDatabaseNames;
    lVecSAllDatabases.push_back(lSSanityDbName);
    std::sort(lVecSAllDatabases.begin(), lVecSAllDatabases.end());
    lVecSAllDatabases.erase(std::unique(lVecSAllDatabases.begin(), lVecSAllDatabases.end()), lVecSAllDatabases.end());

    std::string lSError;
    const bool lbLoadOk = pPtrEngine->LoadRagDatabases(lVecSAllDatabases, &lSError);
    lEvalTestResult.pVecSSnippets =
        pPtrEngine->Fetch_Relevant_Info("quick_sort partition TEST_F BENCHMARK flat_hash_map", 8, 2);

    const std::string lSJoined = JoinSnippets(lEvalTestResult.pVecSSnippets, 8);
    const bool lbContainsAnyExpectedRepo = ContainsCaseInsensitive(lSJoined, "benchmark") ||
                                           ContainsCaseInsensitive(lSJoined, "googletest") ||
                                           ContainsCaseInsensitive(lSJoined, "C-Plus-Plus");

    lEvalTestResult.pbPass = lbLoadOk && !lEvalTestResult.pVecSSnippets.empty() && lbContainsAnyExpectedRepo;
    if (!lEvalTestResult.pbPass) {
      lEvalTestResult.pSNotes = "Multi-database load/query failed: " + lSError;
    }
    return lEvalTestResult;
  }

  lEvalTestResult.pbPass = false;
  lEvalTestResult.pSNotes = "Unknown RAG sanity operation: " + pEvalTestDefinition.pSRagSanityOperation;
  return lEvalTestResult;
}

bool WriteRepoManifestJson(const std::vector<RepoMaterialized>& pVecRepos, const fs::path& pPathOutput) {
  std::error_code lErrorCode;
  fs::create_directories(pPathOutput.parent_path(), lErrorCode);

  std::ofstream lFileOut(pPathOutput, std::ios::out | std::ios::trunc);
  if (!lFileOut.good()) {
    return false;
  }

  lFileOut << "{\n";
  lFileOut << "  \"repos\": [\n";
  for (std::size_t liIndex = 0; liIndex < pVecRepos.size(); ++liIndex) {
    const RepoMaterialized& lRepo = pVecRepos[liIndex];
    lFileOut << "    {\n";
    lFileOut << "      \"name\": \"" << EscapeJson(lRepo.pRepoSpec.pSName) << "\",\n";
    lFileOut << "      \"url\": \"" << EscapeJson(lRepo.pRepoSpec.pSRemoteUrl) << "\",\n";
    lFileOut << "      \"path\": \"" << EscapeJson(lRepo.pPathRepository.string()) << "\",\n";
    lFileOut << "      \"corpus_path\": \"" << EscapeJson(lRepo.pPathCorpus.string()) << "\",\n";
    lFileOut << "      \"commit\": \"" << EscapeJson(lRepo.pSCommitHash) << "\",\n";
    lFileOut << "      \"note\": \"" << EscapeJson(lRepo.pSNote) << "\"\n";
    lFileOut << "    }";
    if (liIndex + 1 < pVecRepos.size()) {
      lFileOut << ",";
    }
    lFileOut << "\n";
  }
  lFileOut << "  ]\n";
  lFileOut << "}\n";
  return true;
}

bool WriteResultsJson(const std::vector<EvalTestResult>& pVecEvalTestResults,
                      const fs::path& pPathOutput,
                      const RunContext& pRunContext) {
  std::error_code lErrorCode;
  fs::create_directories(pPathOutput.parent_path(), lErrorCode);

  std::ofstream lFileOut(pPathOutput, std::ios::out | std::ios::trunc);
  if (!lFileOut.good()) {
    return false;
  }

  lFileOut << "{\n";
  lFileOut << "  \"model_loaded\": " << (pRunContext.pbModelLoaded ? "true" : "false") << ",\n";
  lFileOut << "  \"loaded_model_name\": \"" << EscapeJson(pRunContext.pSLoadedModelName) << "\",\n";
  lFileOut << "  \"initial_database_load_ok\": " << (pRunContext.pbInitialDatabaseLoadOk ? "true" : "false") << ",\n";
  lFileOut << "  \"initial_database_load_error\": \"" << EscapeJson(pRunContext.pSInitialDatabaseLoadError) << "\",\n";
  lFileOut << "  \"tests\": [\n";

  for (std::size_t liIndex = 0; liIndex < pVecEvalTestResults.size(); ++liIndex) {
    const EvalTestResult& lEvalTestResult = pVecEvalTestResults[liIndex];

    lFileOut << "    {\n";
    lFileOut << "      \"id\": " << lEvalTestResult.piId << ",\n";
    lFileOut << "      \"title\": \"" << EscapeJson(lEvalTestResult.pSTitle) << "\",\n";
    lFileOut << "      \"category\": \"" << EscapeJson(CategoryToString(lEvalTestResult.pCategory)) << "\",\n";
    lFileOut << "      \"prompt\": \"" << EscapeJson(lEvalTestResult.pSPrompt) << "\",\n";
    lFileOut << "      \"pass\": " << (lEvalTestResult.pbPass ? "true" : "false") << ",\n";
    lFileOut << "      \"notes\": \"" << EscapeJson(lEvalTestResult.pSNotes) << "\",\n";
    lFileOut << "      \"response\": \"" << EscapeJson(lEvalTestResult.pSResponse) << "\",\n";
    lFileOut << "      \"snippets\": [";
    for (std::size_t liSnippetIndex = 0; liSnippetIndex < lEvalTestResult.pVecSSnippets.size(); ++liSnippetIndex) {
      lFileOut << "\"" << EscapeJson(lEvalTestResult.pVecSSnippets[liSnippetIndex]) << "\"";
      if (liSnippetIndex + 1 < lEvalTestResult.pVecSSnippets.size()) {
        lFileOut << ", ";
      }
    }
    lFileOut << "]\n";
    lFileOut << "    }";
    if (liIndex + 1 < pVecEvalTestResults.size()) {
      lFileOut << ",";
    }
    lFileOut << "\n";
  }

  lFileOut << "  ]\n";
  lFileOut << "}\n";
  return true;
}

void PrintRequiredOutput(const std::vector<EvalTestResult>& pVecEvalTestResults) {
  for (const EvalTestResult& lEvalTestResult : pVecEvalTestResults) {
    std::cout << "Test " << lEvalTestResult.piId << ": " << lEvalTestResult.pSTitle << " ("
              << (lEvalTestResult.pbPass ? "Pass" : "Fail") << ")\n";
  }
}

RunnerPaths ResolveRunnerPaths(int argc, char** argv) {
  RunnerPaths lRunnerPaths;
  lRunnerPaths.pPathModelFolder = fs::current_path() / "models";
  lRunnerPaths.pPathEvalRoot = fs::current_path() / "evaluation";

  if (argc > 1 && argv[1] != nullptr && std::string(argv[1]).empty() == false) {
    lRunnerPaths.pPathModelFolder = fs::path(argv[1]);
  }
  if (argc > 2 && argv[2] != nullptr && std::string(argv[2]).empty() == false) {
    lRunnerPaths.pPathEvalRoot = fs::path(argv[2]);
  }

  lRunnerPaths.pPathReferenceRepos = lRunnerPaths.pPathEvalRoot / "reference_repos";
  lRunnerPaths.pPathIndexCorpus = lRunnerPaths.pPathEvalRoot / "index_corpus";
  lRunnerPaths.pPathResults = lRunnerPaths.pPathEvalRoot / "results";
  return lRunnerPaths;
}

}  // namespace

int main(int argc, char** argv) {
  const RunnerPaths lRunnerPaths = ResolveRunnerPaths(argc, argv);

  std::error_code lErrorCode;
  fs::create_directories(lRunnerPaths.pPathEvalRoot, lErrorCode);
  fs::create_directories(lRunnerPaths.pPathReferenceRepos, lErrorCode);
  fs::create_directories(lRunnerPaths.pPathIndexCorpus, lErrorCode);
  fs::create_directories(lRunnerPaths.pPathResults, lErrorCode);

  std::vector<RepoMaterialized> lVecRepos;
  for (const RepoSpec& lRepoSpec : BuildRepoSpecs()) {
    RepoMaterialized lRepoMaterialized;
    std::string lSRepoError;

    if (!EnsureRepository(lRepoSpec, lRunnerPaths.pPathReferenceRepos, &lRepoMaterialized, &lSRepoError)) {
      lRepoMaterialized.pRepoSpec = lRepoSpec;
      lRepoMaterialized.pPathRepository = lRunnerPaths.pPathReferenceRepos / lRepoSpec.pSName;
      lRepoMaterialized.pPathCorpus = lRunnerPaths.pPathIndexCorpus / lRepoSpec.pSName;
      lRepoMaterialized.pSNote = "fetch_failed: " + lSRepoError;
      lVecRepos.push_back(std::move(lRepoMaterialized));
      continue;
    }

    std::string lSCorpusError;
    if (!PrepareRepoCorpus(&lRepoMaterialized, lRunnerPaths.pPathIndexCorpus, &lSCorpusError)) {
      lRepoMaterialized.pSNote += " | corpus_failed: " + lSCorpusError;
    }

    lVecRepos.push_back(std::move(lRepoMaterialized));
  }

  WriteRepoManifestJson(lVecRepos, lRunnerPaths.pPathResults / "repo_manifest.json");

  RunContext lRunContext;
  lRunContext.pRunnerPaths = lRunnerPaths;

  ollama_engine::EngineOptions lEngineOptions;
  lEngineOptions.pPathModelFolder = lRunnerPaths.pPathModelFolder;
  lEngineOptions.piEmbeddingDimensions = 256;
  lEngineOptions.pRagRuntimeMode = ollama_engine::RagRuntimeMode::Deterministic;

  std::unique_ptr<ollama_engine::EngineInterface> lPtrEngine = ollama_engine::CreateEngine(lEngineOptions);

  if (lPtrEngine) {
    const std::vector<std::string> lVecSModels = lPtrEngine->ListModels();
    if (!lVecSModels.empty()) {
      const std::string lSModelName = lVecSModels.front();
      std::string lSLoadError;
      if (lPtrEngine->Load(lSModelName, &lSLoadError)) {
        lRunContext.pbModelLoaded = true;
        lRunContext.pSLoadedModelName = lSModelName;
      }
    }
  }

  for (const RepoMaterialized& lRepoMaterialized : lVecRepos) {
    if (!lRepoMaterialized.pbFetchOk || !lRepoMaterialized.pbCorpusPrepared) {
      continue;
    }

    lRunContext.pMapSRepoNameToPath[lRepoMaterialized.pRepoSpec.pSName] = lRepoMaterialized.pPathRepository;
    lRunContext.pMapSRepoNameToCorpusPath[lRepoMaterialized.pRepoSpec.pSName] = lRepoMaterialized.pPathCorpus;

    const std::string lSDatabaseName = SanitizeDatabaseName("eval_" + lRepoMaterialized.pRepoSpec.pSName);
    lRunContext.pVecSDatabaseNames.push_back(lSDatabaseName);

    if (!lPtrEngine) {
      continue;
    }

    const ScanReport lScanReport =
        RunScanAndWait(lPtrEngine.get(), lRepoMaterialized.pPathCorpus, lSDatabaseName, std::chrono::seconds(180));
    (void)lScanReport;
  }

  if (lPtrEngine != nullptr) {
    std::string lSLoadDatabasesError;
    lRunContext.pbInitialDatabaseLoadOk = lPtrEngine->LoadRagDatabases(lRunContext.pVecSDatabaseNames, &lSLoadDatabasesError);
    lRunContext.pSInitialDatabaseLoadError = lSLoadDatabasesError;
  }

  const std::vector<EvalTestDefinition> lVecEvalTestDefinitions = BuildEvaluationDefinitions();
  std::string lSTestDefinitionError;
  if (!EnsureSequentialFifty(lVecEvalTestDefinitions, &lSTestDefinitionError)) {
    std::vector<EvalTestResult> lVecEvalTestResults;
    lVecEvalTestResults.reserve(50);
    for (int liTestId = 1; liTestId <= 50; ++liTestId) {
      EvalTestResult lEvalTestResult;
      lEvalTestResult.piId = liTestId;
      lEvalTestResult.pSTitle = (liTestId == 1) ? "Test definition validation" : ("Test definition placeholder " + std::to_string(liTestId));
      lEvalTestResult.pbPass = false;
      lEvalTestResult.pSNotes = lSTestDefinitionError;
      lVecEvalTestResults.push_back(std::move(lEvalTestResult));
    }
    PrintRequiredOutput(lVecEvalTestResults);
    WriteResultsJson(lVecEvalTestResults, lRunnerPaths.pPathResults / "latest_results.json", lRunContext);
    return 2;
  }

  std::vector<EvalTestResult> lVecEvalTestResults;
  lVecEvalTestResults.reserve(lVecEvalTestDefinitions.size());

  for (const EvalTestDefinition& lEvalTestDefinition : lVecEvalTestDefinitions) {
    EvalTestResult lEvalTestResult;

    if (lEvalTestDefinition.pCategory == EvalCategory::Retrieval) {
      lEvalTestResult = EvaluateRetrievalTest(lEvalTestDefinition, lPtrEngine.get());
    } else if (lEvalTestDefinition.pCategory == EvalCategory::RagSanity) {
      lEvalTestResult = EvaluateRagSanityTest(lEvalTestDefinition, lPtrEngine.get(), &lRunContext);
    } else {
      lEvalTestResult = EvaluateModelDrivenTest(lEvalTestDefinition, lPtrEngine.get(), lRunContext);
    }

    lVecEvalTestResults.push_back(std::move(lEvalTestResult));
  }

  PrintRequiredOutput(lVecEvalTestResults);
  WriteResultsJson(lVecEvalTestResults, lRunnerPaths.pPathResults / "latest_results.json", lRunContext);

  return 0;
}
