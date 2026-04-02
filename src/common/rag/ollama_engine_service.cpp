#include "ollama_engine_service.h"

OllamaEngineService& OllamaEngineService::Instance()
{
	static OllamaEngineService service;
	return service;
}

OllamaEngineClient& OllamaEngineService::Client()
{
	return client_;
}
