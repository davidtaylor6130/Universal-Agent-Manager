#include "vectorised_rag.h"

#include "../embedding_utils.h"
#include "../determanistic_hash/determanistic_hash.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sqlite3.h>
#include <tree_sitter/api.h>

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
#include "llama.h"
#endif

extern "C" const TSLanguage* tree_sitter_cpp(void);

namespace ollama_engine::internal::vectorised_rag {
namespace {
namespace fs = std::filesystem;

constexpr std::size_t kiMaxSourceFileBytes = 2 * 1024 * 1024;
constexpr int kiMaxSummaryLines = 120;
constexpr int kiSplitSymbolLines = 120;
constexpr std::size_t kiDefaultChunkCharLimit = 4000;

struct SourceSpec {
  std::string pSInput;
  std::string pSSourceId;
  fs::path pPathRoot;
};

struct ChunkRecord {
  std::string pSId;
  std::string pSSourceId;
  std::string pSFilePath;
  std::string pSChunkType;
  std::string pSSymbolName;
  std::string pSParentSymbol;
  int piStartLine = 1;
  int piEndLine = 1;
  std::string pSHash;
  std::string pSRawText;
  std::vector<float> pVecfEmbedding;
};

struct QueryCaptureMatch {
  TSNode pNodeChunk;
  TSNode pNodeName;
  bool pbHasName = false;
};

std::string ToLowerAscii(std::string pSValue) {
  std::transform(pSValue.begin(), pSValue.end(), pSValue.begin(),
                 [](const unsigned char pCChar) { return static_cast<char>(std::tolower(pCChar)); });
  return pSValue;
}

bool StartsWith(const std::string& pSValue, const std::string& pSPrefix) {
  return pSValue.size() >= pSPrefix.size() && pSValue.compare(0, pSPrefix.size(), pSPrefix) == 0;
}

bool EndsWithIgnoreCase(const std::string& pSValue, const std::string& pSSuffix) {
  if (pSValue.size() < pSSuffix.size()) {
    return false;
  }
  const std::size_t liOffset = pSValue.size() - pSSuffix.size();
  for (std::size_t liIndex = 0; liIndex < pSSuffix.size(); ++liIndex) {
    if (static_cast<char>(std::tolower(static_cast<unsigned char>(pSValue[liOffset + liIndex]))) !=
        static_cast<char>(std::tolower(static_cast<unsigned char>(pSSuffix[liIndex])))) {
      return false;
    }
  }
  return true;
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

bool IsAllowedDatabaseNameCharacter(const char pCChar) {
  const unsigned char lCUChar = static_cast<unsigned char>(pCChar);
  return std::isalnum(lCUChar) != 0 || pCChar == '_' || pCChar == '-' || pCChar == '.';
}

std::string SanitizeDatabaseName(const std::string& pSDatabaseName) {
  std::string lSDatabaseName = TrimAscii(pSDatabaseName);
  if (EndsWithIgnoreCase(lSDatabaseName, ".sqlite3")) {
    lSDatabaseName.resize(lSDatabaseName.size() - std::string(".sqlite3").size());
  }
  std::string lSSanitized;
  lSSanitized.reserve(lSDatabaseName.size());
  for (const char lCChar : lSDatabaseName) {
    if (IsAllowedDatabaseNameCharacter(lCChar)) {
      lSSanitized.push_back(lCChar);
    } else {
      lSSanitized.push_back('_');
    }
  }
  while (!lSSanitized.empty() &&
         (lSSanitized.back() == '.' || lSSanitized.back() == '_' || lSSanitized.back() == '-')) {
    lSSanitized.pop_back();
  }
  return lSSanitized;
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

int RunShellCommand(const std::string& pSCommand, std::string* pSOutput = nullptr) {
  std::array<char, 4096> lArrCBuffer{};
  const std::string lSFullCommand = pSCommand + " 2>&1";
  FILE* lPtrPipe = popen(lSFullCommand.c_str(), "r");
  if (lPtrPipe == nullptr) {
    return -1;
  }
  std::string lSOutput;
  while (std::fgets(lArrCBuffer.data(), static_cast<int>(lArrCBuffer.size()), lPtrPipe) != nullptr) {
    lSOutput += lArrCBuffer.data();
  }
  const int liExitCode = pclose(lPtrPipe);
  if (pSOutput != nullptr) {
    *pSOutput = std::move(lSOutput);
  }
  return liExitCode;
}

bool LooksLikeRemoteRepo(const std::string& pSInput) {
  return StartsWith(pSInput, "http://") || StartsWith(pSInput, "https://") || StartsWith(pSInput, "git@") ||
         StartsWith(pSInput, "ssh://");
}

std::pair<std::string, std::string> SplitRemoteAndBranch(const std::string& pSInput) {
  const std::size_t liHashPosition = pSInput.find('#');
  if (liHashPosition == std::string::npos) {
    return {pSInput, ""};
  }
  return {pSInput.substr(0, liHashPosition), pSInput.substr(liHashPosition + 1)};
}

fs::path BuildRemoteClonePath(const std::string& pSRemoteUrl, const std::string& pSBranch) {
  const std::string lSKey = pSRemoteUrl + "#" + pSBranch;
  return fs::temp_directory_path() / "uam_ollama_engine_vectorised_rag" / determanistic_hash::HashTextHex(lSKey);
}

bool EnsureRemoteClone(const std::string& pSRemoteUrl,
                       const std::string& pSBranch,
                       const fs::path& pPathClone,
                       std::string* pSErrorOut) {
  std::error_code lErrorCode;
  fs::create_directories(pPathClone.parent_path(), lErrorCode);
  const fs::path lPathGitDirectory = pPathClone / ".git";

  if (!fs::exists(lPathGitDirectory, lErrorCode)) {
    const std::string lSBranchArg = pSBranch.empty() ? "" : (" --branch " + ShellQuote(pSBranch));
    const std::string lSCloneCommand = "git clone --depth 1" + lSBranchArg + " " + ShellQuote(pSRemoteUrl) + " " +
                                       ShellQuote(pPathClone.string());
    std::string lSCloneOutput;
    if (RunShellCommand(lSCloneCommand, &lSCloneOutput) != 0) {
      if (pSErrorOut != nullptr) {
        *pSErrorOut = "Failed to clone repository: " + TrimAscii(lSCloneOutput);
      }
      return false;
    }
    return true;
  }

  std::string lSFetchOutput;
  const std::string lSFetchCommand = "git -C " + ShellQuote(pPathClone.string()) + " fetch --all --prune";
  if (RunShellCommand(lSFetchCommand, &lSFetchOutput) != 0) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Failed to fetch remote updates: " + TrimAscii(lSFetchOutput);
    }
    return false;
  }

  if (!pSBranch.empty()) {
    std::string lSCheckoutOutput;
    const std::string lSCheckoutCommand =
        "git -C " + ShellQuote(pPathClone.string()) + " checkout " + ShellQuote(pSBranch);
    if (RunShellCommand(lSCheckoutCommand, &lSCheckoutOutput) != 0) {
      if (pSErrorOut != nullptr) {
        *pSErrorOut = "Failed to checkout branch: " + TrimAscii(lSCheckoutOutput);
      }
      return false;
    }

    std::string lSPullOutput;
    const std::string lSPullCommand = "git -C " + ShellQuote(pPathClone.string()) + " pull --ff-only origin " +
                                      ShellQuote(pSBranch);
    RunShellCommand(lSPullCommand, &lSPullOutput);
  }

  return true;
}

std::string NormalizePathKey(const fs::path& pPathValue) {
  std::error_code lErrorCode;
  const fs::path lPathAbsolute = fs::absolute(pPathValue, lErrorCode);
  const fs::path lPathNormalized =
      (lErrorCode ? pPathValue.lexically_normal() : lPathAbsolute.lexically_normal());
  return lPathNormalized.generic_string();
}

bool ResolveSourceSpec(Context& pContext,
                       const std::optional<std::string>& pOptSVectorFile,
                       SourceSpec* pPtrSourceSpecOut,
                       std::string* pSErrorOut) {
  if (pPtrSourceSpecOut == nullptr) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Source output pointer was null.";
    }
    return false;
  }

  std::string lSInput;
  {
    std::lock_guard<std::mutex> lGuard(pContext.pMutex);
    if (pOptSVectorFile.has_value() && !pOptSVectorFile->empty()) {
      lSInput = *pOptSVectorFile;
      pContext.pSLastVectorFileInput = lSInput;
    } else {
      lSInput = pContext.pSLastVectorFileInput;
    }
  }

  if (lSInput.empty()) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "No scan target is known. Provide vector_file at least once.";
    }
    return false;
  }

  if (LooksLikeRemoteRepo(lSInput)) {
    const auto [lSRemoteUrl, lSBranch] = SplitRemoteAndBranch(lSInput);
    if (lSRemoteUrl.empty()) {
      if (pSErrorOut != nullptr) {
        *pSErrorOut = "Remote repository URL was empty.";
      }
      return false;
    }
    const fs::path lPathClone = BuildRemoteClonePath(lSRemoteUrl, lSBranch);
    if (!EnsureRemoteClone(lSRemoteUrl, lSBranch, lPathClone, pSErrorOut)) {
      return false;
    }

    pPtrSourceSpecOut->pSInput = lSInput;
    pPtrSourceSpecOut->pSSourceId = lSRemoteUrl + (lSBranch.empty() ? "" : ("#" + lSBranch));
    pPtrSourceSpecOut->pPathRoot = lPathClone;
    return true;
  }

  std::error_code lErrorCode;
  fs::path lPathResolved = fs::absolute(fs::path(lSInput), lErrorCode);
  if (lErrorCode) {
    lPathResolved = fs::path(lSInput);
  }
  if (!fs::exists(lPathResolved, lErrorCode)) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Local path does not exist: " + lSInput;
    }
    return false;
  }

  if (fs::is_regular_file(lPathResolved, lErrorCode)) {
    lPathResolved = lPathResolved.parent_path();
  }
  if (!fs::is_directory(lPathResolved, lErrorCode)) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Scan target must be a directory or repository path.";
    }
    return false;
  }

  pPtrSourceSpecOut->pSInput = lSInput;
  pPtrSourceSpecOut->pSSourceId = NormalizePathKey(lPathResolved);
  pPtrSourceSpecOut->pPathRoot = lPathResolved;
  return true;
}

bool IsSupportedCppFile(const fs::path& pPathFile) {
  static const std::unordered_set<std::string> kSetSExtensions = {
      ".c++", ".cc", ".cpp", ".cxx", ".h", ".h++", ".hh", ".hpp", ".hxx", ".ipp", ".tcc", ".tpp"};
  return kSetSExtensions.find(ToLowerAscii(pPathFile.extension().string())) != kSetSExtensions.end();
}

bool IsSupportedDocumentationFile(const fs::path& pPathFile) {
  static const std::unordered_set<std::string> kSetSExtensions = {
      ".md", ".markdown", ".txt", ".rst", ".adoc", ".asciidoc"};
  return kSetSExtensions.find(ToLowerAscii(pPathFile.extension().string())) != kSetSExtensions.end();
}

bool IsIndexableSourceFile(const fs::path& pPathFile) {
  return IsSupportedCppFile(pPathFile) || IsSupportedDocumentationFile(pPathFile);
}

bool ShouldSkipDirectory(const fs::path& pPathDirectory) {
  const std::string lSName = pPathDirectory.filename().string();
  if (lSName.empty()) {
    return false;
  }
  if (lSName == ".git" || lSName == ".svn" || lSName == ".hg") {
    return true;
  }
  if (!lSName.empty() && lSName.front() == '.') {
    return true;
  }
  return false;
}

std::vector<fs::path> CollectIndexableFiles(const fs::path& pPathRoot) {
  std::vector<fs::path> lVecPathFiles;
  std::error_code lErrorCode;
  fs::recursive_directory_iterator lIterator(pPathRoot, fs::directory_options::skip_permission_denied, lErrorCode);
  const fs::recursive_directory_iterator lEndIterator;
  while (!lErrorCode && lIterator != lEndIterator) {
    const fs::directory_entry lEntry = *lIterator;
    lIterator.increment(lErrorCode);
    if (lErrorCode) {
      lErrorCode.clear();
      continue;
    }

    if (lEntry.is_directory(lErrorCode)) {
      if (!lErrorCode && ShouldSkipDirectory(lEntry.path())) {
        lIterator.disable_recursion_pending();
      }
      lErrorCode.clear();
      continue;
    }

    if (!lEntry.is_regular_file(lErrorCode) || lErrorCode) {
      lErrorCode.clear();
      continue;
    }
    if (!IsIndexableSourceFile(lEntry.path())) {
      continue;
    }
    lVecPathFiles.push_back(lEntry.path());
  }
  std::sort(lVecPathFiles.begin(), lVecPathFiles.end());
  return lVecPathFiles;
}

