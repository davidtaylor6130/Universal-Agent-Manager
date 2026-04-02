#include "ollama_engine_client.h"

#include <algorithm>
#include <fstream>

OllamaEngineClient::OllamaEngineClient()
{
	options_.pPathModelFolder = std::filesystem::current_path() / "models";
	options_.piEmbeddingDimensions = 256;
	options_.piEmbeddingMaxTokens = 0;
}

void OllamaEngineClient::SetModelFolder(const std::filesystem::path& model_folder)
{
	if (options_.pPathModelFolder == model_folder)
	{
		return;
	}

	options_.pPathModelFolder = model_folder;
	engine_.reset();
}

void OllamaEngineClient::SetEmbeddingDimensions(const std::size_t dimensions)
{
	const std::size_t clamped_dimensions = std::clamp<std::size_t>(dimensions, 32, 4096);

	if (options_.piEmbeddingDimensions == clamped_dimensions)
	{
		return;
	}

	options_.piEmbeddingDimensions = clamped_dimensions;
	engine_.reset();
}

void OllamaEngineClient::SetEmbeddingMaxTokens(const std::size_t max_tokens)
{
	const std::size_t clamped_max_tokens = std::clamp<std::size_t>(max_tokens, 0, 32768);

	if (options_.piEmbeddingMaxTokens == clamped_max_tokens)
	{
		return;
	}

	options_.piEmbeddingMaxTokens = clamped_max_tokens;
	engine_.reset();
}

std::vector<std::string> OllamaEngineClient::ListModels()
{
#if UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
	EnsureEngine();
	return engine_->ListModels();
#else
	std::vector<std::string> models;
	std::error_code ec;

	if (!std::filesystem::exists(options_.pPathModelFolder, ec) || ec)
	{
		return models;
	}

	for (const auto& entry : std::filesystem::directory_iterator(options_.pPathModelFolder, ec))
	{
		if (ec || !entry.is_regular_file())
		{
			continue;
		}

		const std::string ext = entry.path().extension().string();

		if (ext == ".gguf" || ext == ".ggml")
		{
			models.push_back(entry.path().filename().string());
		}
	}

	std::sort(models.begin(), models.end());
	return models;
#endif
}

bool OllamaEngineClient::Load(const std::string& model_name, std::string* error_out)
{
#if UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
	EnsureEngine();
	return engine_->Load(model_name, error_out);
#else
	(void)model_name;

	if (error_out != nullptr)
	{
		*error_out = "Ollama engine runtime is disabled in this build.";
	}

	return false;
#endif
}

ollama_engine::SendMessageResponse OllamaEngineClient::SendMessage(const std::string& prompt)
{
#if UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
	EnsureEngine();
	return engine_->SendMessage(prompt);
#else
	(void)prompt;
	ollama_engine::SendMessageResponse response;
	response.pbOk = false;
	response.pSError = "Ollama engine runtime is disabled in this build.";
	return response;
#endif
}

ollama_engine::CurrentStateResponse OllamaEngineClient::QueryCurrentState() const
{
#if UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
	if (!engine_)
	{
		return {};
	}

	return engine_->QueryCurrentState();
#else
	return {};
#endif
}

bool OllamaEngineClient::Scan(const std::optional<std::string>& vector_file, std::string* error_out)
{
#if UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
	EnsureEngine();
	return engine_->Scan(vector_file, error_out);
#else
	(void)vector_file;

	if (error_out != nullptr)
	{
		*error_out = "Ollama engine runtime is disabled in this build.";
	}

	return false;
#endif
}

bool OllamaEngineClient::SetRagOutputDatabase(const std::string& database_name, std::string* error_out)
{
#if UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
	EnsureEngine();
	return engine_->SetRagOutputDatabase(database_name, error_out);
#else
	(void)database_name;

	if (error_out != nullptr)
	{
		*error_out = "Ollama engine runtime is disabled in this build.";
	}

	return false;
#endif
}

bool OllamaEngineClient::LoadRagDatabases(const std::vector<std::string>& database_inputs, std::string* error_out)
{
#if UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
	EnsureEngine();
	return engine_->LoadRagDatabases(database_inputs, error_out);
#else
	(void)database_inputs;

	if (error_out != nullptr)
	{
		*error_out = "Ollama engine runtime is disabled in this build.";
	}

	return false;
#endif
}

std::vector<std::string> OllamaEngineClient::FetchRelevantInfo(const std::string& prompt, const std::size_t max_items, const std::size_t min_items)
{
#if UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
	EnsureEngine();
	return engine_->Fetch_Relevant_Info(prompt, max_items, min_items);
#else
	(void)prompt;
	(void)max_items;
	(void)min_items;
	return {};
#endif
}

ollama_engine::VectorisationStateResponse OllamaEngineClient::FetchVectorisationState()
{
#if UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
	if (!engine_)
	{
		return {};
	}

	return engine_->Fetch_state();
#else
	return {};
#endif
}

void OllamaEngineClient::EnsureEngine()
{
#if UAM_ENABLE_RUNTIME_OLLAMA_ENGINE
	if (!engine_)
	{
		engine_ = ollama_engine::CreateEngine(options_);
	}
#endif
}
