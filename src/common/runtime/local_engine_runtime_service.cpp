#include "local_engine_runtime_service.h"

#include "common/ollama_engine_service.h"

namespace
{

	std::string TrimLocal(const std::string& value)
	{
		const auto start = value.find_first_not_of(" \t\r\n");

		if (start == std::string::npos)
		{
			return "";
		}

		const auto end = value.find_last_not_of(" \t\r\n");
		return value.substr(start, end - start + 1);
	}

	OllamaEngineClient& SharedClient()
	{
		return OllamaEngineService::Instance().Client();
	}

	void ConfigureClient(const std::filesystem::path& model_folder)
	{
		OllamaEngineClient& engine = SharedClient();
		engine.SetModelFolder(model_folder);
		engine.SetEmbeddingDimensions(256);
	}

} // namespace

std::vector<std::string> LocalEngineRuntimeService::ListModels(const std::filesystem::path& model_folder) const
{
	ConfigureClient(model_folder);
	return SharedClient().ListModels();
}

bool LocalEngineRuntimeService::LoadModelIfNeeded(const std::filesystem::path& model_folder,
                                                  const std::string& selected_model_id,
                                                  std::string& loaded_model_id_in_out,
                                                  std::string* error_out) const
{
	ConfigureClient(model_folder);
	const std::string selected = TrimLocal(selected_model_id);

	if (selected.empty() || loaded_model_id_in_out == selected)
	{
		return true;
	}

	if (!SharedClient().Load(selected, error_out))
	{
		return false;
	}

	loaded_model_id_in_out = selected;
	return true;
}

LocalEngineResponse LocalEngineRuntimeService::SendPrompt(const std::filesystem::path& model_folder, const std::string& prompt) const
{
	ConfigureClient(model_folder);
	const ollama_engine::SendMessageResponse response = SharedClient().SendMessage(prompt);

	LocalEngineResponse out;
	out.ok = response.pbOk;
	out.text = response.pSText;
	out.error = response.pSError;
	return out;
}