std::optional<std::string> ReadFileText(const fs::path& pPathFile) {
  std::error_code lErrorCode;
  const std::uintmax_t liFileSize = fs::file_size(pPathFile, lErrorCode);
  if (lErrorCode || liFileSize > kiMaxSourceFileBytes) {
    return std::nullopt;
  }
  std::ifstream lFileIn(pPathFile, std::ios::binary);
  if (!lFileIn.good()) {
    return std::nullopt;
  }
  std::ostringstream lBuffer;
  lBuffer << lFileIn.rdbuf();
  return lBuffer.str();
}

bool IsLikelyBinary(const std::string& pSContent) {
  const std::size_t liProbeSize = std::min<std::size_t>(pSContent.size(), 4096);
  for (std::size_t liIndex = 0; liIndex < liProbeSize; ++liIndex) {
    if (pSContent[liIndex] == '\0') {
      return true;
    }
  }
  return false;
}

std::string GetNodeText(const TSNode pNodeValue, const std::string& pSContent) {
  if (ts_node_is_null(pNodeValue)) {
    return {};
  }
  const std::uint32_t liStart = ts_node_start_byte(pNodeValue);
  const std::uint32_t liEnd = ts_node_end_byte(pNodeValue);
  if (liEnd <= liStart || liEnd > pSContent.size()) {
    return {};
  }
  return pSContent.substr(liStart, liEnd - liStart);
}

TSNode GetChildByFieldName(const TSNode pNodeValue, const char* pSFieldName) {
  if (ts_node_is_null(pNodeValue) || pSFieldName == nullptr) {
    return TSNode{};
  }
  return ts_node_child_by_field_name(pNodeValue, pSFieldName, std::strlen(pSFieldName));
}

std::string ExtractDeclaratorName(const TSNode pNodeValue, const std::string& pSContent) {
  if (ts_node_is_null(pNodeValue)) {
    return {};
  }

  const std::string_view lSType(ts_node_type(pNodeValue));
  if (lSType == "identifier" || lSType == "field_identifier" || lSType == "operator_name" ||
      lSType == "destructor_name" || lSType == "namespace_identifier" || lSType == "type_identifier") {
    return TrimAscii(GetNodeText(pNodeValue, pSContent));
  }

  const TSNode lNodeNamed = GetChildByFieldName(pNodeValue, "name");
  if (!ts_node_is_null(lNodeNamed)) {
    const std::string lSNamed = ExtractDeclaratorName(lNodeNamed, pSContent);
    if (!lSNamed.empty()) {
      return lSNamed;
    }
  }

  const TSNode lNodeDeclarator = GetChildByFieldName(pNodeValue, "declarator");
  if (!ts_node_is_null(lNodeDeclarator)) {
    const std::string lSDeclarator = ExtractDeclaratorName(lNodeDeclarator, pSContent);
    if (!lSDeclarator.empty()) {
      return lSDeclarator;
    }
  }

  const std::uint32_t liNamedChildren = ts_node_named_child_count(pNodeValue);
  for (std::uint32_t liChildIndex = 0; liChildIndex < liNamedChildren; ++liChildIndex) {
    const TSNode lNodeChild = ts_node_named_child(pNodeValue, liChildIndex);
    const std::string lSChildName = ExtractDeclaratorName(lNodeChild, pSContent);
    if (!lSChildName.empty()) {
      return lSChildName;
    }
  }

  return {};
}

std::string ExtractFunctionName(const TSNode pNodeFunction, const std::string& pSContent) {
  const TSNode lNodeDeclarator = GetChildByFieldName(pNodeFunction, "declarator");
  if (ts_node_is_null(lNodeDeclarator)) {
    return {};
  }
  return ExtractDeclaratorName(lNodeDeclarator, pSContent);
}

std::string JoinWithScope(const std::vector<std::string>& pVecSParts) {
  if (pVecSParts.empty()) {
    return {};
  }
  std::ostringstream lStream;
  for (std::size_t liIndex = 0; liIndex < pVecSParts.size(); ++liIndex) {
    if (liIndex > 0) {
      lStream << "::";
    }
    lStream << pVecSParts[liIndex];
  }
  return lStream.str();
}

std::string BuildParentSymbol(const TSNode pNodeValue, const std::string& pSContent) {
  if (ts_node_is_null(pNodeValue)) {
    return {};
  }
  std::vector<std::string> lVecSAncestors;
  TSNode lNodeCurrent = ts_node_parent(pNodeValue);
  while (!ts_node_is_null(lNodeCurrent)) {
    const std::string_view lSType(ts_node_type(lNodeCurrent));
    if (lSType == "namespace_definition" || lSType == "class_specifier" || lSType == "struct_specifier") {
      const TSNode lNodeName = GetChildByFieldName(lNodeCurrent, "name");
      std::string lSName = TrimAscii(GetNodeText(lNodeName, pSContent));
      if (lSName.empty() && lSType == "namespace_definition") {
        lSName = "(anonymous_namespace)";
      }
      if (!lSName.empty()) {
        lVecSAncestors.push_back(lSName);
      }
    }
    lNodeCurrent = ts_node_parent(lNodeCurrent);
  }

  std::reverse(lVecSAncestors.begin(), lVecSAncestors.end());
  return JoinWithScope(lVecSAncestors);
}

bool IsInsideClassOrStruct(const TSNode pNodeValue) {
  if (ts_node_is_null(pNodeValue)) {
    return false;
  }
  TSNode lNodeCurrent = ts_node_parent(pNodeValue);
  while (!ts_node_is_null(lNodeCurrent)) {
    const std::string_view lSType(ts_node_type(lNodeCurrent));
    if (lSType == "class_specifier" || lSType == "struct_specifier") {
      return true;
    }
    lNodeCurrent = ts_node_parent(lNodeCurrent);
  }
  return false;
}

