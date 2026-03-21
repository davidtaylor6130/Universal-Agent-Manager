#include "local_ollama_engine.h"

#include "embedding_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
#include "llama.h"
#endif

namespace ollama_engine::internal {
namespace {
namespace fs = std::filesystem;

constexpr std::size_t ki_DefaultContextTokens = 2048;
constexpr std::size_t ki_DefaultMaxGeneratedTokens = 256;
constexpr int32_t ki_DefaultGpuLayers = 99;

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
std::once_flag g_OnceLlamaRuntimeInit;

/// <summary>Filters llama.cpp logs so the CLI remains readable.</summary>
/// <param name="pLogLevel">Log severity emitted by llama.cpp.</param>
/// <param name="pSMessage">Log message bytes.</param>
/// <param name="pPtrUserData">Unused callback user data pointer.</param>
void LlamaLogCallback(enum ggml_log_level pLogLevel, const char* pSMessage, void* pPtrUserData) {
  (void)pPtrUserData;
  if (pLogLevel >= GGML_LOG_LEVEL_ERROR && pSMessage != nullptr) {
    std::fputs(pSMessage, stderr);
  }
}

/// <summary>
/// Keeps llama.cpp model-load progress internal so CLI output stays focused on prompts/responses.
/// </summary>
/// <param name="pfProgress">Progress value in the range [0.0, 1.0].</param>
/// <param name="pPtrUserData">Unused callback user data pointer.</param>
/// <returns>Always true to continue loading.</returns>
bool LlamaLoadProgressCallback(float pfProgress, void* pPtrUserData) {
  (void)pfProgress;
  (void)pPtrUserData;
  return true;
}
#endif

/// <summary>Returns true when a filename is hidden on Unix-like systems.</summary>
/// <param name="pSFileName">Name to inspect.</param>
/// <returns>True if the name starts with a dot.</returns>
bool IsHiddenFileName(const std::string& pSFileName) {
  return !pSFileName.empty() && pSFileName.front() == '.';
}

/// <summary>Lowercases ASCII characters for extension comparisons.</summary>
/// <param name="pSInput">Input text.</param>
/// <returns>Lowercased copy.</returns>
std::string ToLowerAscii(std::string pSInput) {
  std::transform(pSInput.begin(), pSInput.end(), pSInput.begin(),
                 [](unsigned char pCChar) { return static_cast<char>(std::tolower(pCChar)); });
  return pSInput;
}

/// <summary>Checks whether a file path points to a GGUF model file.</summary>
/// <param name="pPathFile">Path to inspect.</param>
/// <returns>True for supported model extensions.</returns>
bool IsSupportedModelFile(const fs::path& pPathFile) {
  return ToLowerAscii(pPathFile.extension().string()) == ".gguf";
}

/// <summary>
/// Lists GGUF files under a directory recursively while skipping hidden files/folders.
/// </summary>
/// <param name="pPathRootDirectory">Directory to scan.</param>
/// <returns>Absolute file paths for discovered model files.</returns>
std::vector<fs::path> DiscoverModelFilesRecursively(const fs::path& pPathRootDirectory) {
  std::vector<fs::path> lVecPathModelFiles;
  std::error_code lErrorCode;
  if (!fs::exists(pPathRootDirectory, lErrorCode) || !fs::is_directory(pPathRootDirectory, lErrorCode)) {
    return lVecPathModelFiles;
  }

  const fs::directory_options lDirectoryOptions = fs::directory_options::skip_permission_denied;
  fs::recursive_directory_iterator lIterator(pPathRootDirectory, lDirectoryOptions, lErrorCode);
  const fs::recursive_directory_iterator lEndIterator;
  for (; lIterator != lEndIterator; lIterator.increment(lErrorCode)) {
    if (lErrorCode) {
      lErrorCode.clear();
      continue;
    }

    const fs::directory_entry& lDirectoryEntry = *lIterator;
    const std::string lSName = lDirectoryEntry.path().filename().string();
    if (IsHiddenFileName(lSName)) {
      if (lDirectoryEntry.is_directory(lErrorCode) && !lErrorCode) {
        lIterator.disable_recursion_pending();
      }
      continue;
    }

    if (!lDirectoryEntry.is_regular_file(lErrorCode) || lErrorCode) {
      lErrorCode.clear();
      continue;
    }

    if (!IsSupportedModelFile(lDirectoryEntry.path())) {
      continue;
    }

    lVecPathModelFiles.push_back(lDirectoryEntry.path());
  }

  std::sort(lVecPathModelFiles.begin(), lVecPathModelFiles.end());
  return lVecPathModelFiles;
}

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
/// <summary>Initializes llama.cpp backend APIs once for the current process.</summary>
void EnsureLlamaRuntimeInitialized() {
  std::call_once(g_OnceLlamaRuntimeInit, []() {
    llama_log_set(LlamaLogCallback, nullptr);
    llama_backend_init();
    ggml_backend_load_all();
  });
}

/// <summary>Formats a user prompt with the model's chat template when available.</summary>
/// <param name="pPtrModel">Loaded llama model.</param>
/// <param name="pSPrompt">Raw user prompt.</param>
/// <returns>Formatted prompt for generation, or nullopt on formatting failure.</returns>
std::optional<std::string> BuildPromptWithChatTemplate(const llama_model* pPtrModel,
                                                       const std::string& pSPrompt) {
  if (pPtrModel == nullptr) {
    return std::nullopt;
  }
  const char* lPtrTemplate = llama_model_chat_template(pPtrModel, nullptr);
  if (lPtrTemplate == nullptr) {
    return pSPrompt;
  }

  llama_chat_message lChatMessage{};
  lChatMessage.role = "user";
  lChatMessage.content = pSPrompt.c_str();

  std::vector<char> lVecCBuffer(std::max<std::size_t>(1024, (pSPrompt.size() * 2) + 512));
  int32_t liWrittenBytes =
      llama_chat_apply_template(lPtrTemplate, &lChatMessage, 1, true, lVecCBuffer.data(), lVecCBuffer.size());
  if (liWrittenBytes > static_cast<int32_t>(lVecCBuffer.size())) {
    lVecCBuffer.resize(static_cast<std::size_t>(liWrittenBytes));
    liWrittenBytes =
        llama_chat_apply_template(lPtrTemplate, &lChatMessage, 1, true, lVecCBuffer.data(), lVecCBuffer.size());
  }
  if (liWrittenBytes < 0) {
    return std::nullopt;
  }
  return std::string(lVecCBuffer.data(), static_cast<std::size_t>(liWrittenBytes));
}

/// <summary>Converts one sampled token to text bytes.</summary>
/// <param name="pPtrVocab">Vocabulary associated with the loaded model.</param>
/// <param name="piToken">Sampled token id.</param>
/// <returns>Decoded token bytes, or nullopt on conversion failure.</returns>
std::optional<std::string> TokenToPieceText(const llama_vocab* pPtrVocab, const llama_token piToken) {
  if (pPtrVocab == nullptr) {
    return std::nullopt;
  }

  std::array<char, 256> lArrCBuffer{};
  int32_t liPieceLength =
      llama_token_to_piece(pPtrVocab, piToken, lArrCBuffer.data(), lArrCBuffer.size(), 0, false);
  if (liPieceLength < 0) {
    const int32_t liRequiredBufferLength = -liPieceLength;
    if (liRequiredBufferLength <= 0) {
      return std::nullopt;
    }
    std::vector<char> lVecCBuffer(static_cast<std::size_t>(liRequiredBufferLength));
    liPieceLength =
        llama_token_to_piece(pPtrVocab, piToken, lVecCBuffer.data(), lVecCBuffer.size(), 0, false);
    if (liPieceLength < 0) {
      return std::nullopt;
    }
    return std::string(lVecCBuffer.data(), static_cast<std::size_t>(liPieceLength));
  }

  return std::string(lArrCBuffer.data(), static_cast<std::size_t>(liPieceLength));
}

/// <summary>Trims ASCII whitespace from both ends.</summary>
/// <param name="pSInput">Input text.</param>
/// <returns>Trimmed text.</returns>
std::string TrimAsciiWhitespace(const std::string& pSInput) {
  std::size_t liBegin = 0;
  while (liBegin < pSInput.size() && std::isspace(static_cast<unsigned char>(pSInput[liBegin])) != 0) {
    ++liBegin;
  }
  std::size_t liEnd = pSInput.size();
  while (liEnd > liBegin && std::isspace(static_cast<unsigned char>(pSInput[liEnd - 1])) != 0) {
    --liEnd;
  }
  return pSInput.substr(liBegin, liEnd - liBegin);
}

/// <summary>Normalizes token marker text for comparisons.</summary>
/// <param name="pSInput">Token text.</param>
/// <returns>Lowercased and trimmed marker text.</returns>
std::string NormalizeTokenMarkerText(const std::string& pSInput) {
  return TrimAsciiWhitespace(ToLowerAscii(pSInput));
}

/// <summary>Returns true when token text is an explicit stop marker.</summary>
/// <param name="pSNormalizedTokenText">Normalized marker text.</param>
/// <returns>True for known end-of-generation marker forms.</returns>
bool IsKnownStopTokenText(const std::string& pSNormalizedTokenText) {
  return pSNormalizedTokenText == "</s>" || pSNormalizedTokenText == "<|eot_id|>" ||
         pSNormalizedTokenText == "<|eom_id|>" || pSNormalizedTokenText == "<|end|>" ||
         pSNormalizedTokenText == "<|endoftext|>" || pSNormalizedTokenText == "<|end_of_text|>" ||
         pSNormalizedTokenText == "<end_of_turn>" || pSNormalizedTokenText == "<|im_end|>";
}

/// <summary>Returns true when token text represents a reasoning/thinking marker.</summary>
/// <param name="pSNormalizedTokenText">Normalized marker text.</param>
/// <returns>True for known think/reasoning marker tokens.</returns>
bool IsKnownThinkingMarkerTokenText(const std::string& pSNormalizedTokenText) {
  return pSNormalizedTokenText == "<think>" || pSNormalizedTokenText == "</think>" ||
         pSNormalizedTokenText == "<thinking>" || pSNormalizedTokenText == "</thinking>" ||
         pSNormalizedTokenText == "<|start_think|>" || pSNormalizedTokenText == "<|end_think|>" ||
         pSNormalizedTokenText == "<|thinking|>" || pSNormalizedTokenText == "<reasoning>" ||
         pSNormalizedTokenText == "</reasoning>";
}

/// <summary>Returns true when a marker token opens a thinking section.</summary>
/// <param name="pSNormalizedTokenText">Normalized marker text.</param>
/// <returns>True if this token starts reasoning mode.</returns>
bool IsThinkingStartTokenText(const std::string& pSNormalizedTokenText) {
  return pSNormalizedTokenText == "<think>" || pSNormalizedTokenText == "<thinking>" ||
         pSNormalizedTokenText == "<|start_think|>" || pSNormalizedTokenText == "<|thinking|>" ||
         pSNormalizedTokenText == "<reasoning>";
}

/// <summary>Returns true when a marker token closes a thinking section.</summary>
/// <param name="pSNormalizedTokenText">Normalized marker text.</param>
/// <returns>True if this token ends reasoning mode.</returns>
bool IsThinkingEndTokenText(const std::string& pSNormalizedTokenText) {
  return pSNormalizedTokenText == "</think>" || pSNormalizedTokenText == "</thinking>" ||
         pSNormalizedTokenText == "<|end_think|>" || pSNormalizedTokenText == "</reasoning>";
}

/// <summary>Reads normalized marker text for a token id from the vocab.</summary>
/// <param name="pPtrVocab">Vocabulary associated with the loaded model.</param>
/// <param name="piToken">Token id.</param>
/// <returns>Normalized token marker text (possibly empty).</returns>
std::string GetNormalizedTokenMarkerText(const llama_vocab* pPtrVocab, const llama_token piToken) {
  if (pPtrVocab == nullptr) {
    return {};
  }
  const char* pSRawTokenText = llama_vocab_get_text(pPtrVocab, piToken);
  if (pSRawTokenText == nullptr) {
    return {};
  }
  return NormalizeTokenMarkerText(pSRawTokenText);
}

/// <summary>Removes all case-insensitive marker occurrences.</summary>
/// <param name="pSText">Source text.</param>
/// <param name="pSMarkerLower">Marker text in lowercase.</param>
/// <returns>Text with marker removed.</returns>
std::string RemoveAllCaseInsensitive(std::string pSText, const std::string& pSMarkerLower) {
  if (pSMarkerLower.empty()) {
    return pSText;
  }

  std::string lSLower = ToLowerAscii(pSText);
  std::size_t liPosition = 0;
  while (true) {
    liPosition = lSLower.find(pSMarkerLower, liPosition);
    if (liPosition == std::string::npos) {
      break;
    }
    pSText.erase(liPosition, pSMarkerLower.size());
    lSLower.erase(liPosition, pSMarkerLower.size());
  }
  return pSText;
}

/// <summary>Removes case-insensitive delimited sections such as think blocks.</summary>
/// <param name="pSText">Source text.</param>
/// <param name="pSStartLower">Start marker in lowercase.</param>
/// <param name="pSEndLower">End marker in lowercase.</param>
/// <returns>Text with delimited segments removed.</returns>
std::string RemoveDelimitedSegmentsCaseInsensitive(std::string pSText,
                                                   const std::string& pSStartLower,
                                                   const std::string& pSEndLower) {
  if (pSStartLower.empty() || pSEndLower.empty()) {
    return pSText;
  }

  std::string lSLower = ToLowerAscii(pSText);
  std::size_t liSearchPosition = 0;
  while (true) {
    const std::size_t liStart = lSLower.find(pSStartLower, liSearchPosition);
    if (liStart == std::string::npos) {
      break;
    }
    const std::size_t liEnd = lSLower.find(pSEndLower, liStart + pSStartLower.size());
    const std::size_t liEraseEnd =
        (liEnd == std::string::npos) ? pSText.size() : (liEnd + pSEndLower.size());
    pSText.erase(liStart, liEraseEnd - liStart);
    lSLower.erase(liStart, liEraseEnd - liStart);
    liSearchPosition = liStart;
  }
  return pSText;
}

/// <summary>Strips known reasoning blocks/markers from generated text.</summary>
/// <param name="pSText">Generated text.</param>
/// <returns>Visible assistant response text.</returns>
std::string StripReasoningSections(const std::string& pSText) {
  std::string lSOutput = pSText;
  lSOutput = RemoveDelimitedSegmentsCaseInsensitive(lSOutput, "<think>", "</think>");
  lSOutput = RemoveDelimitedSegmentsCaseInsensitive(lSOutput, "<thinking>", "</thinking>");
  lSOutput = RemoveDelimitedSegmentsCaseInsensitive(lSOutput, "<|start_think|>", "<|end_think|>");
  lSOutput = RemoveDelimitedSegmentsCaseInsensitive(lSOutput, "<reasoning>", "</reasoning>");

  lSOutput = RemoveAllCaseInsensitive(lSOutput, "<think>");
  lSOutput = RemoveAllCaseInsensitive(lSOutput, "</think>");
  lSOutput = RemoveAllCaseInsensitive(lSOutput, "<thinking>");
  lSOutput = RemoveAllCaseInsensitive(lSOutput, "</thinking>");
  lSOutput = RemoveAllCaseInsensitive(lSOutput, "<|start_think|>");
  lSOutput = RemoveAllCaseInsensitive(lSOutput, "<|end_think|>");
  lSOutput = RemoveAllCaseInsensitive(lSOutput, "<|thinking|>");
  lSOutput = RemoveAllCaseInsensitive(lSOutput, "<reasoning>");
  lSOutput = RemoveAllCaseInsensitive(lSOutput, "</reasoning>");
  return lSOutput;
}

enum class SampledTokenBehavior {
  Stop,
  Thinking,
  Hidden,
  Emit
};

/// <summary>Classifies sampled tokens as stop/thinking/hidden/emit for robust generation.</summary>
/// <param name="pPtrVocab">Vocabulary for the loaded model.</param>
/// <param name="piToken">Sampled token id.</param>
/// <returns>Behavior classification for this token.</returns>
SampledTokenBehavior ClassifySampledToken(const llama_vocab* pPtrVocab, const llama_token piToken) {
  if (pPtrVocab == nullptr) {
    return SampledTokenBehavior::Stop;
  }
  if (llama_vocab_is_eog(pPtrVocab, piToken)) {
    return SampledTokenBehavior::Stop;
  }

  const std::string lSNormalizedTokenText = GetNormalizedTokenMarkerText(pPtrVocab, piToken);
  if (IsKnownStopTokenText(lSNormalizedTokenText)) {
    return SampledTokenBehavior::Stop;
  }
  if (IsKnownThinkingMarkerTokenText(lSNormalizedTokenText)) {
    return SampledTokenBehavior::Thinking;
  }
  if (llama_vocab_is_control(pPtrVocab, piToken)) {
    return SampledTokenBehavior::Hidden;
  }
  return SampledTokenBehavior::Emit;
}

/// <summary>
/// Chooses a highest-logit fallback token from the latest decoder output.
/// First pass prefers visible tokens; second pass allows hidden non-stop tokens.
/// </summary>
/// <param name="pPtrVocab">Vocabulary associated with the loaded model.</param>
/// <param name="pPtrContext">Active llama context with current logits.</param>
/// <returns>Best fallback token id when available.</returns>
std::optional<llama_token> SelectBestFallbackToken(const llama_vocab* pPtrVocab, llama_context* pPtrContext) {
  if (pPtrVocab == nullptr || pPtrContext == nullptr) {
    return std::nullopt;
  }

  float* lPtrfLogits = llama_get_logits_ith(pPtrContext, -1);
  if (lPtrfLogits == nullptr) {
    return std::nullopt;
  }

  const int32_t liVocabSize = llama_vocab_n_tokens(pPtrVocab);
  if (liVocabSize <= 0) {
    return std::nullopt;
  }

  auto lFindBestByBehavior = [&](const SampledTokenBehavior pDesiredBehavior) -> std::optional<llama_token> {
    bool lbFound = false;
    llama_token liBestToken = LLAMA_TOKEN_NULL;
    float lfBestLogit = -std::numeric_limits<float>::infinity();
    for (int32_t liTokenIndex = 0; liTokenIndex < liVocabSize; ++liTokenIndex) {
      const llama_token liToken = static_cast<llama_token>(liTokenIndex);
      const SampledTokenBehavior lTokenBehavior = ClassifySampledToken(pPtrVocab, liToken);
      if (lTokenBehavior != pDesiredBehavior) {
        continue;
      }

      const float lfLogit = lPtrfLogits[liTokenIndex];
      if (!lbFound || lfLogit > lfBestLogit) {
        lbFound = true;
        liBestToken = liToken;
        lfBestLogit = lfLogit;
      }
    }
    if (!lbFound) {
      return std::nullopt;
    }
    return liBestToken;
  };

  const std::optional<llama_token> lOptVisibleToken = lFindBestByBehavior(SampledTokenBehavior::Emit);
  if (lOptVisibleToken.has_value()) {
    return lOptVisibleToken;
  }
  const std::optional<llama_token> lOptThinkingToken = lFindBestByBehavior(SampledTokenBehavior::Thinking);
  if (lOptThinkingToken.has_value()) {
    return lOptThinkingToken;
  }
  return lFindBestByBehavior(SampledTokenBehavior::Hidden);
}
#endif

}  // namespace

/// <summary>
/// Initializes the local engine and normalizes runtime options.
/// </summary>
/// <param name="pEngineOptions">Runtime options used by this engine instance.</param>
LocalOllamaCppEngine::LocalOllamaCppEngine(EngineOptions pEngineOptions)
    : mEngineOptions(std::move(pEngineOptions)) {
  if (mEngineOptions.pPathModelFolder.empty()) {
    mEngineOptions.pPathModelFolder = fs::current_path() / "models";
  }
  mEngineOptions.piEmbeddingDimensions =
      std::clamp<std::size_t>(mEngineOptions.piEmbeddingDimensions, 32, 4096);
}

/// <summary>Releases loaded runtime resources.</summary>
LocalOllamaCppEngine::~LocalOllamaCppEngine() {
  std::lock_guard<std::mutex> lGuard(mMutexEngineRuntime);
  ReleaseRuntimeLocked();
}

/// <summary>Enumerates model files from the configured model folder.</summary>
/// <returns>Sorted model file names.</returns>
std::vector<std::string> LocalOllamaCppEngine::ListModels() {
  std::vector<std::string> lVecSModels;
  const std::vector<fs::path> lVecPathModelFiles = DiscoverModelFilesRecursively(mEngineOptions.pPathModelFolder);
  std::error_code lErrorCode;
  for (const fs::path& lPathModelFile : lVecPathModelFiles) {
    const fs::path lPathRelative = fs::relative(lPathModelFile, mEngineOptions.pPathModelFolder, lErrorCode);
    if (lErrorCode) {
      lErrorCode.clear();
      continue;
    }
    lVecSModels.push_back(lPathRelative.generic_string());
  }
  std::sort(lVecSModels.begin(), lVecSModels.end());
  return lVecSModels;
}

/// <summary>Frees active llama model/context/sampler pointers.</summary>
void LocalOllamaCppEngine::ReleaseRuntimeLocked() {
#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
  if (mPtrSampler != nullptr) {
    llama_sampler_free(mPtrSampler);
    mPtrSampler = nullptr;
  }
  if (mPtrContext != nullptr) {
    llama_free(mPtrContext);
    mPtrContext = nullptr;
  }
  if (mPtrModel != nullptr) {
    llama_model_free(mPtrModel);
    mPtrModel = nullptr;
  }
  mPathLoadedModelFile.clear();
#endif
}

/// <summary>Loads a model file and updates progress state.</summary>
/// <param name="pSModelName">Model file name in the configured model folder.</param>
/// <param name="pSErrorOut">Optional output pointer for error details.</param>
/// <returns>True when the model load succeeds.</returns>
bool LocalOllamaCppEngine::Load(const std::string& pSModelName, std::string* pSErrorOut) {
  if (pSModelName.empty()) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Model name is empty.";
    }
    return false;
  }

  const fs::path lPathModelCandidate = mEngineOptions.pPathModelFolder / pSModelName;
  fs::path lPathModel = lPathModelCandidate;
  std::string lSModelDisplayName = pSModelName;
  std::error_code lErrorCode;
  if (!fs::exists(lPathModelCandidate, lErrorCode)) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Model not found in model folder: " + pSModelName;
    }
    return false;
  }
  lErrorCode.clear();

  if (fs::is_directory(lPathModelCandidate, lErrorCode) && !lErrorCode) {
    const std::vector<fs::path> lVecPathModelFiles = DiscoverModelFilesRecursively(lPathModelCandidate);
    if (lVecPathModelFiles.empty()) {
      if (pSErrorOut != nullptr) {
        *pSErrorOut = "No .gguf model file found inside directory: " + pSModelName;
      }
      return false;
    }
    lPathModel = lVecPathModelFiles.front();
  } else if (!fs::is_regular_file(lPathModelCandidate, lErrorCode) || lErrorCode) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Model selection is not a regular file: " + pSModelName;
    }
    return false;
  }

  if (!IsSupportedModelFile(lPathModel)) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Unsupported model extension. Expected .gguf";
    }
    return false;
  }

  const fs::path lPathRelativeModel = fs::relative(lPathModel, mEngineOptions.pPathModelFolder, lErrorCode);
  if (!lErrorCode && !lPathRelativeModel.empty()) {
    lSModelDisplayName = lPathRelativeModel.generic_string();
  }

  const std::size_t liFileUnits = static_cast<std::size_t>(fs::file_size(lPathModel, lErrorCode));
  const std::size_t liTotalUnits = (lErrorCode || liFileUnits == 0) ? static_cast<std::size_t>(1) : liFileUnits;

  std::lock_guard<std::mutex> lRuntimeGuard(mMutexEngineRuntime);
  ReleaseRuntimeLocked();

  {
    std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
    mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Loading;
    mCurrentStateResponse.pSLoadedModelName.clear();
    mCurrentStateResponse.pOptLoadingStructure =
        LoadingStructure{lSModelDisplayName, 0, liTotalUnits, "Loading model from disk"};
    mCurrentStateResponse.pOptRunningStructure.reset();
  }

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
  EnsureLlamaRuntimeInitialized();

  llama_model_params lModelParams = llama_model_default_params();
  lModelParams.n_gpu_layers = ki_DefaultGpuLayers;
  lModelParams.progress_callback = LlamaLoadProgressCallback;
  lModelParams.progress_callback_user_data = nullptr;

  llama_model* lPtrModel = llama_model_load_from_file(lPathModel.string().c_str(), lModelParams);
  if (lPtrModel == nullptr) {
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Failed to load GGUF model via llama.cpp: " + lPathModel.string();
    }
    std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
    mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Idle;
    mCurrentStateResponse.pOptLoadingStructure.reset();
    mCurrentStateResponse.pOptRunningStructure.reset();
    return false;
  }

  llama_context_params lContextParams = llama_context_default_params();
  lContextParams.n_ctx = ki_DefaultContextTokens;
  lContextParams.n_batch = std::min<std::size_t>(ki_DefaultContextTokens, 512);
  lContextParams.n_ubatch = lContextParams.n_batch;

  llama_context* lPtrContext = llama_init_from_model(lPtrModel, lContextParams);
  if (lPtrContext == nullptr) {
    llama_model_free(lPtrModel);
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Failed to create llama.cpp context.";
    }
    std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
    mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Idle;
    mCurrentStateResponse.pOptLoadingStructure.reset();
    mCurrentStateResponse.pOptRunningStructure.reset();
    return false;
  }

  llama_sampler_chain_params lSamplerChainParams = llama_sampler_chain_default_params();
  llama_sampler* lPtrSampler = llama_sampler_chain_init(lSamplerChainParams);
  if (lPtrSampler == nullptr) {
    llama_free(lPtrContext);
    llama_model_free(lPtrModel);
    if (pSErrorOut != nullptr) {
      *pSErrorOut = "Failed to create llama.cpp sampler chain.";
    }
    std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
    mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Idle;
    mCurrentStateResponse.pOptLoadingStructure.reset();
    mCurrentStateResponse.pOptRunningStructure.reset();
    return false;
  }

  llama_sampler_chain_add(lPtrSampler, llama_sampler_init_min_p(0.05f, 1));
  llama_sampler_chain_add(lPtrSampler, llama_sampler_init_temp(0.8f));
  llama_sampler_chain_add(lPtrSampler, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

  mPtrModel = lPtrModel;
  mPtrContext = lPtrContext;
  mPtrSampler = lPtrSampler;
  mPathLoadedModelFile = lPathModel;

  {
    std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
    ms_LoadedModelName = lSModelDisplayName;
    mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Loaded;
    mCurrentStateResponse.pSLoadedModelName = lSModelDisplayName;
    mCurrentStateResponse.pOptLoadingStructure.reset();
    mCurrentStateResponse.pOptRunningStructure.reset();
  }
  return true;
#else
  (void)lPathModel;
  (void)liTotalUnits;
  if (pSErrorOut != nullptr) {
    *pSErrorOut = "llama.cpp is not linked. Reconfigure with UAM_FETCH_LLAMA_CPP=ON.";
  }
  std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
  mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Idle;
  mCurrentStateResponse.pOptLoadingStructure.reset();
  mCurrentStateResponse.pOptRunningStructure.reset();
  return false;
#endif
}

