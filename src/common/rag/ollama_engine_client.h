#pragma once

#include "ollama_engine/engine_api.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

/// <summary>
/// Thin RAII wrapper over the Ollama engine interface.
/// </summary>
class OllamaEngineClient
{
  public:
	/// <summary>Constructs the client with default engine options.</summary>
	OllamaEngineClient();

	/// <summary>Sets the model folder used by the engine.</summary>
	void SetModelFolder(const std::filesystem::path& model_folder);
	/// <summary>Sets embedding dimensions for generated vectors.</summary>
	void SetEmbeddingDimensions(std::size_t dimensions);
	/// <summary>Sets max tokens used per embedding request (0 = engine default).</summary>
	void SetEmbeddingMaxTokens(std::size_t max_tokens);

	/// <summary>Lists available local models.</summary>
	std::vector<std::string> ListModels();
	/// <summary>Loads the selected model into the engine.</summary>
	bool Load(const std::string& model_name, std::string* error_out = nullptr);
	/// <summary>Sends one message to the loaded model.</summary>
	ollama_engine::SendMessageResponse SendMessage(const std::string& prompt);
	/// <summary>Returns current engine state information.</summary>
	ollama_engine::CurrentStateResponse QueryCurrentState() const;
	/// <summary>Starts an asynchronous RAG scan for the given source.</summary>
	bool Scan(const std::optional<std::string>& vector_file, std::string* error_out = nullptr);
	/// <summary>Sets the output database name for future scans.</summary>
	bool SetRagOutputDatabase(const std::string& database_name, std::string* error_out = nullptr);
	/// <summary>Loads one or more RAG databases for retrieval.</summary>
	bool LoadRagDatabases(const std::vector<std::string>& database_inputs, std::string* error_out = nullptr);
	/// <summary>Fetches semantically relevant snippets from the active/loaded database set.</summary>
	std::vector<std::string> FetchRelevantInfo(const std::string& prompt, std::size_t max_items, std::size_t min_items);
	/// <summary>Returns current vectorisation lifecycle/progress state.</summary>
	ollama_engine::VectorisationStateResponse FetchVectorisationState();

  private:
	void EnsureEngine();

	ollama_engine::EngineOptions options_;
	std::unique_ptr<ollama_engine::EngineInterface> engine_;
};