std::vector<QueryCaptureMatch> RunQuery(const TSNode pNodeRoot, const std::string& pSQuerySource) {
  std::vector<QueryCaptureMatch> lVecMatches;
  std::uint32_t liErrorOffset = 0;
  TSQueryError lQueryErrorType = TSQueryErrorNone;
  TSQuery* lPtrQuery =
      ts_query_new(tree_sitter_cpp(), pSQuerySource.c_str(), pSQuerySource.size(), &liErrorOffset, &lQueryErrorType);
  if (lPtrQuery == nullptr || lQueryErrorType != TSQueryErrorNone) {
    if (lPtrQuery != nullptr) {
      ts_query_delete(lPtrQuery);
    }
    return lVecMatches;
  }

  TSQueryCursor* lPtrCursor = ts_query_cursor_new();
  if (lPtrCursor == nullptr) {
    ts_query_delete(lPtrQuery);
    return lVecMatches;
  }
  ts_query_cursor_exec(lPtrCursor, lPtrQuery, pNodeRoot);

  TSQueryMatch lQueryMatch;
  while (ts_query_cursor_next_match(lPtrCursor, &lQueryMatch)) {
    QueryCaptureMatch lCaptureMatch{};
    lCaptureMatch.pNodeChunk = TSNode{};
    lCaptureMatch.pNodeName = TSNode{};
    lCaptureMatch.pbHasName = false;

    for (std::uint16_t liCaptureIndex = 0; liCaptureIndex < lQueryMatch.capture_count; ++liCaptureIndex) {
      const TSQueryCapture lCapture = lQueryMatch.captures[liCaptureIndex];
      std::uint32_t liNameLength = 0;
      const char* lPtrCaptureName = ts_query_capture_name_for_id(lPtrQuery, lCapture.index, &liNameLength);
      if (lPtrCaptureName == nullptr || liNameLength == 0) {
        continue;
      }
      const std::string_view lSCaptureName(lPtrCaptureName, liNameLength);
      if (lSCaptureName == "chunk") {
        lCaptureMatch.pNodeChunk = lCapture.node;
      } else if (lSCaptureName == "name") {
        lCaptureMatch.pNodeName = lCapture.node;
        lCaptureMatch.pbHasName = true;
      }
    }
    if (!ts_node_is_null(lCaptureMatch.pNodeChunk)) {
      lVecMatches.push_back(lCaptureMatch);
    }
  }

  ts_query_cursor_delete(lPtrCursor);
  ts_query_delete(lPtrQuery);
  return lVecMatches;
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

std::string JoinLines(const std::vector<std::string>& pVecSLines, const std::size_t piBegin, const std::size_t piEnd) {
  std::string lSOut;
  for (std::size_t liIndex = piBegin; liIndex < piEnd; ++liIndex) {
    if (!lSOut.empty()) {
      lSOut.push_back('\n');
    }
    lSOut += pVecSLines[liIndex];
  }
  return lSOut;
}

std::string BuildOverviewText(const std::string& pSRawText, const int piMaxLines, const std::size_t piMaxChars) {
  const std::vector<std::string> lVecSLines = SplitLines(pSRawText);
  const std::size_t liLineCap = std::min<std::size_t>(lVecSLines.size(), static_cast<std::size_t>(std::max(1, piMaxLines)));
  std::string lSResult = JoinLines(lVecSLines, 0, liLineCap);
  if (lSResult.size() > piMaxChars) {
    lSResult = lSResult.substr(0, piMaxChars);
  }
  return TrimAscii(lSResult);
}

void FinalizeChunkIdentity(ChunkRecord* pPtrChunk) {
  if (pPtrChunk == nullptr) {
    return;
  }
  const std::string lSHashSeed = pPtrChunk->pSSourceId + "|" + pPtrChunk->pSFilePath + "|" + pPtrChunk->pSChunkType +
                                 "|" + pPtrChunk->pSSymbolName + "|" + pPtrChunk->pSParentSymbol + "|" +
                                 std::to_string(pPtrChunk->piStartLine) + "|" + std::to_string(pPtrChunk->piEndLine) +
                                 "|" + pPtrChunk->pSRawText;
  pPtrChunk->pSHash = determanistic_hash::HashTextHex(pPtrChunk->pSRawText);
  pPtrChunk->pSId = determanistic_hash::HashTextHex(lSHashSeed);
}

std::vector<ChunkRecord> MaybeSplitLargeChunk(const ChunkRecord& pChunkRecord, const std::size_t piChunkCharLimit) {
  if (pChunkRecord.pSRawText.size() <= piChunkCharLimit) {
    return {pChunkRecord};
  }

  const std::vector<std::string> lVecSLines = SplitLines(pChunkRecord.pSRawText);
  if (lVecSLines.empty()) {
    return {pChunkRecord};
  }

  std::vector<ChunkRecord> lVecChunks;
  std::size_t liLineIndex = 0;
  int liCurrentLine = pChunkRecord.piStartLine;
  while (liLineIndex < lVecSLines.size()) {
    const std::size_t liSegmentStart = liLineIndex;
    int liSegmentStartLine = liCurrentLine;
    std::size_t liChars = 0;
    while (liLineIndex < lVecSLines.size() && (liLineIndex - liSegmentStart) < static_cast<std::size_t>(kiSplitSymbolLines)) {
      const std::size_t liNextChars = liChars + lVecSLines[liLineIndex].size() + 1;
      if (liLineIndex > liSegmentStart && liNextChars > piChunkCharLimit) {
        break;
      }
      liChars = liNextChars;
      ++liLineIndex;
      ++liCurrentLine;
    }

    if (liLineIndex == liSegmentStart) {
      ++liLineIndex;
      ++liCurrentLine;
    }

    ChunkRecord lSegmentChunk = pChunkRecord;
    lSegmentChunk.piStartLine = liSegmentStartLine;
    lSegmentChunk.piEndLine = std::max(liSegmentStartLine, liCurrentLine - 1);
    lSegmentChunk.pSRawText = JoinLines(lVecSLines, liSegmentStart, liLineIndex);
    lSegmentChunk.pSRawText = "// symbol: " + pChunkRecord.pSSymbolName + "\n" +
                              (pChunkRecord.pSParentSymbol.empty()
                                   ? std::string{}
                                   : ("// parent: " + pChunkRecord.pSParentSymbol + "\n")) +
                              lSegmentChunk.pSRawText;
    FinalizeChunkIdentity(&lSegmentChunk);
    lVecChunks.push_back(std::move(lSegmentChunk));
  }
  return lVecChunks;
}

std::vector<ChunkRecord> BuildFileSummaryFallback(const std::string& pSSourceId,
                                                  const std::string& pSRelativePath,
                                                  const std::string& pSContent,
                                                  const std::size_t piChunkCharLimit) {
  std::vector<ChunkRecord> lVecChunks;
  ChunkRecord lChunk;
  lChunk.pSSourceId = pSSourceId;
  lChunk.pSFilePath = pSRelativePath;
  lChunk.pSChunkType = "file_summary";
  lChunk.pSSymbolName = fs::path(pSRelativePath).filename().string();
  lChunk.piStartLine = 1;
  lChunk.pSRawText = BuildOverviewText(pSContent, kiMaxSummaryLines, std::max<std::size_t>(piChunkCharLimit, std::size_t{384}));
  if (lChunk.pSRawText.empty()) {
    return lVecChunks;
  }
  const std::size_t liLineSpan = std::max<std::size_t>(1, SplitLines(lChunk.pSRawText).size());
  lChunk.piEndLine = static_cast<int>(liLineSpan);
  FinalizeChunkIdentity(&lChunk);
  lVecChunks.push_back(std::move(lChunk));
  return lVecChunks;
}

bool IsRstHeadingUnderline(const std::string& pSLine) {
  const std::string lSTrimmed = TrimAscii(pSLine);
  if (lSTrimmed.size() < 3) {
    return false;
  }
  const char lCChar = lSTrimmed.front();
  if (lCChar != '=' && lCChar != '-' && lCChar != '~' && lCChar != '^' && lCChar != '*') {
    return false;
  }
  for (const char lCCheck : lSTrimmed) {
    if (lCCheck != lCChar) {
      return false;
    }
  }
  return true;
}

std::vector<ChunkRecord> ExtractChunksFromDocumentationFile(const std::string& pSSourceId,
                                                            const std::string& pSRelativePath,
                                                            const std::string& pSContent,
                                                            const std::size_t piChunkCharLimit) {
  const std::size_t liChunkLimit = std::max<std::size_t>(piChunkCharLimit, std::size_t{512});
  const std::vector<std::string> lVecSLines = SplitLines(pSContent);
  if (lVecSLines.empty()) {
    return BuildFileSummaryFallback(pSSourceId, pSRelativePath, pSContent, liChunkLimit);
  }

  std::vector<ChunkRecord> lVecChunks;
  std::string lSCurrentHeading = fs::path(pSRelativePath).filename().string();
  std::string lSCurrentChunkText;
  int liCurrentChunkStartLine = 1;

  auto FlushChunk = [&](int piEndLineInclusive) {
    const std::string lSTrimmed = TrimAscii(lSCurrentChunkText);
    if (lSTrimmed.empty()) {
      lSCurrentChunkText.clear();
      return;
    }
    ChunkRecord lChunk;
    lChunk.pSSourceId = pSSourceId;
    lChunk.pSFilePath = pSRelativePath;
    lChunk.pSChunkType = "doc_section";
    lChunk.pSSymbolName = lSCurrentHeading.empty() ? fs::path(pSRelativePath).filename().string() : lSCurrentHeading;
    lChunk.pSParentSymbol.clear();
    lChunk.piStartLine = liCurrentChunkStartLine;
    lChunk.piEndLine = std::max(liCurrentChunkStartLine, piEndLineInclusive);
    lChunk.pSRawText = lSTrimmed;
    FinalizeChunkIdentity(&lChunk);
    std::vector<ChunkRecord> lVecSplitChunks = MaybeSplitLargeChunk(lChunk, liChunkLimit);
    lVecChunks.insert(lVecChunks.end(),
                      std::make_move_iterator(lVecSplitChunks.begin()),
                      std::make_move_iterator(lVecSplitChunks.end()));
    lSCurrentChunkText.clear();
  };

  std::size_t liIndex = 0;
  while (liIndex < lVecSLines.size()) {
    const std::string& lSLine = lVecSLines[liIndex];
    const std::string lSTrimmed = TrimAscii(lSLine);

    bool lbMarkdownHeading = false;
    std::string lSHeadingText;
    if (!lSTrimmed.empty() && lSTrimmed.front() == '#') {
      lbMarkdownHeading = true;
      std::size_t liPos = 0;
      while (liPos < lSTrimmed.size() && lSTrimmed[liPos] == '#') {
        ++liPos;
      }
      lSHeadingText = TrimAscii(lSTrimmed.substr(liPos));
    }

    bool lbSetextHeading = false;
    if (!lbMarkdownHeading && !lSTrimmed.empty() && (liIndex + 1) < lVecSLines.size() &&
        IsRstHeadingUnderline(lVecSLines[liIndex + 1])) {
      lbSetextHeading = true;
      lSHeadingText = lSTrimmed;
    }

    if (lbMarkdownHeading || lbSetextHeading) {
      const int liHeadingLine = static_cast<int>(liIndex + 1);
      FlushChunk(liHeadingLine - 1);
      if (!lSHeadingText.empty()) {
        lSCurrentHeading = lSHeadingText;
      }
      liCurrentChunkStartLine = liHeadingLine;
      lSCurrentChunkText = lSLine;
      if (lbSetextHeading) {
        lSCurrentChunkText += "\n";
        lSCurrentChunkText += lVecSLines[liIndex + 1];
        liIndex += 2;
      } else {
        ++liIndex;
      }
      continue;
    }

    if (lSCurrentChunkText.empty()) {
      liCurrentChunkStartLine = static_cast<int>(liIndex + 1);
      lSCurrentChunkText = lSLine;
    } else {
      lSCurrentChunkText += "\n";
      lSCurrentChunkText += lSLine;
    }

    const bool lbParagraphBoundary = lSTrimmed.empty();
    const bool lbChunkLarge = lSCurrentChunkText.size() >= liChunkLimit;
    if (lbChunkLarge || (lbParagraphBoundary && lSCurrentChunkText.size() >= (liChunkLimit / 2))) {
      FlushChunk(static_cast<int>(liIndex + 1));
    }
    ++liIndex;
  }

  FlushChunk(static_cast<int>(lVecSLines.size()));
  if (lVecChunks.empty()) {
    return BuildFileSummaryFallback(pSSourceId, pSRelativePath, pSContent, liChunkLimit);
  }
  return lVecChunks;
}

std::vector<ChunkRecord> ExtractChunksFromCppFile(const std::string& pSSourceId,
                                                  const std::string& pSRelativePath,
                                                  const std::string& pSContent,
                                                  const std::size_t piChunkCharLimit) {
  std::vector<ChunkRecord> lVecAllChunks;

  TSParser* lPtrParser = ts_parser_new();
  if (lPtrParser == nullptr) {
    return BuildFileSummaryFallback(pSSourceId, pSRelativePath, pSContent, piChunkCharLimit);
  }
  if (!ts_parser_set_language(lPtrParser, tree_sitter_cpp())) {
    ts_parser_delete(lPtrParser);
    return BuildFileSummaryFallback(pSSourceId, pSRelativePath, pSContent, piChunkCharLimit);
  }

  TSTree* lPtrTree = ts_parser_parse_string(lPtrParser, nullptr, pSContent.c_str(), pSContent.size());
  if (lPtrTree == nullptr) {
    ts_parser_delete(lPtrParser);
    return BuildFileSummaryFallback(pSSourceId, pSRelativePath, pSContent, piChunkCharLimit);
  }
  const TSNode lNodeRoot = ts_tree_root_node(lPtrTree);

  std::vector<ChunkRecord> lVecFunctionChunks;
  std::vector<ChunkRecord> lVecTypeOverviewChunks;
  std::vector<ChunkRecord> lVecEnumNamespaceGlobalChunks;

  const std::vector<QueryCaptureMatch> lVecFunctionMatches = RunQuery(lNodeRoot, "(function_definition) @chunk");
  for (const QueryCaptureMatch& lMatch : lVecFunctionMatches) {
    ChunkRecord lChunk;
    lChunk.pSSourceId = pSSourceId;
    lChunk.pSFilePath = pSRelativePath;
    lChunk.pSChunkType = IsInsideClassOrStruct(lMatch.pNodeChunk) ? "method" : "function";
    lChunk.pSSymbolName = ExtractFunctionName(lMatch.pNodeChunk, pSContent);
    if (lChunk.pSSymbolName.empty()) {
      lChunk.pSSymbolName = "(anonymous_function)";
    }
    lChunk.pSParentSymbol = BuildParentSymbol(lMatch.pNodeChunk, pSContent);
    lChunk.piStartLine = static_cast<int>(ts_node_start_point(lMatch.pNodeChunk).row + 1);
    lChunk.piEndLine = static_cast<int>(ts_node_end_point(lMatch.pNodeChunk).row + 1);
    lChunk.pSRawText = TrimAscii(GetNodeText(lMatch.pNodeChunk, pSContent));
    if (lChunk.pSRawText.empty()) {
      continue;
    }
    FinalizeChunkIdentity(&lChunk);
    std::vector<ChunkRecord> lVecChunksForSymbol = MaybeSplitLargeChunk(lChunk, std::max(piChunkCharLimit, std::size_t{512}));
    lVecFunctionChunks.insert(lVecFunctionChunks.end(),
                              std::make_move_iterator(lVecChunksForSymbol.begin()),
                              std::make_move_iterator(lVecChunksForSymbol.end()));
  }

  const std::vector<QueryCaptureMatch> lVecClassStructMatches =
      RunQuery(lNodeRoot, "(class_specifier name: (type_identifier) @name) @chunk\n"
                         "(struct_specifier name: (type_identifier) @name) @chunk");
  for (const QueryCaptureMatch& lMatch : lVecClassStructMatches) {
    ChunkRecord lChunk;
    lChunk.pSSourceId = pSSourceId;
    lChunk.pSFilePath = pSRelativePath;
    lChunk.pSChunkType = (std::string_view(ts_node_type(lMatch.pNodeChunk)) == "struct_specifier") ? "struct_overview"
                                                                                                     : "class_overview";
    lChunk.pSSymbolName = lMatch.pbHasName ? TrimAscii(GetNodeText(lMatch.pNodeName, pSContent)) : "";
    if (lChunk.pSSymbolName.empty()) {
      lChunk.pSSymbolName = "(anonymous_type)";
    }
    lChunk.pSParentSymbol = BuildParentSymbol(lMatch.pNodeChunk, pSContent);
    lChunk.piStartLine = static_cast<int>(ts_node_start_point(lMatch.pNodeChunk).row + 1);
    lChunk.pSRawText = BuildOverviewText(GetNodeText(lMatch.pNodeChunk, pSContent), kiMaxSummaryLines,
                                         std::max<std::size_t>(piChunkCharLimit / 2, std::size_t{512}));
    if (lChunk.pSRawText.empty()) {
      continue;
    }
    const std::size_t liLineSpan = std::max<std::size_t>(1, SplitLines(lChunk.pSRawText).size());
    lChunk.piEndLine = lChunk.piStartLine + static_cast<int>(liLineSpan) - 1;
    FinalizeChunkIdentity(&lChunk);
    lVecTypeOverviewChunks.push_back(std::move(lChunk));
  }

  const std::vector<QueryCaptureMatch> lVecEnumMatches =
      RunQuery(lNodeRoot, "(enum_specifier name: (type_identifier) @name) @chunk");
  for (const QueryCaptureMatch& lMatch : lVecEnumMatches) {
    ChunkRecord lChunk;
    lChunk.pSSourceId = pSSourceId;
    lChunk.pSFilePath = pSRelativePath;
    lChunk.pSChunkType = "enum";
    lChunk.pSSymbolName = lMatch.pbHasName ? TrimAscii(GetNodeText(lMatch.pNodeName, pSContent)) : "(anonymous_enum)";
    lChunk.pSParentSymbol = BuildParentSymbol(lMatch.pNodeChunk, pSContent);
    lChunk.piStartLine = static_cast<int>(ts_node_start_point(lMatch.pNodeChunk).row + 1);
    lChunk.piEndLine = static_cast<int>(ts_node_end_point(lMatch.pNodeChunk).row + 1);
    lChunk.pSRawText = TrimAscii(GetNodeText(lMatch.pNodeChunk, pSContent));
    if (lChunk.pSRawText.empty()) {
      continue;
    }
    FinalizeChunkIdentity(&lChunk);
    std::vector<ChunkRecord> lVecChunksForSymbol = MaybeSplitLargeChunk(lChunk, std::max(piChunkCharLimit, std::size_t{512}));
    lVecEnumNamespaceGlobalChunks.insert(lVecEnumNamespaceGlobalChunks.end(),
                                         std::make_move_iterator(lVecChunksForSymbol.begin()),
                                         std::make_move_iterator(lVecChunksForSymbol.end()));
  }

  const std::vector<QueryCaptureMatch> lVecNamespaceMatches =
      RunQuery(lNodeRoot, "(namespace_definition name: (namespace_identifier) @name) @chunk");
  for (const QueryCaptureMatch& lMatch : lVecNamespaceMatches) {
    ChunkRecord lChunk;
    lChunk.pSSourceId = pSSourceId;
    lChunk.pSFilePath = pSRelativePath;
    lChunk.pSChunkType = "namespace";
    lChunk.pSSymbolName = lMatch.pbHasName ? TrimAscii(GetNodeText(lMatch.pNodeName, pSContent)) : "(anonymous_namespace)";
    if (lChunk.pSSymbolName.empty()) {
      lChunk.pSSymbolName = "(anonymous_namespace)";
    }
    lChunk.pSParentSymbol = BuildParentSymbol(lMatch.pNodeChunk, pSContent);
    lChunk.piStartLine = static_cast<int>(ts_node_start_point(lMatch.pNodeChunk).row + 1);
    lChunk.pSRawText = BuildOverviewText(GetNodeText(lMatch.pNodeChunk, pSContent), kiMaxSummaryLines,
                                         std::max<std::size_t>(piChunkCharLimit / 2, std::size_t{512}));
    if (lChunk.pSRawText.empty()) {
      continue;
    }
    const std::size_t liLineSpan = std::max<std::size_t>(1, SplitLines(lChunk.pSRawText).size());
    lChunk.piEndLine = lChunk.piStartLine + static_cast<int>(liLineSpan) - 1;
    FinalizeChunkIdentity(&lChunk);
    lVecEnumNamespaceGlobalChunks.push_back(std::move(lChunk));
  }

  const std::uint32_t liRootChildCount = ts_node_named_child_count(lNodeRoot);
  for (std::uint32_t liChildIndex = 0; liChildIndex < liRootChildCount; ++liChildIndex) {
    const TSNode lNodeChild = ts_node_named_child(lNodeRoot, liChildIndex);
    const std::string_view lSType(ts_node_type(lNodeChild));
    if (lSType == "function_definition" || lSType == "class_specifier" || lSType == "struct_specifier" ||
        lSType == "enum_specifier" || lSType == "namespace_definition") {
      continue;
    }
    ChunkRecord lChunk;
    lChunk.pSSourceId = pSSourceId;
    lChunk.pSFilePath = pSRelativePath;
    lChunk.pSChunkType = "global_block";
    lChunk.pSSymbolName = std::string(lSType);
    lChunk.pSParentSymbol.clear();
    lChunk.piStartLine = static_cast<int>(ts_node_start_point(lNodeChild).row + 1);
    lChunk.pSRawText = BuildOverviewText(GetNodeText(lNodeChild, pSContent), kiMaxSummaryLines,
                                         std::max<std::size_t>(piChunkCharLimit / 2, std::size_t{384}));
    if (lChunk.pSRawText.empty()) {
      continue;
    }
    const std::size_t liLineSpan = std::max<std::size_t>(1, SplitLines(lChunk.pSRawText).size());
    lChunk.piEndLine = lChunk.piStartLine + static_cast<int>(liLineSpan) - 1;
    FinalizeChunkIdentity(&lChunk);
    lVecEnumNamespaceGlobalChunks.push_back(std::move(lChunk));
  }

  lVecAllChunks.reserve(lVecFunctionChunks.size() + lVecTypeOverviewChunks.size() + lVecEnumNamespaceGlobalChunks.size() + 1);
  lVecAllChunks.insert(lVecAllChunks.end(),
                       std::make_move_iterator(lVecFunctionChunks.begin()),
                       std::make_move_iterator(lVecFunctionChunks.end()));
  lVecAllChunks.insert(lVecAllChunks.end(),
                       std::make_move_iterator(lVecTypeOverviewChunks.begin()),
                       std::make_move_iterator(lVecTypeOverviewChunks.end()));
  lVecAllChunks.insert(lVecAllChunks.end(),
                       std::make_move_iterator(lVecEnumNamespaceGlobalChunks.begin()),
                       std::make_move_iterator(lVecEnumNamespaceGlobalChunks.end()));

  if (lVecAllChunks.empty()) {
    lVecAllChunks = BuildFileSummaryFallback(pSSourceId, pSRelativePath, pSContent, piChunkCharLimit);
  }

  ts_tree_delete(lPtrTree);
  ts_parser_delete(lPtrParser);
  return lVecAllChunks;
}

std::string EscapeJson(const std::string& pSValue) {
  std::string lSOut;
  lSOut.reserve(pSValue.size() + 8);
  for (const unsigned char lCChar : pSValue) {
    switch (lCChar) {
      case '\"':
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
          const char lArrHex[] = "0123456789abcdef";
          lSOut += "\\u00";
          lSOut.push_back(lArrHex[(lCChar >> 4) & 0x0F]);
          lSOut.push_back(lArrHex[lCChar & 0x0F]);
        } else {
          lSOut.push_back(static_cast<char>(lCChar));
        }
        break;
    }
  }
  return lSOut;
}

