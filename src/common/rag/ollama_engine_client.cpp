#include "ollama_engine_client.h"

#include <algorithm>

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
	EnsureEngine();
	return engine_->ListModels();
}

bool OllamaEngineClient::Load(const std::string& model_name, std::string* error_out)
{
	EnsureEngine();
	return engine_->Load(model_name, error_out);
}

ollama_engine::SendMessageResponse OllamaEngineClient::SendMessage(const std::string& prompt)
{
	EnsureEngine();
	return engine_->SendMessage(prompt);
}

ollama_engine::CurrentStateResponse OllamaEngineClient::QueryCurrentState() const
{
	if (!engine_)
	{
		return {};
	}

	return engine_->QueryCurrentState();
}

bool OllamaEngineClient::Scan(const std::optional<std::string>& vector_file, std::string* error_out)
{
	EnsureEngine();
	return engine_->Scan(vector_file, error_out);
}

bool OllamaEngineClient::SetRagOutputDatabase(const std::string& database_name, std::string* error_out)
{
	EnsureEngine();
	return engine_->SetRagOutputDatabase(database_name, error_out);
}

bool OllamaEngineClient::LoadRagDatabases(const std::vector<std::string>& database_inputs, std::string* error_out)
{
	EnsureEngine();
	return engine_->LoadRagDatabases(database_inputs, error_out);
}

std::vector<std::string> OllamaEngineClient::FetchRelevantInfo(const std::string& prompt, const std::size_t max_items, const std::size_t min_items)
{
	EnsureEngine();
	return engine_->Fetch_Relevant_Info(prompt, max_items, min_items);
}

ollama_engine::VectorisationStateResponse OllamaEngineClient::FetchVectorisationState()
{
	if (!engine_)
	{
		return {};
	}

	return engine_->Fetch_state();
}

void OllamaEngineClient::EnsureEngine()
{
	if (!engine_)
	{
		engine_ = ollama_engine::CreateEngine(options_);
	}
}
