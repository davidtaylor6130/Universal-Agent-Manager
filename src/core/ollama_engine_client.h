#pragma once

#include "ollama_engine/engine_api.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class OllamaEngineClient {
 public:
  OllamaEngineClient();

  void SetModelFolder(const std::filesystem::path& model_folder);
  void SetEmbeddingDimensions(std::size_t dimensions);

  std::vector<std::string> ListModels();
  bool Load(const std::string& model_name, std::string* error_out = nullptr);
  ollama_engine::SendMessageResponse SendMessage(const std::string& prompt);
  ollama_engine::CurrentStateResponse QueryCurrentState() const;

 private:
  void EnsureEngine();

  ollama_engine::EngineOptions options_;
  std::unique_ptr<ollama_engine::EngineInterface> engine_;
};
