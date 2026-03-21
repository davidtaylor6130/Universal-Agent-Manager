#include "ollama_engine_client.h"

#include <algorithm>

OllamaEngineClient::OllamaEngineClient() {
  options_.pPathModelFolder = std::filesystem::current_path() / "models";
  options_.piEmbeddingDimensions = 256;
}

void OllamaEngineClient::SetModelFolder(const std::filesystem::path& model_folder) {
  options_.pPathModelFolder = model_folder;
  engine_.reset();
}

void OllamaEngineClient::SetEmbeddingDimensions(const std::size_t dimensions) {
  options_.piEmbeddingDimensions = std::clamp<std::size_t>(dimensions, 32, 4096);
  engine_.reset();
}

std::vector<std::string> OllamaEngineClient::ListModels() {
  EnsureEngine();
  return engine_->ListModels();
}

bool OllamaEngineClient::Load(const std::string& model_name, std::string* error_out) {
  EnsureEngine();
  return engine_->Load(model_name, error_out);
}

ollama_engine::SendMessageResponse OllamaEngineClient::SendMessage(const std::string& prompt) {
  EnsureEngine();
  return engine_->SendMessage(prompt);
}

ollama_engine::CurrentStateResponse OllamaEngineClient::QueryCurrentState() const {
  if (!engine_) {
    return {};
  }
  return engine_->QueryCurrentState();
}

void OllamaEngineClient::EnsureEngine() {
  if (!engine_) {
    engine_ = ollama_engine::CreateEngine(options_);
  }
}