std::optional<std::vector<float>> ParseEmbeddingFromJson(const std::string& pSJson) {
  const std::size_t liEmbeddingKey = pSJson.find("\"embedding\"");
  if (liEmbeddingKey == std::string::npos) {
    return std::nullopt;
  }

  const std::size_t liArrayStart = pSJson.find('[', liEmbeddingKey);
  if (liArrayStart == std::string::npos) {
    return std::nullopt;
  }
  std::size_t liArrayEnd = std::string::npos;
  int liDepth = 0;
  for (std::size_t liIndex = liArrayStart; liIndex < pSJson.size(); ++liIndex) {
    if (pSJson[liIndex] == '[') {
      ++liDepth;
    } else if (pSJson[liIndex] == ']') {
      --liDepth;
      if (liDepth == 0) {
        liArrayEnd = liIndex;
        break;
      }
    }
  }
  if (liArrayEnd == std::string::npos || liArrayEnd <= liArrayStart + 1) {
    return std::nullopt;
  }

  const std::string lSArray = pSJson.substr(liArrayStart + 1, liArrayEnd - liArrayStart - 1);
  std::vector<float> lVecfEmbedding;
  lVecfEmbedding.reserve(512);
  std::string lSToken;
  for (const char lCChar : lSArray) {
    const bool lbNumeric = (std::isdigit(static_cast<unsigned char>(lCChar)) != 0) || lCChar == '-' || lCChar == '+' ||
                           lCChar == '.' || lCChar == 'e' || lCChar == 'E';
    if (lbNumeric) {
      lSToken.push_back(lCChar);
      continue;
    }
    if (!lSToken.empty()) {
      lVecfEmbedding.push_back(static_cast<float>(std::strtod(lSToken.c_str(), nullptr)));
      lSToken.clear();
    }
  }
  if (!lSToken.empty()) {
    lVecfEmbedding.push_back(static_cast<float>(std::strtod(lSToken.c_str(), nullptr)));
  }

  if (lVecfEmbedding.empty()) {
    return std::nullopt;
  }
  return lVecfEmbedding;
}

std::vector<float> NormalizeEmbedding(std::vector<float> pVecfEmbedding) {
  double ldNorm = 0.0;
  for (const float lfValue : pVecfEmbedding) {
    ldNorm += static_cast<double>(lfValue) * static_cast<double>(lfValue);
  }
  ldNorm = std::sqrt(ldNorm);
  if (ldNorm > 0.0) {
    for (float& lfValue : pVecfEmbedding) {
      lfValue = static_cast<float>(static_cast<double>(lfValue) / ldNorm);
    }
  }
  return pVecfEmbedding;
}

bool IsServerEmbeddingsAvailable(Context& pContext, const std::string& pSServerUrl) {
  {
    std::lock_guard<std::mutex> lGuard(pContext.pMutex);
    if (pContext.pbLlamaServerChecked && pContext.pSCheckedLlamaServerUrl == pSServerUrl) {
      return pContext.pbLlamaServerAvailable;
    }
  }

  std::string lSOutput;
  const std::string lSHealthCommand =
      "curl -sS --fail --max-time 2 " + ShellQuote(pSServerUrl + "/health");
  const bool lbAvailable = (RunShellCommand(lSHealthCommand, &lSOutput) == 0);

  std::lock_guard<std::mutex> lGuard(pContext.pMutex);
  pContext.pbLlamaServerChecked = true;
  pContext.pSCheckedLlamaServerUrl = pSServerUrl;
  pContext.pbLlamaServerAvailable = lbAvailable;
  return lbAvailable;
}

std::optional<std::vector<float>> TryEmbedWithServer(Context& pContext,
                                                     const RuntimeOptions& pRuntimeOptions,
                                                     const fs::path& pPathModel,
                                                     const std::string& pSText) {
  const std::string lSServerUrl = pRuntimeOptions.pSLlamaServerUrl.empty() ? "http://127.0.0.1:8080"
                                                                            : pRuntimeOptions.pSLlamaServerUrl;
  if (!IsServerEmbeddingsAvailable(pContext, lSServerUrl)) {
    return std::nullopt;
  }

  const fs::path lPathPayloadFile =
      fs::temp_directory_path() / ("uam_rag_payload_" + determanistic_hash::HashTextHex(pSText) + ".json");
  {
    std::ofstream lPayloadOut(lPathPayloadFile, std::ios::binary | std::ios::trunc);
    if (!lPayloadOut.good()) {
      return std::nullopt;
    }
    lPayloadOut << "{";
    if (!pPathModel.empty()) {
      lPayloadOut << "\"model\":\"" << EscapeJson(pPathModel.string()) << "\",";
    }
    lPayloadOut << "\"input\":\"" << EscapeJson(pSText) << "\"}";
  }

  const std::array<std::string, 2> kArrSEndpoints = {"/v1/embeddings", "/embeddings"};
  std::optional<std::vector<float>> lOptVecfEmbedding;
  for (const std::string& lSEndpoint : kArrSEndpoints) {
    std::string lSCommandOutput;
    const std::string lSCommand = "curl -sS --fail --max-time 30 -H 'Content-Type: application/json' --data-binary @" +
                                  ShellQuote(lPathPayloadFile.string()) + " " + ShellQuote(lSServerUrl + lSEndpoint);
    if (RunShellCommand(lSCommand, &lSCommandOutput) != 0) {
      continue;
    }
    lOptVecfEmbedding = ParseEmbeddingFromJson(lSCommandOutput);
    if (lOptVecfEmbedding.has_value()) {
      break;
    }
  }

  std::error_code lErrorCode;
  fs::remove(lPathPayloadFile, lErrorCode);
  if (!lOptVecfEmbedding.has_value()) {
    return std::nullopt;
  }
  return NormalizeEmbedding(*lOptVecfEmbedding);
}

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
std::once_flag g_OnceLlamaBackendInit;

void EnsureLlamaBackendInit() {
  std::call_once(g_OnceLlamaBackendInit, []() {
    llama_backend_init();
    ggml_backend_load_all();
  });
}

void ReleaseEmbeddingRuntimeLocked(Context& pContext) {
  if (pContext.pPtrEmbeddingContext != nullptr) {
    llama_free(pContext.pPtrEmbeddingContext);
    pContext.pPtrEmbeddingContext = nullptr;
  }
  if (pContext.pPtrEmbeddingModel != nullptr) {
    llama_model_free(pContext.pPtrEmbeddingModel);
    pContext.pPtrEmbeddingModel = nullptr;
  }
  pContext.pPathLoadedEmbeddingModel.clear();
  pContext.piLoadedEmbeddingMaxTokens = 0;
}

