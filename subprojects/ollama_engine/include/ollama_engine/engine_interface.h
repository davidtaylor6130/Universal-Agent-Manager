#pragma once

/// <summary>Public abstraction for Ollama-compatible engine implementations.</summary>

#include "ollama_engine/engine_structures.h"

#include <string>
#include <vector>

namespace ollama_engine {

/// <summary>
/// Contract implemented by concrete engine backends.
/// </summary>
class EngineInterface {
 public:
  /// <summary>Virtual destructor for interface cleanup.</summary>
  virtual ~EngineInterface() = default;

  /// <summary>Enumerates model files available to the engine.</summary>
  /// <returns>Sorted model file names.</returns>
  virtual std::vector<std::string> ListModels() = 0;

  /// <summary>Loads a model by file name.</summary>
  /// <param name="pSModelName">File name in the configured model folder.</param>
  /// <param name="pSErrorOut">Optional output pointer for human-readable errors.</param>
  /// <returns>True if loading completed successfully.</returns>
  virtual bool Load(const std::string& pSModelName, std::string* pSErrorOut = nullptr) = 0;

  /// <summary>Sends a prompt to the currently loaded model.</summary>
  /// <param name="pSPrompt">Prompt text.</param>
  /// <returns>Response payload with status, text, and embedding.</returns>
  virtual SendMessageResponse SendMessage(const std::string& pSPrompt) = 0;

  /// <summary>Reads the latest state snapshot for lifecycle and progress polling.</summary>
  /// <returns>Current state response.</returns>
  virtual CurrentStateResponse QueryCurrentState() const = 0;
};

}  // namespace ollama_engine
