#pragma once

#include "ollama_engine/engine_api.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

/// <summary>
/// Thin RAII wrapper over the Ollama engine interface.
/// </summary>
class OllamaEngineClient {
 public:
  /// <summary>Constructs the client with default engine options.</summary>
  OllamaEngineClient();

  /// <summary>Sets the model folder used by the engine.</summary>
  void SetModelFolder(const std::filesystem::path& model_folder);
  /// <summary>Sets embedding dimensions for generated vectors.</summary>
  void SetEmbeddingDimensions(std::size_t dimensions);

  /// <summary>Lists available local models.</summary>
  std::vector<std::string> ListModels();
  /// <summary>Loads the selected model into the engine.</summary>
  bool Load(const std::string& model_name, std::string* error_out = nullptr);
  /// <summary>Sends one message to the loaded model.</summary>
  ollama_engine::SendMessageResponse SendMessage(const std::string& prompt);
  /// <summary>Returns current engine state information.</summary>
  ollama_engine::CurrentStateResponse QueryCurrentState() const;

 private:
  void EnsureEngine();

  ollama_engine::EngineOptions options_;
  std::unique_ptr<ollama_engine::EngineInterface> engine_;
};