/// <summary>Processes a prompt and returns a model response and optional embedding.</summary>
/// <param name="pSPrompt">Prompt text to process.</param>
/// <returns>Response payload with status, text, embedding, and error fields.</returns>
SendMessageResponse LocalOllamaCppEngine::SendMessage(const std::string& pSPrompt) {
  SendMessageResponse lSendMessageResponse;

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
  std::lock_guard<std::mutex> lRuntimeGuard(mMutexEngineRuntime);
  if (mPtrModel == nullptr || mPtrContext == nullptr || mPtrSampler == nullptr || ms_LoadedModelName.empty()) {
    lSendMessageResponse.pSError = "No model is loaded.";
    return lSendMessageResponse;
  }

  const llama_vocab* lPtrVocab = llama_model_get_vocab(mPtrModel);
  if (lPtrVocab == nullptr) {
    lSendMessageResponse.pSError = "Loaded model has no vocabulary.";
    return lSendMessageResponse;
  }

  std::string lSPromptForModel = pSPrompt;
  const std::optional<std::string> lOptFormattedPrompt = BuildPromptWithChatTemplate(mPtrModel, pSPrompt);
  if (lOptFormattedPrompt.has_value() && !lOptFormattedPrompt->empty()) {
    lSPromptForModel = *lOptFormattedPrompt;
  }

  const int32_t liPromptTokenCountWanted = -llama_tokenize(
      lPtrVocab, lSPromptForModel.c_str(), static_cast<int32_t>(lSPromptForModel.size()), nullptr, 0, true, true);
  if (liPromptTokenCountWanted <= 0) {
    lSendMessageResponse.pSError = "Prompt tokenization failed.";
    return lSendMessageResponse;
  }

  std::vector<llama_token> lVeciPromptTokens(static_cast<std::size_t>(liPromptTokenCountWanted));
  const int32_t liPromptTokenCountWritten = llama_tokenize(lPtrVocab, lSPromptForModel.c_str(),
                                                           static_cast<int32_t>(lSPromptForModel.size()),
                                                           lVeciPromptTokens.data(),
                                                           static_cast<int32_t>(lVeciPromptTokens.size()), true, true);
  if (liPromptTokenCountWritten < 0) {
    lSendMessageResponse.pSError = "Prompt tokenization write pass failed.";
    return lSendMessageResponse;
  }
  lVeciPromptTokens.resize(static_cast<std::size_t>(liPromptTokenCountWritten));

  const int32_t liContextSize = static_cast<int32_t>(llama_n_ctx(mPtrContext));
  if (liContextSize <= 2) {
    lSendMessageResponse.pSError = "Context size is too small for generation.";
    return lSendMessageResponse;
  }

  if (static_cast<int32_t>(lVeciPromptTokens.size()) >= liContextSize) {
    const std::size_t liKeepTokenCount = static_cast<std::size_t>(liContextSize - 1);
    const std::size_t liDropTokenCount = lVeciPromptTokens.size() - liKeepTokenCount;
    lVeciPromptTokens.erase(lVeciPromptTokens.begin(), lVeciPromptTokens.begin() + liDropTokenCount);
  }

  llama_memory_clear(llama_get_memory(mPtrContext), true);
  llama_sampler_reset(mPtrSampler);

  const std::size_t liTotalUnits = lVeciPromptTokens.size() + ki_DefaultMaxGeneratedTokens;
  {
    std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
    mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Running;
    mCurrentStateResponse.pOptRunningStructure =
        RunningStructure{ms_LoadedModelName, 0, liTotalUnits, "Tokenizing and evaluating prompt"};
    mCurrentStateResponse.pOptLoadingStructure.reset();
  }

  llama_batch lPromptBatch =
      llama_batch_get_one(lVeciPromptTokens.data(), static_cast<int32_t>(lVeciPromptTokens.size()));
  if (llama_decode(mPtrContext, lPromptBatch) != 0) {
    lSendMessageResponse.pSError = "llama_decode failed while evaluating prompt.";
    std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
    mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Loaded;
    mCurrentStateResponse.pSLoadedModelName = ms_LoadedModelName;
    mCurrentStateResponse.pOptRunningStructure.reset();
    return lSendMessageResponse;
  }

  std::size_t liProcessedUnits = lVeciPromptTokens.size();
  std::string lSResponseText;
  lSResponseText.reserve(1024);
  bool lbThinkingMode = false;

  auto UpdateGenerationState = [&](const EngineLifecycleState pEngineLifecycleState, const std::string& pSDetail,
                                   const std::size_t piProcessedUnits) {
    std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
    mCurrentStateResponse.pEngineLifecycleState = pEngineLifecycleState;
    if (mCurrentStateResponse.pOptRunningStructure.has_value()) {
      mCurrentStateResponse.pOptRunningStructure->piProcessedUnits = piProcessedUnits;
      mCurrentStateResponse.pOptRunningStructure->pSDetail = pSDetail;
    }
  };

  UpdateGenerationState(EngineLifecycleState::Running, "Generating response", liProcessedUnits);

  for (std::size_t liStep = 0; liStep < ki_DefaultMaxGeneratedTokens; ++liStep) {
    const llama_pos liSeqPosMax = llama_memory_seq_pos_max(llama_get_memory(mPtrContext), 0);
    if ((liSeqPosMax + 1) >= liContextSize) {
      break;
    }

    llama_token liNextToken = llama_sampler_sample(mPtrSampler, mPtrContext, -1);
    SampledTokenBehavior lTokenBehavior = ClassifySampledToken(lPtrVocab, liNextToken);
    if (lTokenBehavior == SampledTokenBehavior::Stop) {
      if (liStep == 0 && lSResponseText.empty()) {
        // First-token stop markers can happen with stochastic samplers; retry before giving up.
        bool lbRecoveredWithResample = false;
        constexpr int kiMaxResampleAttempts = 4;
        for (int liAttempt = 0; liAttempt < kiMaxResampleAttempts; ++liAttempt) {
          const llama_token liRetryToken = llama_sampler_sample(mPtrSampler, mPtrContext, -1);
          const SampledTokenBehavior lRetryBehavior = ClassifySampledToken(lPtrVocab, liRetryToken);
          if (lRetryBehavior != SampledTokenBehavior::Stop) {
            liNextToken = liRetryToken;
            lTokenBehavior = lRetryBehavior;
            lbRecoveredWithResample = true;
            break;
          }
        }

        if (!lbRecoveredWithResample) {
          const std::optional<llama_token> lOptFallbackToken = SelectBestFallbackToken(lPtrVocab, mPtrContext);
          if (!lOptFallbackToken.has_value()) {
            break;
          }
          liNextToken = *lOptFallbackToken;
          lTokenBehavior = ClassifySampledToken(lPtrVocab, liNextToken);
          if (lTokenBehavior == SampledTokenBehavior::Stop) {
            break;
          }
        }
      } else {
        break;
      }
    }

    if (lTokenBehavior == SampledTokenBehavior::Thinking) {
      const std::string lSThinkingMarker = GetNormalizedTokenMarkerText(lPtrVocab, liNextToken);
      if (IsThinkingStartTokenText(lSThinkingMarker)) {
        lbThinkingMode = true;
      } else if (IsThinkingEndTokenText(lSThinkingMarker)) {
        lbThinkingMode = false;
      } else {
        lbThinkingMode = true;
      }
    }

    if (lTokenBehavior == SampledTokenBehavior::Emit && !lbThinkingMode) {
      const std::optional<std::string> lOptPiece = TokenToPieceText(lPtrVocab, liNextToken);
      if (!lOptPiece.has_value()) {
        lSendMessageResponse.pSError = "Failed to decode sampled token bytes.";
        std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
        mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Loaded;
        mCurrentStateResponse.pSLoadedModelName = ms_LoadedModelName;
        mCurrentStateResponse.pOptRunningStructure.reset();
        return lSendMessageResponse;
      }
      lSResponseText += *lOptPiece;
    }

    ++liProcessedUnits;
    UpdateGenerationState(lbThinkingMode ? EngineLifecycleState::Thinking : EngineLifecycleState::Running,
                          lbThinkingMode ? "Reasoning about response" : "Generating response", liProcessedUnits);

    llama_token liTokenForDecode = liNextToken;
    llama_batch lNextTokenBatch = llama_batch_get_one(&liTokenForDecode, 1);
    if (llama_decode(mPtrContext, lNextTokenBatch) != 0) {
      lSendMessageResponse.pSError = "llama_decode failed during generation.";
      std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
      mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Loaded;
      mCurrentStateResponse.pSLoadedModelName = ms_LoadedModelName;
      mCurrentStateResponse.pOptRunningStructure.reset();
      return lSendMessageResponse;
    }
  }

  const std::string lSVisibleResponse = TrimAsciiWhitespace(StripReasoningSections(lSResponseText));
  const std::string lSRawTrimmedResponse = TrimAsciiWhitespace(lSResponseText);

  lSendMessageResponse.pbOk = true;
  if (!lSVisibleResponse.empty()) {
    lSendMessageResponse.pSText = lSVisibleResponse;
  } else if (!lSRawTrimmedResponse.empty()) {
    lSendMessageResponse.pSText = lSRawTrimmedResponse;
  } else {
    lSendMessageResponse.pSText = "(no textual response)";
  }
  lSendMessageResponse.pVecfEmbedding = BuildEmbedding(pSPrompt, mEngineOptions.piEmbeddingDimensions);

  {
    std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
    mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Finished;
    mCurrentStateResponse.pSLoadedModelName = ms_LoadedModelName;
    if (mCurrentStateResponse.pOptRunningStructure.has_value()) {
      mCurrentStateResponse.pOptRunningStructure->piProcessedUnits = liProcessedUnits;
      mCurrentStateResponse.pOptRunningStructure->piTotalUnits = liProcessedUnits;
      mCurrentStateResponse.pOptRunningStructure->pSDetail = "Response ready; awaiting fetch";
    }
    mCurrentStateResponse.pOptLoadingStructure.reset();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(6));

  {
    std::lock_guard<std::mutex> lStateGuard(mMutexCurrentState);
    mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Loaded;
    mCurrentStateResponse.pSLoadedModelName = ms_LoadedModelName;
    mCurrentStateResponse.pOptRunningStructure.reset();
    mCurrentStateResponse.pOptLoadingStructure.reset();
  }

  return lSendMessageResponse;
#else
  std::string lSModelNameSnapshot;
  {
    std::lock_guard<std::mutex> lGuard(mMutexCurrentState);
    lSModelNameSnapshot = ms_LoadedModelName;
    if (lSModelNameSnapshot.empty()) {
      lSendMessageResponse.pSError = "No model is loaded.";
      return lSendMessageResponse;
    }
    mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Running;
    mCurrentStateResponse.pOptRunningStructure =
        RunningStructure{lSModelNameSnapshot, 0, pSPrompt.size(), "Running inference"};
    mCurrentStateResponse.pOptLoadingStructure.reset();
  }

  lSendMessageResponse.pbOk = true;
  lSendMessageResponse.pSText = "llama.cpp runtime is not linked in this build.";
  lSendMessageResponse.pVecfEmbedding = BuildEmbedding(pSPrompt, mEngineOptions.piEmbeddingDimensions);

  {
    std::lock_guard<std::mutex> lGuard(mMutexCurrentState);
    mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Finished;
    mCurrentStateResponse.pSLoadedModelName = ms_LoadedModelName;
    if (mCurrentStateResponse.pOptRunningStructure.has_value()) {
      mCurrentStateResponse.pOptRunningStructure->piProcessedUnits =
          std::max<std::size_t>(pSPrompt.size(), static_cast<std::size_t>(1));
      mCurrentStateResponse.pOptRunningStructure->piTotalUnits =
          std::max<std::size_t>(pSPrompt.size(), static_cast<std::size_t>(1));
      mCurrentStateResponse.pOptRunningStructure->pSDetail = "Response ready; awaiting fetch";
    }
    mCurrentStateResponse.pOptLoadingStructure.reset();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(6));

  {
    std::lock_guard<std::mutex> lGuard(mMutexCurrentState);
    mCurrentStateResponse.pEngineLifecycleState = EngineLifecycleState::Loaded;
    mCurrentStateResponse.pSLoadedModelName = ms_LoadedModelName;
    mCurrentStateResponse.pOptRunningStructure.reset();
    mCurrentStateResponse.pOptLoadingStructure.reset();
  }
  return lSendMessageResponse;
#endif
}

/// <summary>Returns a thread-safe snapshot of current engine state.</summary>
/// <returns>Current lifecycle/progress state.</returns>
CurrentStateResponse LocalOllamaCppEngine::QueryCurrentState() const {
  std::lock_guard<std::mutex> lGuard(mMutexCurrentState);
  return mCurrentStateResponse;
}

}  // namespace ollama_engine::internal