std::vector<fs::path> DiscoverModelFiles(const fs::path& pPathModelFolder) {
  std::vector<fs::path> lVecPathModels;
  std::error_code lErrorCode;
  fs::recursive_directory_iterator lIterator(pPathModelFolder, fs::directory_options::skip_permission_denied, lErrorCode);
  const fs::recursive_directory_iterator lEndIterator;
  while (!lErrorCode && lIterator != lEndIterator) {
    const fs::directory_entry lEntry = *lIterator;
    lIterator.increment(lErrorCode);
    if (lErrorCode) {
      lErrorCode.clear();
      continue;
    }
    if (lEntry.is_directory(lErrorCode)) {
      if (!lErrorCode && ShouldSkipDirectory(lEntry.path())) {
        lIterator.disable_recursion_pending();
      }
      lErrorCode.clear();
      continue;
    }
    if (!lEntry.is_regular_file(lErrorCode) || lErrorCode) {
      lErrorCode.clear();
      continue;
    }
    if (ToLowerAscii(lEntry.path().extension().string()) != ".gguf") {
      continue;
    }
    lVecPathModels.push_back(lEntry.path());
  }
  std::sort(lVecPathModels.begin(), lVecPathModels.end());
  return lVecPathModels;
}

fs::path ResolveEmbeddingModelPath(const RuntimeOptions& pRuntimeOptions) {
  std::error_code lErrorCode;
  if (!pRuntimeOptions.pPathEmbeddingModelFile.empty() &&
      fs::exists(pRuntimeOptions.pPathEmbeddingModelFile, lErrorCode) &&
      fs::is_regular_file(pRuntimeOptions.pPathEmbeddingModelFile, lErrorCode)) {
    return pRuntimeOptions.pPathEmbeddingModelFile;
  }
  const char* lPtrEnvModel = std::getenv("UAM_EMBEDDING_MODEL_PATH");
  if (lPtrEnvModel != nullptr && *lPtrEnvModel != '\0') {
    fs::path lPathModel = lPtrEnvModel;
    if (fs::exists(lPathModel, lErrorCode) && fs::is_regular_file(lPathModel, lErrorCode)) {
      return lPathModel;
    }
  }
  const std::vector<fs::path> lVecPathModels = DiscoverModelFiles(pRuntimeOptions.pPathModelFolder);
  for (const fs::path& lPathModel : lVecPathModels) {
    const std::string lSNameLower = ToLowerAscii(lPathModel.filename().string());
    if (lSNameLower.find("embed") != std::string::npos) {
      return lPathModel;
    }
  }
  return lVecPathModels.empty() ? fs::path{} : lVecPathModels.front();
}

bool EnsureDirectEmbeddingRuntime(Context& pContext,
                                  const fs::path& pPathModel,
                                  const RuntimeOptions& pRuntimeOptions,
                                  std::string* pSErrorOut) {
  if (pPathModel.empty()) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "No embedding model could be resolved.";
    }
    return false;
  }

  const std::size_t liEmbeddingMaxTokens = pRuntimeOptions.piEmbeddingMaxTokens > 0
                                               ? std::clamp<std::size_t>(pRuntimeOptions.piEmbeddingMaxTokens, 1, 32768)
                                               : 4096;

  EnsureLlamaBackendInit();

  if (pContext.pPtrEmbeddingModel != nullptr && pContext.pPtrEmbeddingContext != nullptr &&
      pContext.pPathLoadedEmbeddingModel == pPathModel &&
      pContext.piLoadedEmbeddingMaxTokens == liEmbeddingMaxTokens) {
    return true;
  }
  ReleaseEmbeddingRuntimeLocked(pContext);

  llama_model_params lModelParams = llama_model_default_params();
  lModelParams.n_gpu_layers = 99;
  llama_model* lPtrModel = llama_model_load_from_file(pPathModel.string().c_str(), lModelParams);
  if (lPtrModel == nullptr) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Failed to load embedding GGUF model: " + pPathModel.string();
    }
    return false;
  }

  llama_context_params lContextParams = llama_context_default_params();
  const auto liContextTokens = static_cast<decltype(lContextParams.n_ctx)>(liEmbeddingMaxTokens);
  lContextParams.n_ctx = liContextTokens;
  lContextParams.n_batch = liContextTokens;
  lContextParams.n_ubatch = liContextTokens;
  lContextParams.embeddings = true;
  lContextParams.pooling_type = LLAMA_POOLING_TYPE_MEAN;
  llama_context* lPtrContext = llama_init_from_model(lPtrModel, lContextParams);
  if (lPtrContext == nullptr) {
    llama_model_free(lPtrModel);
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Failed to initialize llama.cpp embedding context.";
    }
    return false;
  }

  pContext.pPtrEmbeddingModel = lPtrModel;
  pContext.pPtrEmbeddingContext = lPtrContext;
  pContext.pPathLoadedEmbeddingModel = pPathModel;
  pContext.piLoadedEmbeddingMaxTokens = liEmbeddingMaxTokens;
  return true;
}

std::optional<std::vector<float>> BuildEmbeddingDirect(Context& pContext,
                                                       const RuntimeOptions& pRuntimeOptions,
                                                       const std::string& pSText,
                                                       std::string* pSErrorOut) {
  const fs::path lPathEmbeddingModel = ResolveEmbeddingModelPath(pRuntimeOptions);
  std::lock_guard<std::mutex> lGuard(pContext.pMutex);
  if (!EnsureDirectEmbeddingRuntime(pContext, lPathEmbeddingModel, pRuntimeOptions, pSErrorOut)) {
    return std::nullopt;
  }

  const llama_vocab* lPtrVocab = llama_model_get_vocab(pContext.pPtrEmbeddingModel);
  if (lPtrVocab == nullptr) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Embedding model has no vocabulary.";
    }
    return std::nullopt;
  }

  const int32_t liTokenCountWanted =
      -llama_tokenize(lPtrVocab, pSText.c_str(), static_cast<int32_t>(pSText.size()), nullptr, 0, true, true);
  if (liTokenCountWanted <= 0) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Failed to tokenize text for embeddings.";
    }
    return std::nullopt;
  }

  std::vector<llama_token> lVeciTokens(static_cast<std::size_t>(liTokenCountWanted));
  const int32_t liTokenCountWritten =
      llama_tokenize(lPtrVocab, pSText.c_str(), static_cast<int32_t>(pSText.size()), lVeciTokens.data(),
                     static_cast<int32_t>(lVeciTokens.size()), true, true);
  if (liTokenCountWritten <= 0) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Failed to write tokenized text for embeddings.";
    }
    return std::nullopt;
  }
  lVeciTokens.resize(static_cast<std::size_t>(liTokenCountWritten));
  if (pContext.piLoadedEmbeddingMaxTokens > 0 && lVeciTokens.size() > pContext.piLoadedEmbeddingMaxTokens) {
    lVeciTokens.resize(pContext.piLoadedEmbeddingMaxTokens);
  }
  if (lVeciTokens.empty()) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "No tokens available after applying embedding token limit.";
    }
    return std::nullopt;
  }

  llama_memory_clear(llama_get_memory(pContext.pPtrEmbeddingContext), true);

  llama_batch lBatch = llama_batch_init(static_cast<int32_t>(lVeciTokens.size()), 0, 1);
  for (std::size_t liIndex = 0; liIndex < lVeciTokens.size(); ++liIndex) {
    lBatch.token[liIndex] = lVeciTokens[liIndex];
    lBatch.pos[liIndex] = static_cast<llama_pos>(liIndex);
    lBatch.n_seq_id[liIndex] = 1;
    lBatch.seq_id[liIndex][0] = 0;
    lBatch.logits[liIndex] = 1;
  }
  lBatch.n_tokens = static_cast<int32_t>(lVeciTokens.size());

  int32_t liStatus = 0;
  if (llama_model_has_encoder(pContext.pPtrEmbeddingModel) && !llama_model_has_decoder(pContext.pPtrEmbeddingModel)) {
    liStatus = llama_encode(pContext.pPtrEmbeddingContext, lBatch);
  } else {
    liStatus = llama_decode(pContext.pPtrEmbeddingContext, lBatch);
  }

  if (liStatus != 0) {
    llama_batch_free(lBatch);
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "llama.cpp failed while creating embeddings.";
    }
    return std::nullopt;
  }

  const int32_t liEmbeddingDimensionsRaw = std::max<int32_t>(
      llama_model_n_embd_out(pContext.pPtrEmbeddingModel), llama_model_n_embd(pContext.pPtrEmbeddingModel));
  if (liEmbeddingDimensionsRaw <= 0) {
    llama_batch_free(lBatch);
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Embedding model reported invalid dimensions.";
    }
    return std::nullopt;
  }

  const float* lPtrfEmbedding = nullptr;
  if (llama_pooling_type(pContext.pPtrEmbeddingContext) != LLAMA_POOLING_TYPE_NONE) {
    lPtrfEmbedding = llama_get_embeddings_seq(pContext.pPtrEmbeddingContext, 0);
  }
  if (lPtrfEmbedding == nullptr) {
    lPtrfEmbedding = llama_get_embeddings_ith(pContext.pPtrEmbeddingContext, -1);
  }
  if (lPtrfEmbedding == nullptr) {
    llama_batch_free(lBatch);
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "llama.cpp returned null embedding tensor.";
    }
    return std::nullopt;
  }

  std::vector<float> lVecfEmbedding(static_cast<std::size_t>(liEmbeddingDimensionsRaw));
  std::copy(lPtrfEmbedding, lPtrfEmbedding + liEmbeddingDimensionsRaw, lVecfEmbedding.begin());
  llama_batch_free(lBatch);
  return NormalizeEmbedding(std::move(lVecfEmbedding));
}
#endif

std::optional<std::vector<float>> BuildEmbedding(Context& pContext,
                                                 const RuntimeOptions& pRuntimeOptions,
                                                 const std::string& pSText,
                                                 std::string* pSErrorOut) {
  if (pRuntimeOptions.pbUseDeterministicEmbeddings) {
    const std::size_t liDimensions =
        std::clamp<std::size_t>(pRuntimeOptions.piDeterministicEmbeddingDimensions, 32, 4096);
    return NormalizeEmbedding(ollama_engine::internal::BuildEmbedding(pSText, liDimensions));
  }
#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
  const fs::path lPathModelForServer = ResolveEmbeddingModelPath(pRuntimeOptions);
  const std::optional<std::vector<float>> lOptServerEmbedding =
      TryEmbedWithServer(pContext, pRuntimeOptions, lPathModelForServer, pSText);
  if (lOptServerEmbedding.has_value()) {
    return lOptServerEmbedding;
  }
  return BuildEmbeddingDirect(pContext, pRuntimeOptions, pSText, pSErrorOut);
#else
  (void)pContext;
  (void)pRuntimeOptions;
  (void)pSText;
  if (pSErrorOut != nullptr) {
    *pSErrorOut = "llama.cpp is not linked, embeddings are unavailable.";
  }
  return std::nullopt;
#endif
}

bool ExecSql(sqlite3* pPtrDatabase, const char* pSSql, std::string* pSErrorOut) {
  char* lPtrSError = nullptr;
  const int liStatus = sqlite3_exec(pPtrDatabase, pSSql, nullptr, nullptr, &lPtrSError);
  if (liStatus == SQLITE_OK) {
    return true;
  }
  if (pSErrorOut != nullptr) {
    *pSErrorOut = (lPtrSError != nullptr) ? lPtrSError : "sqlite error";
  }
  sqlite3_free(lPtrSError);
  return false;
}

fs::path BuildStorageDirectory(const RuntimeOptions& pRuntimeOptions) {
  const std::string lSFolderName =
      pRuntimeOptions.pSStorageFolderName.empty() ? ".vectorised_rag" : pRuntimeOptions.pSStorageFolderName;
  const fs::path lPathDatabaseDirectory = fs::current_path() / lSFolderName;
  std::error_code lErrorCode;
  fs::create_directories(lPathDatabaseDirectory, lErrorCode);
  return lPathDatabaseDirectory;
}

