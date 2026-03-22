#pragma once

/**
 * @file engine_structures.h
 * @brief Public state, options, and message structures for the Ollama engine.
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ollama_engine {

/// <summary>
/// Lifecycle state for the engine runtime.
/// </summary>
enum class EngineLifecycleState {
  Idle,    ///< Engine is initialized but has no loaded model.
  Loaded,  ///< Model is loaded and idle.
  Loading, ///< Model load operation is in progress.
  Thinking, ///< Model is producing hidden reasoning/thinking tokens.
  Running,  ///< Prompt processing is in progress.
  Finished ///< Prompt processing has finished.
};

/// <summary>
/// Progress data returned while loading a model.
/// </summary>
struct LoadingStructure {
  /// <summary>Active model name.</summary>
  std::string pSModelName;

  /// <summary>Loaded unit count.</summary>
  std::size_t piLoadedUnits = 0;

  /// <summary>Total unit count.</summary>
  std::size_t piTotalUnits = 0;

  /// <summary>Human-readable load status detail.</summary>
  std::string pSDetail;
};

/// <summary>
/// Progress data returned while processing a prompt.
/// </summary>
struct RunningStructure {
  /// <summary>Active model name.</summary>
  std::string pSModelName;

  /// <summary>Processed unit count.</summary>
  std::size_t piProcessedUnits = 0;

  /// <summary>Total unit count.</summary>
  std::size_t piTotalUnits = 0;

  /// <summary>Human-readable run status detail.</summary>
  std::string pSDetail;
};

/// <summary>
/// Snapshot of the engine lifecycle and progress state.
/// </summary>
struct CurrentStateResponse {
  /// <summary>Current lifecycle state.</summary>
  EngineLifecycleState pEngineLifecycleState = EngineLifecycleState::Idle;

  /// <summary>Most recently loaded model name.</summary>
  std::string pSLoadedModelName;

  /// <summary>Loading progress data, if available.</summary>
  std::optional<LoadingStructure> pOptLoadingStructure;

  /// <summary>Running progress data, if available.</summary>
  std::optional<RunningStructure> pOptRunningStructure;
};

/// <summary>
/// Lifecycle state for internal vectorised RAG scanning.
/// </summary>
enum class VectorisationLifecycleState {
  Stopped,  ///< Nothing is currently happening.
  Running,  ///< Repository scan/vectorisation is in progress.
  Finished  ///< Scan just finished (reported once).
};

/// <summary>
/// Selects which internal RAG runtime implementation to use.
/// </summary>
enum class RagRuntimeMode {
  Vectorised,    ///< llama.cpp embedding-based retrieval.
  Deterministic  ///< deterministic hash-embedding retrieval.
};

/// <summary>
/// Snapshot for vectorised RAG progress polling.
/// </summary>
struct VectorisationStateResponse {
  /// <summary>Current vectorisation lifecycle state.</summary>
  VectorisationLifecycleState pVectorisationLifecycleState = VectorisationLifecycleState::Stopped;

  /// <summary>Current vector database size in indexed chunks.</summary>
  std::size_t piVectorDatabaseSize = 0;

  /// <summary>Files processed in the current run.</summary>
  std::size_t piFilesProcessed = 0;

  /// <summary>Total files targeted in the current run.</summary>
  std::size_t piTotalFiles = 0;
};

/// <summary>
/// Result payload from a prompt request.
/// </summary>
struct SendMessageResponse {
  /// <summary>True when processing succeeded.</summary>
  bool pbOk = false;

  /// <summary>Model output text.</summary>
  std::string pSText;

  /// <summary>Optional embedding vector generated for the prompt.</summary>
  std::optional<std::vector<float>> pVecfEmbedding;

  /// <summary>Error text when processing fails.</summary>
  std::string pSError;
};

/// <summary>
/// Common generation/sampling settings used by local inference.
/// </summary>
struct GenerationSettings {
  /// <summary>Sampling temperature. Lower is more deterministic.</summary>
  float pfTemperature = 0.8f;

  /// <summary>Top-p nucleus sampling threshold.</summary>
  float pfTopP = 0.95f;

  /// <summary>Minimum probability floor for candidate tokens.</summary>
  float pfMinP = 0.05f;

  /// <summary>Top-k candidate count cap.</summary>
  int piTopK = 40;

  /// <summary>Repeat penalty to reduce loops/repetition.</summary>
  float pfRepeatPenalty = 1.0f;

  /// <summary>Random seed. 4294967295 means default/random behavior.</summary>
  std::uint32_t piSeed = 4294967295U;
};

/// <summary>
/// Runtime options used to initialize the engine.
/// </summary>
struct EngineOptions {
  /// <summary>Directory containing local model files.</summary>
  std::filesystem::path pPathModelFolder;

  /// <summary>Embedding vector dimensions.</summary>
  std::size_t piEmbeddingDimensions = 256;

  /// <summary>
  /// Maximum tokens used for local embedding generation.
  /// 0 uses engine defaults.
  /// </summary>
  std::size_t piEmbeddingMaxTokens = 0;

  /// <summary>Generation/sampling settings for prompt responses.</summary>
  GenerationSettings pGenerationSettings;

  /// <summary>Internal RAG runtime implementation mode.</summary>
  RagRuntimeMode pRagRuntimeMode = RagRuntimeMode::Vectorised;
};

}  // namespace ollama_engine
