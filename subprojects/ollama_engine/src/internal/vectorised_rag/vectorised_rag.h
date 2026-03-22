#pragma once

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
struct llama_model;
struct llama_context;
#endif

namespace ollama_engine::internal::vectorised_rag {

enum class StateValue {
  Stopped,
  Running,
  Finished,
};

struct ScanStateSnapshot {
  StateValue pState = StateValue::Stopped;
  std::size_t piVectorDatabaseSize = 0;
  std::size_t piFilesProcessed = 0;
  std::size_t piTotalFiles = 0;
};

struct RuntimeOptions {
  std::filesystem::path pPathModelFolder;
  std::filesystem::path pPathEmbeddingModelFile;
  std::string pSLlamaServerUrl = "http://127.0.0.1:8080";
  std::string pSStorageFolderName = ".vectorised_rag";
  std::string pSDatabaseName;
  std::size_t piChunkCharLimit = 4000;
  bool pbUseDeterministicEmbeddings = false;
  std::size_t piDeterministicEmbeddingDimensions = 256;
  std::size_t piEmbeddingMaxTokens = 0;
};

struct Context {
  std::mutex pMutex;
  std::thread pScanThread;
  bool pbThreadRunning = false;
  bool pbFinishedPending = false;
  bool pbLlamaServerChecked = false;
  bool pbLlamaServerAvailable = false;
  std::string pSCheckedLlamaServerUrl;
  std::string pSLastVectorFileInput;
  std::string pSActiveSourceId;
  std::filesystem::path pPathSourceRoot;
  std::filesystem::path pPathVectorDatabaseFile;
  std::vector<std::filesystem::path> pVecPathLoadedDatabases;
  std::string pSError;
  ScanStateSnapshot pScanState;
#ifdef UAM_OLLAMA_ENGINE_WITH_LLAMA_CPP
  std::filesystem::path pPathLoadedEmbeddingModel;
  std::size_t piLoadedEmbeddingMaxTokens = 0;
  llama_model* pPtrEmbeddingModel = nullptr;
  llama_context* pPtrEmbeddingContext = nullptr;
#endif
};

/// <summary>Stops worker thread and releases runtime resources.</summary>
/// <param name="pContext">RAG context to shut down.</param>
void Shutdown(Context& pContext);

/// <summary>Starts asynchronous scan/index run for a source repository.</summary>
/// <param name="pContext">RAG context.</param>
/// <param name="pOptSVectorFile">Optional source spec (local path or remote).</param>
/// <param name="pRuntimeOptions">Runtime options.</param>
/// <param name="pSErrorOut">Optional output pointer for errors.</param>
/// <returns>True when scan worker was started.</returns>
bool Scan(Context& pContext,
          const std::optional<std::string>& pOptSVectorFile,
          const RuntimeOptions& pRuntimeOptions,
          std::string* pSErrorOut = nullptr);

/// <summary>Loads an explicit set of local RAG databases to query.</summary>
/// <param name="pContext">RAG context.</param>
/// <param name="pVecSDatabaseInputs">
/// Database selectors: sqlite file paths, directories, or storage-folder logical names.
/// </param>
/// <param name="pRuntimeOptions">Runtime options.</param>
/// <param name="pSErrorOut">Optional output pointer for errors.</param>
/// <returns>True when all requested inputs resolve to queryable sqlite files.</returns>
bool LoadRagDatabases(Context& pContext,
                      const std::vector<std::string>& pVecSDatabaseInputs,
                      const RuntimeOptions& pRuntimeOptions,
                      std::string* pSErrorOut = nullptr);

/// <summary>Retrieves relevant snippets for a prompt from the indexed source.</summary>
/// <param name="pContext">RAG context.</param>
/// <param name="pSPrompt">Prompt/query text.</param>
/// <param name="piMax">Maximum snippet count.</param>
/// <param name="piMin">Minimum snippet count.</param>
/// <param name="pRuntimeOptions">Runtime options.</param>
/// <returns>Ranked snippets.</returns>
std::vector<std::string> Fetch_Relevant_Info(Context& pContext,
                                             const std::string& pSPrompt,
                                             std::size_t piMax,
                                             std::size_t piMin,
                                             const RuntimeOptions& pRuntimeOptions);

/// <summary>Returns current scan state and progress snapshot.</summary>
/// <param name="pContext">RAG context.</param>
/// <returns>Current state snapshot.</returns>
ScanStateSnapshot Fetch_state(Context& pContext);

}  // namespace ollama_engine::internal::vectorised_rag