fs::path BuildNamedDatabasePath(const RuntimeOptions& pRuntimeOptions, const std::string& pSDatabaseName) {
  const std::string lSDatabaseName = SanitizeDatabaseName(pSDatabaseName);
  if (lSDatabaseName.empty()) {
    return {};
  }
  return BuildStorageDirectory(pRuntimeOptions) / (lSDatabaseName + ".sqlite3");
}

fs::path BuildDatabasePath(const std::string& pSSourceId, const RuntimeOptions& pRuntimeOptions) {
  const fs::path lPathNamedDatabase = BuildNamedDatabasePath(pRuntimeOptions, pRuntimeOptions.pSDatabaseName);
  if (!lPathNamedDatabase.empty()) {
    return lPathNamedDatabase;
  }
  const fs::path lPathDatabaseDirectory = BuildStorageDirectory(pRuntimeOptions);
  return lPathDatabaseDirectory / (determanistic_hash::HashTextHex(pSSourceId) + ".sqlite3");
}

bool HasPathSeparator(const std::string& pSValue) {
  return pSValue.find('/') != std::string::npos || pSValue.find('\\') != std::string::npos;
}

std::vector<fs::path> CollectSqliteFilesInDirectory(const fs::path& pPathDirectory) {
  std::vector<fs::path> lVecPathDatabases;
  std::error_code lErrorCode;
  if (!fs::exists(pPathDirectory, lErrorCode) || !fs::is_directory(pPathDirectory, lErrorCode)) {
    return lVecPathDatabases;
  }

  for (const fs::directory_entry& lEntry : fs::directory_iterator(pPathDirectory, lErrorCode)) {
    if (lErrorCode) {
      lErrorCode.clear();
      continue;
    }
    if (!lEntry.is_regular_file(lErrorCode) || lErrorCode) {
      lErrorCode.clear();
      continue;
    }
    if (ToLowerAscii(lEntry.path().extension().string()) != ".sqlite3") {
      continue;
    }
    lVecPathDatabases.push_back(lEntry.path());
  }
  std::sort(lVecPathDatabases.begin(), lVecPathDatabases.end());
  return lVecPathDatabases;
}

bool ResolveDatabaseInput(const std::string& pSDatabaseInput,
                          const RuntimeOptions& pRuntimeOptions,
                          std::vector<fs::path>* pPtrVecPathDatabasesOut,
                          std::string* pSErrorOut) {
  if (pPtrVecPathDatabasesOut == nullptr) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Database path output was null.";
    }
    return false;
  }

  const std::string lSInput = TrimAscii(pSDatabaseInput);
  if (lSInput.empty()) {
    return true;
  }

  std::error_code lErrorCode;
  const fs::path lPathInput = fs::path(lSInput);
  if (fs::exists(lPathInput, lErrorCode)) {
    if (fs::is_directory(lPathInput, lErrorCode)) {
      const std::vector<fs::path> lVecPathDirectoryDatabases = CollectSqliteFilesInDirectory(lPathInput);
      if (lVecPathDirectoryDatabases.empty()) {
        if (pSErrorOut != nullptr) {
          *pSErrorOut = "No .sqlite3 databases found in directory: " + lPathInput.string();
        }
        return false;
      }
      pPtrVecPathDatabasesOut->insert(pPtrVecPathDatabasesOut->end(), lVecPathDirectoryDatabases.begin(),
                                      lVecPathDirectoryDatabases.end());
      return true;
    }

    if (fs::is_regular_file(lPathInput, lErrorCode)) {
      if (ToLowerAscii(lPathInput.extension().string()) != ".sqlite3") {
        if (pSErrorOut != nullptr) {
          *pSErrorOut = "Expected .sqlite3 database file: " + lPathInput.string();
        }
        return false;
      }
      pPtrVecPathDatabasesOut->push_back(lPathInput);
      return true;
    }

    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Database input is not a regular file or directory: " + lPathInput.string();
    }
    return false;
  }

  if (HasPathSeparator(lSInput)) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Database path was not found: " + lSInput;
    }
    return false;
  }

  const fs::path lPathNamedDatabase = BuildNamedDatabasePath(pRuntimeOptions, lSInput);
  if (lPathNamedDatabase.empty()) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Invalid logical database name: " + lSInput;
    }
    return false;
  }
  if (!fs::exists(lPathNamedDatabase, lErrorCode) || !fs::is_regular_file(lPathNamedDatabase, lErrorCode)) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Named database was not found: " + lPathNamedDatabase.string();
    }
    return false;
  }

  pPtrVecPathDatabasesOut->push_back(lPathNamedDatabase);
  return true;
}

