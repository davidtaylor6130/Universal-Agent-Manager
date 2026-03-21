#pragma once

/**
 * @file engine_structures.h
 * @brief Public state, options, and message structures for the Ollama engine.
 */

#include <cstddef>
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
/// Runtime options used to initialize the engine.
/// </summary>
struct EngineOptions {
  /// <summary>Directory containing local model files.</summary>
  std::filesystem::path pPathModelFolder;

  /// <summary>Embedding vector dimensions.</summary>
  std::size_t piEmbeddingDimensions = 256;
};

}  // namespace ollama_engine
