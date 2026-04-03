#pragma once

#include "common/rag/ollama_engine_client.h"

/// <summary>
/// Process-local singleton for the shared Ollama engine client.
/// </summary>
class OllamaEngineService
{
  public:
	static OllamaEngineService& Instance();
	OllamaEngineClient& Client();

  private:
	OllamaEngineService() = default;
	OllamaEngineClient client_;
};