bool HasChunksTable(sqlite3* pPtrDatabase) {
  static constexpr const char* kSql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name='chunks' LIMIT 1;";
  sqlite3_stmt* lPtrStatement = nullptr;
  if (sqlite3_prepare_v2(pPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK) {
    return false;
  }
  const bool lbHasTable = (sqlite3_step(lPtrStatement) == SQLITE_ROW);
  sqlite3_finalize(lPtrStatement);
  return lbHasTable;
}

bool OpenDatabaseForSource(const std::string& pSSourceId,
                           const RuntimeOptions& pRuntimeOptions,
                           sqlite3** pPtrDatabaseOut,
                           fs::path* pPathDatabaseOut) {
  if (pPtrDatabaseOut == nullptr || pPathDatabaseOut == nullptr) {
    return false;
  }
  *pPtrDatabaseOut = nullptr;
  *pPathDatabaseOut = BuildDatabasePath(pSSourceId, pRuntimeOptions);
  if (sqlite3_open((*pPathDatabaseOut).string().c_str(), pPtrDatabaseOut) != SQLITE_OK) {
    if (*pPtrDatabaseOut != nullptr) {
      sqlite3_close(*pPtrDatabaseOut);
      *pPtrDatabaseOut = nullptr;
    }
    return false;
  }
  sqlite3_busy_timeout(*pPtrDatabaseOut, 5000);
  return true;
}

bool EnsureSchema(sqlite3* pPtrDatabase, std::string* pSErrorOut) {
  static constexpr const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS files (
  source_id TEXT NOT NULL,
  file_path TEXT NOT NULL,
  content_hash TEXT NOT NULL,
  mtime_ticks INTEGER NOT NULL,
  file_size INTEGER NOT NULL,
  PRIMARY KEY (source_id, file_path)
);

CREATE TABLE IF NOT EXISTS chunks (
  id TEXT PRIMARY KEY,
  source_id TEXT NOT NULL,
  file_path TEXT NOT NULL,
  chunk_type TEXT NOT NULL,
  symbol_name TEXT,
  parent_symbol TEXT,
  start_line INTEGER NOT NULL,
  end_line INTEGER NOT NULL,
  hash TEXT NOT NULL,
  raw_text TEXT NOT NULL,
  embedding BLOB NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_chunks_source ON chunks(source_id);
CREATE INDEX IF NOT EXISTS idx_chunks_source_file ON chunks(source_id, file_path);
)SQL";
  return ExecSql(pPtrDatabase, kSchemaSql, pSErrorOut);
}

std::string SerializeEmbedding(const std::vector<float>& pVecfEmbedding) {
  if (pVecfEmbedding.empty()) {
    return {};
  }
  const char* lPtrBytes = reinterpret_cast<const char*>(pVecfEmbedding.data());
  return std::string(lPtrBytes, lPtrBytes + (pVecfEmbedding.size() * sizeof(float)));
}

std::vector<float> DeserializeEmbedding(const void* pPtrBlobData, const int piBlobBytes) {
  if (pPtrBlobData == nullptr || piBlobBytes <= 0 || (piBlobBytes % static_cast<int>(sizeof(float))) != 0) {
    return {};
  }
  const std::size_t liCount = static_cast<std::size_t>(piBlobBytes / static_cast<int>(sizeof(float)));
  std::vector<float> lVecfEmbedding(liCount, 0.0f);
  std::memcpy(lVecfEmbedding.data(), pPtrBlobData, liCount * sizeof(float));
  return lVecfEmbedding;
}

bool SelectFileHash(sqlite3* pPtrDatabase,
                    const std::string& pSSourceId,
                    const std::string& pSFilePath,
                    std::string* pPtrHashOut) {
  static constexpr const char* kSql = "SELECT content_hash FROM files WHERE source_id = ?1 AND file_path = ?2;";
  sqlite3_stmt* lPtrStatement = nullptr;
  if (sqlite3_prepare_v2(pPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(lPtrStatement, 1, pSSourceId.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(lPtrStatement, 2, pSFilePath.c_str(), -1, SQLITE_TRANSIENT);
  bool lbFound = false;
  if (sqlite3_step(lPtrStatement) == SQLITE_ROW) {
    const unsigned char* lPtrHash = sqlite3_column_text(lPtrStatement, 0);
    if (lPtrHash != nullptr && pPtrHashOut != nullptr) {
      *pPtrHashOut = reinterpret_cast<const char*>(lPtrHash);
      lbFound = true;
    }
  }
  sqlite3_finalize(lPtrStatement);
  return lbFound;
}

bool UpsertFileRow(sqlite3* pPtrDatabase,
                   const std::string& pSSourceId,
                   const std::string& pSFilePath,
                   const std::string& pSContentHash,
                   const std::uint64_t piMtimeTicks,
                   const std::uintmax_t piFileSize) {
  static constexpr const char* kSql =
      "INSERT INTO files(source_id, file_path, content_hash, mtime_ticks, file_size) "
      "VALUES(?1, ?2, ?3, ?4, ?5) "
      "ON CONFLICT(source_id, file_path) DO UPDATE SET "
      "content_hash = excluded.content_hash, "
      "mtime_ticks = excluded.mtime_ticks, "
      "file_size = excluded.file_size;";
  sqlite3_stmt* lPtrStatement = nullptr;
  if (sqlite3_prepare_v2(pPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(lPtrStatement, 1, pSSourceId.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(lPtrStatement, 2, pSFilePath.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(lPtrStatement, 3, pSContentHash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(lPtrStatement, 4, static_cast<sqlite3_int64>(piMtimeTicks));
  sqlite3_bind_int64(lPtrStatement, 5, static_cast<sqlite3_int64>(piFileSize));
  const bool lbOk = (sqlite3_step(lPtrStatement) == SQLITE_DONE);
  sqlite3_finalize(lPtrStatement);
  return lbOk;
}

bool DeleteChunksForFile(sqlite3* pPtrDatabase, const std::string& pSSourceId, const std::string& pSFilePath) {
  static constexpr const char* kSql = "DELETE FROM chunks WHERE source_id = ?1 AND file_path = ?2;";
  sqlite3_stmt* lPtrStatement = nullptr;
  if (sqlite3_prepare_v2(pPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(lPtrStatement, 1, pSSourceId.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(lPtrStatement, 2, pSFilePath.c_str(), -1, SQLITE_TRANSIENT);
  const bool lbOk = (sqlite3_step(lPtrStatement) == SQLITE_DONE);
  sqlite3_finalize(lPtrStatement);
  return lbOk;
}

bool InsertChunk(sqlite3* pPtrDatabase, const ChunkRecord& pChunkRecord) {
  static constexpr const char* kSql =
      "INSERT OR REPLACE INTO chunks("
      "id, source_id, file_path, chunk_type, symbol_name, parent_symbol, "
      "start_line, end_line, hash, raw_text, embedding) "
      "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11);";
  sqlite3_stmt* lPtrStatement = nullptr;
  if (sqlite3_prepare_v2(pPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK) {
    return false;
  }

  const std::string lSEmbeddingBlob = SerializeEmbedding(pChunkRecord.pVecfEmbedding);
  sqlite3_bind_text(lPtrStatement, 1, pChunkRecord.pSId.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(lPtrStatement, 2, pChunkRecord.pSSourceId.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(lPtrStatement, 3, pChunkRecord.pSFilePath.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(lPtrStatement, 4, pChunkRecord.pSChunkType.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(lPtrStatement, 5, pChunkRecord.pSSymbolName.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(lPtrStatement, 6, pChunkRecord.pSParentSymbol.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(lPtrStatement, 7, pChunkRecord.piStartLine);
  sqlite3_bind_int(lPtrStatement, 8, pChunkRecord.piEndLine);
  sqlite3_bind_text(lPtrStatement, 9, pChunkRecord.pSHash.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(lPtrStatement, 10, pChunkRecord.pSRawText.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(lPtrStatement, 11, lSEmbeddingBlob.data(), static_cast<int>(lSEmbeddingBlob.size()), SQLITE_TRANSIENT);
  const bool lbOk = (sqlite3_step(lPtrStatement) == SQLITE_DONE);
  sqlite3_finalize(lPtrStatement);
  return lbOk;
}

std::vector<std::string> SelectAllIndexedFiles(sqlite3* pPtrDatabase, const std::string& pSSourceId) {
  std::vector<std::string> lVecSFiles;
  static constexpr const char* kSql = "SELECT file_path FROM files WHERE source_id = ?1;";
  sqlite3_stmt* lPtrStatement = nullptr;
  if (sqlite3_prepare_v2(pPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK) {
    return lVecSFiles;
  }
  sqlite3_bind_text(lPtrStatement, 1, pSSourceId.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(lPtrStatement) == SQLITE_ROW) {
    const unsigned char* lPtrFilePath = sqlite3_column_text(lPtrStatement, 0);
    if (lPtrFilePath != nullptr) {
      lVecSFiles.emplace_back(reinterpret_cast<const char*>(lPtrFilePath));
    }
  }
  sqlite3_finalize(lPtrStatement);
  return lVecSFiles;
}

void DeleteFileAndChunks(sqlite3* pPtrDatabase, const std::string& pSSourceId, const std::string& pSFilePath) {
  DeleteChunksForFile(pPtrDatabase, pSSourceId, pSFilePath);
  static constexpr const char* kSql = "DELETE FROM files WHERE source_id = ?1 AND file_path = ?2;";
  sqlite3_stmt* lPtrStatement = nullptr;
  if (sqlite3_prepare_v2(pPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK) {
    return;
  }
  sqlite3_bind_text(lPtrStatement, 1, pSSourceId.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(lPtrStatement, 2, pSFilePath.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(lPtrStatement);
  sqlite3_finalize(lPtrStatement);
}

std::size_t CountVectorRows(sqlite3* pPtrDatabase, const std::string& pSSourceId) {
  static constexpr const char* kSql = "SELECT COUNT(*) FROM chunks WHERE source_id = ?1;";
  sqlite3_stmt* lPtrStatement = nullptr;
  if (sqlite3_prepare_v2(pPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK) {
    return 0;
  }
  sqlite3_bind_text(lPtrStatement, 1, pSSourceId.c_str(), -1, SQLITE_TRANSIENT);
  std::size_t liCount = 0;
  if (sqlite3_step(lPtrStatement) == SQLITE_ROW) {
    liCount = static_cast<std::size_t>(sqlite3_column_int64(lPtrStatement, 0));
  }
  sqlite3_finalize(lPtrStatement);
  return liCount;
}

double CosineSimilarity(const std::vector<float>& pVecfLhs, const std::vector<float>& pVecfRhs) {
  if (pVecfLhs.empty() || pVecfRhs.empty() || pVecfLhs.size() != pVecfRhs.size()) {
    return 0.0;
  }
  double ldDot = 0.0;
  double ldNormLhs = 0.0;
  double ldNormRhs = 0.0;
  for (std::size_t liIndex = 0; liIndex < pVecfLhs.size(); ++liIndex) {
    const double ldLhs = pVecfLhs[liIndex];
    const double ldRhs = pVecfRhs[liIndex];
    ldDot += (ldLhs * ldRhs);
    ldNormLhs += (ldLhs * ldLhs);
    ldNormRhs += (ldRhs * ldRhs);
  }
  if (ldNormLhs <= 0.0 || ldNormRhs <= 0.0) {
    return 0.0;
  }
  return ldDot / (std::sqrt(ldNormLhs) * std::sqrt(ldNormRhs));
}

void UpdateRunningState(Context& pContext,
                        const std::size_t piFilesProcessed,
                        const std::size_t piTotalFiles,
                        const std::size_t piVectorDatabaseSize) {
  std::lock_guard<std::mutex> lGuard(pContext.pMutex);
  pContext.pScanState.pState = StateValue::Running;
  pContext.pScanState.piFilesProcessed = piFilesProcessed;
  pContext.pScanState.piTotalFiles = piTotalFiles;
  pContext.pScanState.piVectorDatabaseSize = piVectorDatabaseSize;
}

void MarkScanStopped(Context& pContext, const std::string& pSError, const bool pbFinishedPending) {
  std::lock_guard<std::mutex> lGuard(pContext.pMutex);
  pContext.pbThreadRunning = false;
  pContext.pbFinishedPending = pbFinishedPending;
  pContext.pScanState.pState = StateValue::Stopped;
  pContext.pSError = pSError;
}

void RunScanWorker(Context& pContext, const SourceSpec pSourceSpec, const RuntimeOptions pRuntimeOptions) {
  sqlite3* lPtrDatabase = nullptr;
  fs::path lPathDatabase;
  if (!OpenDatabaseForSource(pSourceSpec.pSSourceId, pRuntimeOptions, &lPtrDatabase, &lPathDatabase)) {
    MarkScanStopped(pContext, "Failed to open vector database.", false);
    return;
  }
  const auto lDatabaseCloser = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>(lPtrDatabase, &sqlite3_close);

  std::string lSError;
  if (!EnsureSchema(lPtrDatabase, &lSError)) {
    MarkScanStopped(pContext, "Failed to initialize vector database schema: " + lSError, false);
    return;
  }

  {
    std::lock_guard<std::mutex> lGuard(pContext.pMutex);
    pContext.pSActiveSourceId = pSourceSpec.pSSourceId;
    pContext.pPathSourceRoot = pSourceSpec.pPathRoot;
    pContext.pPathVectorDatabaseFile = lPathDatabase;
  }

  const std::vector<fs::path> lVecPathFiles = CollectIndexableFiles(pSourceSpec.pPathRoot);
  const std::size_t liTotalFiles = lVecPathFiles.size();
  std::size_t liFilesProcessed = 0;
  std::size_t liVectorDatabaseSize = CountVectorRows(lPtrDatabase, pSourceSpec.pSSourceId);
  UpdateRunningState(pContext, liFilesProcessed, liTotalFiles, liVectorDatabaseSize);

  ExecSql(lPtrDatabase, "BEGIN IMMEDIATE;", nullptr);

  std::unordered_set<std::string> lSetSSeenPaths;
  std::size_t liIndexedChunks = 0;
  const std::size_t liChunkCharLimit = std::max<std::size_t>(pRuntimeOptions.piChunkCharLimit, kiDefaultChunkCharLimit);

  for (const fs::path& lPathFile : lVecPathFiles) {
    std::error_code lErrorCode;
    const fs::path lPathRelative = fs::relative(lPathFile, pSourceSpec.pPathRoot, lErrorCode);
    if (lErrorCode) {
      ++liFilesProcessed;
      UpdateRunningState(pContext, liFilesProcessed, liTotalFiles, liVectorDatabaseSize);
      continue;
    }
    const std::string lSRelativePath = lPathRelative.generic_string();
    lSetSSeenPaths.insert(lSRelativePath);

    const std::optional<std::string> lOptSContent = ReadFileText(lPathFile);
    if (!lOptSContent.has_value() || IsLikelyBinary(*lOptSContent)) {
      DeleteFileAndChunks(lPtrDatabase, pSourceSpec.pSSourceId, lSRelativePath);
      ++liFilesProcessed;
      liVectorDatabaseSize = CountVectorRows(lPtrDatabase, pSourceSpec.pSSourceId);
      UpdateRunningState(pContext, liFilesProcessed, liTotalFiles, liVectorDatabaseSize);
      continue;
    }

    const std::string lSContentHash = determanistic_hash::HashTextHex(*lOptSContent);
    std::string lSPreviousHash;
    const bool lbHasPreviousHash = SelectFileHash(lPtrDatabase, pSourceSpec.pSSourceId, lSRelativePath, &lSPreviousHash);
    if (lbHasPreviousHash && lSPreviousHash == lSContentHash) {
      ++liFilesProcessed;
      UpdateRunningState(pContext, liFilesProcessed, liTotalFiles, liVectorDatabaseSize);
      continue;
    }

    std::vector<ChunkRecord> lVecChunks;
    if (IsSupportedCppFile(lPathFile)) {
      lVecChunks = ExtractChunksFromCppFile(pSourceSpec.pSSourceId, lSRelativePath, *lOptSContent, liChunkCharLimit);
    } else {
      lVecChunks =
          ExtractChunksFromDocumentationFile(pSourceSpec.pSSourceId, lSRelativePath, *lOptSContent, liChunkCharLimit);
    }
    DeleteChunksForFile(lPtrDatabase, pSourceSpec.pSSourceId, lSRelativePath);

    for (ChunkRecord& lChunk : lVecChunks) {
      std::string lSEmbeddingError;
      const std::optional<std::vector<float>> lOptVecfEmbedding =
          BuildEmbedding(pContext, pRuntimeOptions, lChunk.pSRawText, &lSEmbeddingError);
      if (!lOptVecfEmbedding.has_value() || lOptVecfEmbedding->empty()) {
        continue;
      }
      lChunk.pVecfEmbedding = *lOptVecfEmbedding;
      if (InsertChunk(lPtrDatabase, lChunk)) {
        ++liIndexedChunks;
      }
    }

    const std::uint64_t liMtimeTicks =
        static_cast<std::uint64_t>(fs::last_write_time(lPathFile, lErrorCode).time_since_epoch().count());
    const std::uintmax_t liFileSize = fs::file_size(lPathFile, lErrorCode);
    UpsertFileRow(lPtrDatabase, pSourceSpec.pSSourceId, lSRelativePath, lSContentHash, liMtimeTicks, liFileSize);

    ++liFilesProcessed;
    liVectorDatabaseSize = CountVectorRows(lPtrDatabase, pSourceSpec.pSSourceId);
    UpdateRunningState(pContext, liFilesProcessed, liTotalFiles, liVectorDatabaseSize);
  }

  const std::vector<std::string> lVecSIndexedFiles = SelectAllIndexedFiles(lPtrDatabase, pSourceSpec.pSSourceId);
  for (const std::string& lSIndexedFile : lVecSIndexedFiles) {
    if (lSetSSeenPaths.find(lSIndexedFile) != lSetSSeenPaths.end()) {
      continue;
    }
    DeleteFileAndChunks(lPtrDatabase, pSourceSpec.pSSourceId, lSIndexedFile);
  }

  ExecSql(lPtrDatabase, "COMMIT;", nullptr);

  liVectorDatabaseSize = CountVectorRows(lPtrDatabase, pSourceSpec.pSSourceId);
  UpdateRunningState(pContext, liFilesProcessed, liTotalFiles, liVectorDatabaseSize);

  if (liIndexedChunks == 0 && liVectorDatabaseSize == 0) {
    MarkScanStopped(pContext, "No chunks were indexed. Ensure a local GGUF embedding model is available.", false);
    return;
  }

  MarkScanStopped(pContext, "", true);
}

std::string FormatSnippet(const std::string& pSSourceId,
                          const std::string& pSFilePath,
                          const int piStartLine,
                          const int piEndLine,
                          const std::string& pSChunkType,
                          const std::string& pSSymbolName,
                          const std::string& pSParentSymbol,
                          std::string pSRawText) {
  constexpr std::size_t kiMaxSnippetChars = 1800;
  if (pSRawText.size() > kiMaxSnippetChars) {
    pSRawText = pSRawText.substr(0, kiMaxSnippetChars) + "\n// ... truncated ...";
  }
  std::ostringstream lStream;
  if (!pSSourceId.empty()) {
    lStream << "[" << pSSourceId << "] ";
  }
  lStream << pSFilePath << ":" << piStartLine << "-" << piEndLine << " [" << pSChunkType;
  if (!pSSymbolName.empty()) {
    lStream << " " << pSSymbolName;
  }
  if (!pSParentSymbol.empty()) {
    lStream << " parent=" << pSParentSymbol;
  }
  lStream << "]\n" << pSRawText;
  return lStream.str();
}

}  // namespace

void Shutdown(Context& pContext) {
  {
    std::lock_guard<std::mutex> lGuard(pContext.pMutex);
    pContext.pbThreadRunning = false;
  }
  if (pContext.pScanThread.joinable()) {
    pContext.pScanThread.join();
  }
#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
  std::lock_guard<std::mutex> lGuard(pContext.pMutex);
  ReleaseEmbeddingRuntimeLocked(pContext);
#endif
}

bool Scan(Context& pContext,
          const std::optional<std::string>& pOptSVectorFile,
          const RuntimeOptions& pRuntimeOptions,
          std::string* pSErrorOut) {
  if (pContext.pScanThread.joinable()) {
    bool lbCanJoin = false;
    {
      std::lock_guard<std::mutex> lGuard(pContext.pMutex);
      lbCanJoin = !pContext.pbThreadRunning;
    }
    if (lbCanJoin) {
      pContext.pScanThread.join();
    }
  }

  {
    std::lock_guard<std::mutex> lGuard(pContext.pMutex);
    if (pContext.pbThreadRunning) {
      if (pSErrorOut != nullptr) {
        *pSErrorOut = "Scan is already running.";
      }
      return false;
    }
  }

  SourceSpec lSourceSpec;
  if (!ResolveSourceSpec(pContext, pOptSVectorFile, &lSourceSpec, pSErrorOut)) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lGuard(pContext.pMutex);
    pContext.pbThreadRunning = true;
    pContext.pbFinishedPending = false;
    pContext.pScanState.pState = StateValue::Running;
    pContext.pScanState.piFilesProcessed = 0;
    pContext.pScanState.piTotalFiles = 0;
    pContext.pScanState.piVectorDatabaseSize = 0;
    pContext.pSError.clear();
  }

  pContext.pScanThread = std::thread([&pContext, lSourceSpec, pRuntimeOptions]() {
    RunScanWorker(pContext, lSourceSpec, pRuntimeOptions);
  });
  return true;
}

bool LoadRagDatabases(Context& pContext,
                      const std::vector<std::string>& pVecSDatabaseInputs,
                      const RuntimeOptions& pRuntimeOptions,
                      std::string* pSErrorOut) {
  {
    std::lock_guard<std::mutex> lGuard(pContext.pMutex);
    if (pContext.pbThreadRunning) {
      if (pSErrorOut != nullptr) {
        *pSErrorOut = "Cannot load databases while scan is running.";
      }
      return false;
    }
  }

  if (pVecSDatabaseInputs.empty()) {
    std::lock_guard<std::mutex> lGuard(pContext.pMutex);
    pContext.pVecPathLoadedDatabases.clear();
    return true;
  }

  std::vector<fs::path> lVecPathLoadedDatabases;
  for (const std::string& lSDatabaseInput : pVecSDatabaseInputs) {
    if (!ResolveDatabaseInput(lSDatabaseInput, pRuntimeOptions, &lVecPathLoadedDatabases, pSErrorOut)) {
      return false;
    }
  }

  if (lVecPathLoadedDatabases.empty()) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "No databases resolved from LoadRagDatabases inputs.";
    }
    return false;
  }

  std::vector<fs::path> lVecPathUniqueDatabases;
  std::unordered_set<std::string> lSetSSeenKeys;
  for (const fs::path& lPathDatabaseCandidate : lVecPathLoadedDatabases) {
    std::error_code lErrorCode;
    const fs::path lPathAbsolute = fs::absolute(lPathDatabaseCandidate, lErrorCode);
    const fs::path lPathNormalized = (lErrorCode ? lPathDatabaseCandidate : lPathAbsolute).lexically_normal();
    const std::string lSKey = lPathNormalized.generic_string();
    if (lSetSSeenKeys.insert(lSKey).second) {
      lVecPathUniqueDatabases.push_back(lPathNormalized);
    }
  }

  for (const fs::path& lPathDatabase : lVecPathUniqueDatabases) {
    sqlite3* lPtrDatabase = nullptr;
    if (sqlite3_open(lPathDatabase.string().c_str(), &lPtrDatabase) != SQLITE_OK) {
      if (lPtrDatabase != nullptr) {
        sqlite3_close(lPtrDatabase);
      }
      if (pSErrorOut != nullptr) {
        *pSErrorOut = "Failed to open database: " + lPathDatabase.string();
      }
      return false;
    }
    const auto lDatabaseCloser = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>(lPtrDatabase, &sqlite3_close);
    if (!HasChunksTable(lPtrDatabase)) {
      if (pSErrorOut != nullptr) {
        *pSErrorOut = "Database is missing required chunks table: " + lPathDatabase.string();
      }
      return false;
    }
  }

  std::lock_guard<std::mutex> lGuard(pContext.pMutex);
  pContext.pVecPathLoadedDatabases = std::move(lVecPathUniqueDatabases);
  return true;
}

std::vector<std::string> Fetch_Relevant_Info(Context& pContext,
                                             const std::string& pSPrompt,
                                             std::size_t piMax,
                                             std::size_t piMin,
                                             const RuntimeOptions& pRuntimeOptions) {
  std::vector<std::string> lVecSSnippets;
  if (pSPrompt.empty()) {
    return lVecSSnippets;
  }

  const std::optional<std::vector<float>> lOptVecfPromptEmbedding = BuildEmbedding(pContext, pRuntimeOptions, pSPrompt, nullptr);
  if (!lOptVecfPromptEmbedding.has_value() || lOptVecfPromptEmbedding->empty()) {
    return lVecSSnippets;
  }
  const std::vector<float>& lVecfPromptEmbedding = *lOptVecfPromptEmbedding;

  std::vector<fs::path> lVecPathDatabases;
  {
    std::lock_guard<std::mutex> lGuard(pContext.pMutex);
    lVecPathDatabases = pContext.pVecPathLoadedDatabases;
    if (lVecPathDatabases.empty() && !pContext.pPathVectorDatabaseFile.empty()) {
      lVecPathDatabases.push_back(pContext.pPathVectorDatabaseFile);
    }
  }

  if (lVecPathDatabases.empty()) {
    return lVecSSnippets;
  }

  std::vector<fs::path> lVecPathUniqueDatabases;
  std::unordered_set<std::string> lSetSSeenDatabases;
  for (const fs::path& lPathDatabaseCandidate : lVecPathDatabases) {
    std::error_code lErrorCode;
    const fs::path lPathAbsolute = fs::absolute(lPathDatabaseCandidate, lErrorCode);
    const fs::path lPathNormalized = (lErrorCode ? lPathDatabaseCandidate : lPathAbsolute).lexically_normal();
    const std::string lSKey = lPathNormalized.generic_string();
    if (lSetSSeenDatabases.insert(lSKey).second) {
      lVecPathUniqueDatabases.push_back(lPathNormalized);
    }
  }

  static constexpr const char* kSql =
      "SELECT source_id, file_path, chunk_type, symbol_name, parent_symbol, start_line, end_line, raw_text, "
      "embedding FROM chunks;";

  struct ScoredSnippet {
    std::string pSRendered;
    double pdScore = 0.0;
  };
  std::vector<ScoredSnippet> lVecScored;

  for (const fs::path& lPathDatabase : lVecPathUniqueDatabases) {
    sqlite3* lPtrDatabase = nullptr;
    if (sqlite3_open(lPathDatabase.string().c_str(), &lPtrDatabase) != SQLITE_OK) {
      if (lPtrDatabase != nullptr) {
        sqlite3_close(lPtrDatabase);
      }
      continue;
    }
    const auto lDatabaseCloser = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>(lPtrDatabase, &sqlite3_close);

    sqlite3_stmt* lPtrStatement = nullptr;
    if (sqlite3_prepare_v2(lPtrDatabase, kSql, -1, &lPtrStatement, nullptr) != SQLITE_OK) {
      continue;
    }

    while (sqlite3_step(lPtrStatement) == SQLITE_ROW) {
      const char* lPtrSourceId = reinterpret_cast<const char*>(sqlite3_column_text(lPtrStatement, 0));
      const char* lPtrFilePath = reinterpret_cast<const char*>(sqlite3_column_text(lPtrStatement, 1));
      const char* lPtrChunkType = reinterpret_cast<const char*>(sqlite3_column_text(lPtrStatement, 2));
      const char* lPtrSymbolName = reinterpret_cast<const char*>(sqlite3_column_text(lPtrStatement, 3));
      const char* lPtrParentSymbol = reinterpret_cast<const char*>(sqlite3_column_text(lPtrStatement, 4));
      const int liStartLine = sqlite3_column_int(lPtrStatement, 5);
      const int liEndLine = sqlite3_column_int(lPtrStatement, 6);
      const char* lPtrRawText = reinterpret_cast<const char*>(sqlite3_column_text(lPtrStatement, 7));
      const void* lPtrBlob = sqlite3_column_blob(lPtrStatement, 8);
      const int liBlobBytes = sqlite3_column_bytes(lPtrStatement, 8);

      if (lPtrFilePath == nullptr || lPtrChunkType == nullptr || lPtrRawText == nullptr) {
        continue;
      }

      const std::vector<float> lVecfChunkEmbedding = DeserializeEmbedding(lPtrBlob, liBlobBytes);
      if (lVecfChunkEmbedding.size() != lVecfPromptEmbedding.size()) {
        continue;
      }
      const double ldCosine = CosineSimilarity(lVecfPromptEmbedding, lVecfChunkEmbedding);
      if (ldCosine <= 0.0) {
        continue;
      }

      double ldScore = ldCosine;
      const std::string lSChunkType = lPtrChunkType;
      if (lSChunkType == "function" || lSChunkType == "method") {
        ldScore += 0.06;
      } else if (lSChunkType == "class_overview" || lSChunkType == "struct_overview") {
        ldScore += 0.03;
      }

      ScoredSnippet lScoredSnippet;
      lScoredSnippet.pdScore = ldScore;
      lScoredSnippet.pSRendered = FormatSnippet(
          lPtrSourceId == nullptr ? "" : lPtrSourceId, lPtrFilePath, liStartLine, liEndLine, lSChunkType,
          lPtrSymbolName == nullptr ? "" : lPtrSymbolName, lPtrParentSymbol == nullptr ? "" : lPtrParentSymbol,
          lPtrRawText);
      lVecScored.push_back(std::move(lScoredSnippet));
    }
    sqlite3_finalize(lPtrStatement);
  }

  std::sort(lVecScored.begin(), lVecScored.end(), [](const ScoredSnippet& pLhs, const ScoredSnippet& pRhs) {
    return pLhs.pdScore > pRhs.pdScore;
  });

  const std::size_t liMaxCount = std::max<std::size_t>(1, piMax);
  const std::size_t liMinCount = std::min(liMaxCount, std::max<std::size_t>(1, piMin));
  std::unordered_set<std::string> lSetSRendered;
  for (const ScoredSnippet& lScoredSnippet : lVecScored) {
    if (lVecSSnippets.size() >= liMaxCount) {
      break;
    }
    if (lScoredSnippet.pdScore < 0.1 && lVecSSnippets.size() >= liMinCount) {
      break;
    }
    if (!lSetSRendered.insert(lScoredSnippet.pSRendered).second) {
      continue;
    }
    lVecSSnippets.push_back(lScoredSnippet.pSRendered);
  }

  for (std::size_t liIndex = 0; liIndex < lVecScored.size() && lVecSSnippets.size() < liMinCount; ++liIndex) {
    const std::string& lSRendered = lVecScored[liIndex].pSRendered;
    if (!lSetSRendered.insert(lSRendered).second) {
      continue;
    }
    lVecSSnippets.push_back(lSRendered);
  }

  return lVecSSnippets;
}

ScanStateSnapshot Fetch_state(Context& pContext) {
  if (pContext.pScanThread.joinable()) {
    bool lbShouldJoin = false;
    {
      std::lock_guard<std::mutex> lGuard(pContext.pMutex);
      lbShouldJoin = !pContext.pbThreadRunning;
    }
    if (lbShouldJoin) {
      pContext.pScanThread.join();
    }
  }

  std::lock_guard<std::mutex> lGuard(pContext.pMutex);
  if (pContext.pbThreadRunning) {
    ScanStateSnapshot lStateSnapshot = pContext.pScanState;
    lStateSnapshot.pState = StateValue::Running;
    return lStateSnapshot;
  }
  if (pContext.pbFinishedPending) {
    pContext.pbFinishedPending = false;
    ScanStateSnapshot lStateSnapshot = pContext.pScanState;
    lStateSnapshot.pState = StateValue::Finished;
    return lStateSnapshot;
  }
  ScanStateSnapshot lStateSnapshot;
  lStateSnapshot.pState = StateValue::Stopped;
  return lStateSnapshot;
}

}  // namespace ollama_engine::internal::vectorised_rag
